#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_should_stop = 0;
static volatile sig_atomic_t g_should_pause = 0;
static char g_workspace[512] = {0};

static int copy_file(const char *src, const char *dst);

static void write_json_string(FILE *fp, const char *value) {
    fputc('"', fp);
    if (value != NULL) {
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
            switch (*p) {
                case '\\': fputs("\\\\", fp); break;
                case '"': fputs("\\\"", fp); break;
                case '\n': fputs("\\n", fp); break;
                case '\r': fputs("\\r", fp); break;
                case '\t': fputs("\\t", fp); break;
                default: fputc(*p, fp); break;
            }
        }
    }
    fputc('"', fp);
}

static void now_iso(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm tm_value;
    localtime_r(&now, &tm_value);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &tm_value);
}

static void append_event(const char *component, const char *event, const char *details) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/logs/aura_events.log", g_workspace);

    int lock_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (lock_fd < 0) {
        return;
    }

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    fcntl(lock_fd, F_SETLKW, &lock);

    FILE *fp = fopen(path, "a");
    if (fp != NULL) {
        char ts[64];
        now_iso(ts, sizeof(ts));
        fprintf(fp, "{\"timestamp\":");
        write_json_string(fp, ts);
        fprintf(fp, ",\"component\":");
        write_json_string(fp, component);
        fprintf(fp, ",\"event\":");
        write_json_string(fp, event);
        if (details != NULL) {
            fprintf(fp, ",\"details\":");
            write_json_string(fp, details);
        }
        fprintf(fp, "}\n");
        fclose(fp);
    }

    lock.l_type = F_UNLCK;
    fcntl(lock_fd, F_SETLKW, &lock);
    close(lock_fd);
}

static void update_runtime_state(const char *phase, const char *component, const char *detail) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/runtime_state.json", g_workspace);

    char metadata_path[1024];
    snprintf(metadata_path, sizeof(metadata_path), "%s/agent_state.txt", g_workspace);

    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"systemOnline\": true,\n");
    fprintf(fp, "  \"activeComponents\": 6,\n");
    fprintf(fp, "  \"phase\": ");
    write_json_string(fp, phase);
    fprintf(fp, ",\n  \"latestComponent\": ");
    write_json_string(fp, component);
    fprintf(fp, ",\n  \"detail\": ");
    write_json_string(fp, detail);
    fprintf(fp, ",\n  \"controllers\": [\n    {\"name\": \"Orchestrator\", \"pid\": %d, \"status\": \"RUNNING\"},\n    {\"name\": \"Monitor\", \"pid\": %d, \"status\": \"ONLINE\"}\n  ],\n", getpid(), getpid());
    fprintf(fp, "  \"workers\": [\n    {\"name\": \"Generator\", \"role\": \"Worker Agent\"},\n    {\"name\": \"Executor\", \"role\": \"Worker Agent\"},\n    {\"name\": \"Reviewer\", \"role\": \"Worker Agent\"},\n    {\"name\": \"Tester\", \"role\": \"Worker Agent\"}\n  ],\n");
    fprintf(fp, "  \"lastUpdated\": ");
    char ts[64];
    now_iso(ts, sizeof(ts));
    write_json_string(fp, ts);
    fprintf(fp, ",\n  \"workspace\": ");
    write_json_string(fp, g_workspace);
    fprintf(fp, ",\n  \"metadataFile\": ");
    write_json_string(fp, metadata_path);
    fprintf(fp, "\n}\n");

    fclose(fp);
}

static void ensure_directory(const char *path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        perror("mkdir");
        exit(1);
    }
}

static void create_fifo_if_needed(const char *path) {
    if (mkfifo(path, 0666) != 0 && errno != EEXIST) {
        perror("mkfifo");
        exit(1);
    }
}

static void create_runtime_layout(void) {
    ensure_directory(g_workspace);

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/logs", g_workspace);
    ensure_directory(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s/tasks", g_workspace);
    ensure_directory(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s/backups", g_workspace);
    ensure_directory(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s/fifos", g_workspace);
    ensure_directory(dir_path);

    char fifo_path[1024];
    snprintf(fifo_path, sizeof(fifo_path), "%s/fifos/orchestrator_to_generator.fifo", g_workspace);
    create_fifo_if_needed(fifo_path);
    snprintf(fifo_path, sizeof(fifo_path), "%s/fifos/generator_to_executor.fifo", g_workspace);
    create_fifo_if_needed(fifo_path);
    snprintf(fifo_path, sizeof(fifo_path), "%s/fifos/executor_to_reviewer.fifo", g_workspace);
    create_fifo_if_needed(fifo_path);
    snprintf(fifo_path, sizeof(fifo_path), "%s/fifos/reviewer_to_tester.fifo", g_workspace);
    create_fifo_if_needed(fifo_path);
    snprintf(fifo_path, sizeof(fifo_path), "%s/fifos/tester_to_monitor.fifo", g_workspace);
    create_fifo_if_needed(fifo_path);
}

static void write_agent_registry(pid_t *pids, size_t count) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/agent_state.txt", g_workspace);
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        fprintf(fp, "%d\n", (int)pids[i]);
    }
    fclose(fp);
}

static void run_command_capture(const char *command, char *buffer, size_t buffer_size) {
    memset(buffer, 0, buffer_size);
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        snprintf(buffer, buffer_size, "Command failed: %s", command);
        return;
    }
    size_t length = 0;
    while (fgets(buffer + length, (int)(buffer_size - length), pipe) != NULL) {
        length = strlen(buffer);
        if (length + 1 >= buffer_size) {
            break;
        }
    }
    pclose(pipe);
    if (length > 0 && buffer[length - 1] == '\n') {
        buffer[length - 1] = '\0';
    }
}

static char *trim_newline(char *value) {
    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r')) {
        value[--length] = '\0';
    }
    return value;
}

static void signal_handler(int signal_number) {
    if (signal_number == SIGUSR1) {
        g_should_pause = 1;
    } else if (signal_number == SIGTERM || signal_number == SIGINT) {
        g_should_stop = 1;
    }
}

static void generator_role(const char *workspace) {
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    char fifo_in[1024];
    snprintf(fifo_in, sizeof(fifo_in), "%s/fifos/orchestrator_to_generator.fifo", workspace);
    int in_fd = open(fifo_in, O_RDONLY);
    if (in_fd < 0) {
        perror("open generator fifo");
        exit(1);
    }

    char message[512] = {0};
    ssize_t bytes_read = read(in_fd, message, sizeof(message) - 1);
    if (bytes_read > 0) {
        message[bytes_read] = '\0';
        trim_newline(message);
        char task_file[1024];
        snprintf(task_file, sizeof(task_file), "%s/tasks/generated_task.txt", workspace);
        FILE *task_fp = fopen(task_file, "w");
        if (task_fp != NULL) {
            fprintf(task_fp, "factorial_of_10\n");
            fclose(task_fp);
        }
        append_event("Generator", "task_generated", message);
        update_runtime_state("GENERATE", "Generator", message);

        char fifo_out[1024];
        snprintf(fifo_out, sizeof(fifo_out), "%s/fifos/generator_to_executor.fifo", workspace);
        int out_fd = open(fifo_out, O_WRONLY);
        if (out_fd >= 0) {
            char payload[256];
            snprintf(payload, sizeof(payload), "Generate factorial task for PID %d", (int)getpid());
            write(out_fd, payload, strlen(payload));
            close(out_fd);
        }
    }
    close(in_fd);
    _exit(0);
}

static void executor_role(const char *workspace) {
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    char fifo_in[1024];
    snprintf(fifo_in, sizeof(fifo_in), "%s/fifos/generator_to_executor.fifo", workspace);
    int in_fd = open(fifo_in, O_RDONLY);
    if (in_fd < 0) {
        perror("open executor fifo");
        exit(1);
    }

    char message[512] = {0};
    ssize_t bytes_read = read(in_fd, message, sizeof(message) - 1);
    if (bytes_read > 0) {
        message[bytes_read] = '\0';
        trim_newline(message);
        char output_path[1024];
        snprintf(output_path, sizeof(output_path), "%s/tasks/execution_output.txt", workspace);

        char command[512];
        snprintf(command, sizeof(command), "python3 -c \"import math; print(math.factorial(10))\"");
        char result[512];
        run_command_capture(command, result, sizeof(result));

        char lock_path[1024];
        snprintf(lock_path, sizeof(lock_path), "%s/locks_runtime", workspace);
        int lock_fd = open(lock_path, O_WRONLY | O_CREAT, 0644);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_EX);
            FILE *fp = fopen(output_path, "w");
            if (fp != NULL) {
                fprintf(fp, "%s\n", result);
                fclose(fp);
            }
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }

        append_event("Executor", "execution_complete", result);
        update_runtime_state("EXECUTE", "Executor", result);

        char fifo_out[1024];
        snprintf(fifo_out, sizeof(fifo_out), "%s/fifos/executor_to_reviewer.fifo", workspace);
        int out_fd = open(fifo_out, O_WRONLY);
        if (out_fd >= 0) {
            write(out_fd, result, strlen(result));
            close(out_fd);
        }
    }
    close(in_fd);
    _exit(0);
}

static void reviewer_role(const char *workspace) {
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    char fifo_in[1024];
    snprintf(fifo_in, sizeof(fifo_in), "%s/fifos/executor_to_reviewer.fifo", workspace);
    int in_fd = open(fifo_in, O_RDONLY);
    if (in_fd < 0) {
        perror("open reviewer fifo");
        exit(1);
    }

    char message[512] = {0};
    ssize_t bytes_read = read(in_fd, message, sizeof(message) - 1);
    if (bytes_read > 0) {
        message[bytes_read] = '\0';
        trim_newline(message);

        char decision[64];
        snprintf(decision, sizeof(decision), "%s", strcmp(message, "3628800") == 0 ? "APPROVED" : "REJECTED");

        char review_path[1024];
        snprintf(review_path, sizeof(review_path), "%s/tasks/review_decision.txt", workspace);
        FILE *review_file = fopen(review_path, "w");
        if (review_file != NULL) {
            fprintf(review_file, "%s\n", decision);
            fclose(review_file);
        }
        append_event("Reviewer", "decision_made", decision);
        update_runtime_state("REVIEW", "Reviewer", decision);

        char fifo_out[1024];
        snprintf(fifo_out, sizeof(fifo_out), "%s/fifos/reviewer_to_tester.fifo", workspace);
        int out_fd = open(fifo_out, O_WRONLY);
        if (out_fd >= 0) {
            write(out_fd, decision, strlen(decision));
            close(out_fd);
        }
    }
    close(in_fd);
    _exit(0);
}

static void tester_role(const char *workspace) {
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    char fifo_in[1024];
    snprintf(fifo_in, sizeof(fifo_in), "%s/fifos/reviewer_to_tester.fifo", workspace);
    int in_fd = open(fifo_in, O_RDONLY);
    if (in_fd < 0) {
        perror("open tester fifo");
        exit(1);
    }

    char message[512] = {0};
    ssize_t bytes_read = read(in_fd, message, sizeof(message) - 1);
    if (bytes_read > 0) {
        message[bytes_read] = '\0';
        trim_newline(message);

        char verification[256];
        snprintf(verification, sizeof(verification), "%s", strcmp(message, "APPROVED") == 0 ? "PASS" : "FAIL");

        char output_path[1024];
        snprintf(output_path, sizeof(output_path), "%s/tasks/test_verification.txt", workspace);
        FILE *fp = fopen(output_path, "w");
        if (fp != NULL) {
            fprintf(fp, "%s\n", verification);
            fclose(fp);
        }

        append_event("Tester", "verification_done", verification);
        update_runtime_state("TEST", "Tester", verification);

        char monitor_fifo[1024];
        snprintf(monitor_fifo, sizeof(monitor_fifo), "%s/fifos/tester_to_monitor.fifo", workspace);
        int out_fd = open(monitor_fifo, O_WRONLY);
        if (out_fd >= 0) {
            write(out_fd, verification, strlen(verification));
            close(out_fd);
        }
    }
    close(in_fd);
    _exit(0);
}

static void monitor_role(const char *workspace) {
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    char fifo_in[1024];
    snprintf(fifo_in, sizeof(fifo_in), "%s/fifos/tester_to_monitor.fifo", workspace);
    int in_fd = open(fifo_in, O_RDONLY | O_NONBLOCK);
    if (in_fd < 0) {
        perror("open monitor fifo");
        exit(1);
    }

    while (!g_should_stop) {
        char status_line[256] = {0};
        ssize_t bytes_read = read(in_fd, status_line, sizeof(status_line) - 1);
        if (bytes_read > 0) {
            status_line[bytes_read] = '\0';
            trim_newline(status_line);
            append_event("Monitor", "status_update", status_line);
        }
        char state_path[1024];
        snprintf(state_path, sizeof(state_path), "%s/runtime_state.json", workspace);
        update_runtime_state("MONITOR", "Monitor", "Process state synchronized");
        if (access(state_path, F_OK) == 0) {
            char backup_path[1024];
            snprintf(backup_path, sizeof(backup_path), "%s/backups/backup_%ld.json", workspace, time(NULL));
            if (copy_file(state_path, backup_path) == 0) {
                append_event("Monitor", "backup_created", backup_path);
            }
        }
        if (g_should_pause) {
            append_event("Monitor", "paused", "Execution paused by signal");
            while (g_should_pause && !g_should_stop) {
                usleep(200000);
            }
        }
        usleep(500000);
    }
    close(in_fd);
    _exit(0);
}

static int copy_file(const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }
    char buffer[4096];
    ssize_t count;
    while ((count = read(in_fd, buffer, sizeof(buffer))) > 0) {
        write(out_fd, buffer, (size_t)count);
    }
    close(in_fd);
    close(out_fd);
    return 0;
}

static void orchestrator_main(const char *workspace) {
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    create_runtime_layout();
    append_event("Orchestrator", "system_boot", "AURA runtime initialized");
    update_runtime_state("ORCHESTRATE", "Orchestrator", "Booting workers");

    pid_t children[5] = {0};
    const char *binary_path = "./build/aura_runtime";

    pid_t generator_pid = fork();
    if (generator_pid == 0) {
        execl(binary_path, binary_path, "generator", workspace, (char *)NULL);
        perror("execl generator");
        _exit(1);
    }
    children[0] = generator_pid;

    pid_t executor_pid = fork();
    if (executor_pid == 0) {
        execl(binary_path, binary_path, "executor", workspace, (char *)NULL);
        perror("execl executor");
        _exit(1);
    }
    children[1] = executor_pid;

    pid_t reviewer_pid = fork();
    if (reviewer_pid == 0) {
        execl(binary_path, binary_path, "reviewer", workspace, (char *)NULL);
        perror("execl reviewer");
        _exit(1);
    }
    children[2] = reviewer_pid;

    pid_t tester_pid = fork();
    if (tester_pid == 0) {
        execl(binary_path, binary_path, "tester", workspace, (char *)NULL);
        perror("execl tester");
        _exit(1);
    }
    children[3] = tester_pid;

    pid_t monitor_pid = fork();
    if (monitor_pid == 0) {
        execl(binary_path, binary_path, "monitor", workspace, (char *)NULL);
        perror("execl monitor");
        _exit(1);
    }
    children[4] = monitor_pid;

    write_agent_registry(children, 5);

    char route_message[256];
    snprintf(route_message, sizeof(route_message), "Route to Generator PID %d", (int)generator_pid);
    append_event("Orchestrator", "route_task", route_message);
    update_runtime_state("ORCHESTRATE", "Orchestrator", route_message);

    char orchestrator_fifo[1024];
    snprintf(orchestrator_fifo, sizeof(orchestrator_fifo), "%s/fifos/orchestrator_to_generator.fifo", workspace);
    int out_fd = open(orchestrator_fifo, O_WRONLY);
    if (out_fd >= 0) {
        write(out_fd, route_message, strlen(route_message));
        close(out_fd);
    }

    while (!g_should_stop) {
        if (g_should_pause) {
            append_event("Orchestrator", "paused", "Execution paused safely");
            update_runtime_state("PAUSE", "Orchestrator", "Execution paused safely");
            while (g_should_pause && !g_should_stop) {
                usleep(200000);
            }
            append_event("Orchestrator", "resumed", "Execution resumed");
            update_runtime_state("RESUME", "Orchestrator", "Execution resumed");
        }

        int status = 0;
        pid_t waited = waitpid(-1, &status, WNOHANG);
        if (waited > 0) {
            append_event("Orchestrator", "worker_exited", "A worker completed its stage");
            update_runtime_state("COMPLETE", "Orchestrator", "Worker completed");
            break;
        }
        usleep(200000);
    }

    if (g_should_stop) {
        append_event("Orchestrator", "stopped", "AURA stopped gracefully");
        update_runtime_state("STOP", "Orchestrator", "Graceful stop complete");
    }

    for (size_t i = 0; i < 5; ++i) {
        if (children[i] > 0) {
            kill(children[i], SIGTERM);
            waitpid(children[i], NULL, 0);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <generator|executor|reviewer|tester|monitor|orchestrator> <workspace>\n", argv[0]);
        return 1;
    }

    const char *role = argv[1];
    const char *workspace = argv[2];

    if (strcmp(role, "generator") == 0) {
        generator_role(workspace);
    } else if (strcmp(role, "executor") == 0) {
        executor_role(workspace);
    } else if (strcmp(role, "reviewer") == 0) {
        reviewer_role(workspace);
    } else if (strcmp(role, "tester") == 0) {
        tester_role(workspace);
    } else if (strcmp(role, "monitor") == 0) {
        monitor_role(workspace);
    } else if (strcmp(role, "orchestrator") == 0) {
        orchestrator_main(workspace);
    } else {
        fprintf(stderr, "Unknown role: %s\n", role);
        return 1;
    }

    return 0;
}
