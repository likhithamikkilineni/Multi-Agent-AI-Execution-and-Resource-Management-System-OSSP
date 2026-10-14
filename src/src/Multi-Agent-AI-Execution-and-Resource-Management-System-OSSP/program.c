#include <stdio.h>
#include <unistd.h>          // fork(), access(), read(), write(), sleep(), execl()
#include <sys/types.h>       // pid_t
#include <sys/wait.h>        // waitpid(), WIFEXITED(), WEXITSTATUS()
#include <stdlib.h>          // exit(), atol()
#include <fcntl.h>           // open()
#include <string.h>          // strcpy(), memset(), strlen(), strstr(), strtok()
#include <signal.h>          // signal(), SIGTERM, kill()
#include <sys/file.h>        // flock()
#include <limits.h>          // PATH_MAX
#include <errno.h>


/* ============================================================
   FIFO NAMES
   Named pipes are used for communication between agents.
   ============================================================ */

#define FIFO_GENERATOR "fifo/generator_to_executor"
#define FIFO_REVIEWER "fifo/executor_to_reviewer"
#define FIFO_TESTER "fifo/reviewer_to_tester"


/* ============================================================
   RESTRICTED WORKSPACE
   ============================================================ */

#define WORKSPACE "workspace"
#define INPUT_DIR "workspace/input"
#define OUTPUT_DIR "workspace/output"
#define BACKUP_DIR "workspace/backup"


/* ============================================================
   ACTIVITY LOG
   ============================================================ */

#define LOG_FILE "logs/agent_activity.log"


/* ============================================================
   RESOURCE LIMITS
   ============================================================ */

#define MEMORY_LIMIT 50000       // 50000 KB
#define CPU_LIMIT 1000           // 1000 CPU ticks


void log_activity(const char *agent, const char *activity);


/* ============================================================
   STRUCTURED MESSAGE
   This structure is transferred between agents through FIFOs.
   ============================================================ */

typedef struct
{
    char agent[32];
    char task[32];
    char file[128];
    char status[32];
    char result[128];

} TaskMessage;


/* ============================================================
   ACTIVITY LOGGING
   ============================================================ */

void log_activity(const char *agent, const char *activity)
{
    /*
       open()
       System call used to open/create the activity log file.
       O_APPEND ensures new entries are added at the end.
    */

    int fd = open(LOG_FILE,
                  O_WRONLY | O_CREAT | O_APPEND,
                  0644);

    if (fd == -1)
    {
        perror("Log file opening failed");
        return;
    }

    char log_entry[500];

    int length = snprintf(log_entry,
                          sizeof(log_entry),
                          "[%s] PID: %d | %s\n",
                          agent,
                          getpid(),
                          activity);

    /*
       write()
       System call used to write the activity information
       into the log file.
    */

    write(fd, log_entry, length);

    /*
       close()
       Closes the file descriptor after logging.
    */

    close(fd);
}


/* ============================================================
   RESTRICTED WORKSPACE VALIDATION
   ============================================================ */

int validate_workspace_path(const char *filename)
{
    char workspace_path[PATH_MAX];
    char requested_path[PATH_MAX];


    /*
       realpath()
       Converts a path into its absolute/canonical path.
       It is used to check whether the requested file
       belongs to the restricted workspace.
    */

    if (realpath(WORKSPACE, workspace_path) == NULL)
    {
        perror("Workspace path error");
        return 0;
    }

    if (realpath(filename, requested_path) == NULL)
    {
        printf("File does not exist: %s\n", filename);
        return 0;
    }


    size_t workspace_length = strlen(workspace_path);


    /*
       strcmp/strncmp logic:
       Check whether the requested file path starts
       inside the allowed workspace path.
    */

    if (strncmp(requested_path,
                workspace_path,
                workspace_length) != 0)
    {
        printf("ACCESS DENIED: File is outside restricted workspace.\n");
        return 0;
    }

    if (requested_path[workspace_length] != '/' &&
        requested_path[workspace_length] != '\0')
    {
        printf("ACCESS DENIED: Invalid workspace path.\n");
        return 0;
    }

    printf("Workspace access validated: %s\n", requested_path);

    return 1;
}


/* ============================================================
   FILE ACCESS VALIDATION
   ============================================================ */

int validate_file(const char *filename)
{
    /*
       First check whether the requested path belongs
       to the restricted workspace.
    */

    if (!validate_workspace_path(filename))
    {
        log_activity("Main Controller",
                     "Unauthorized file access rejected");

        return 0;
    }


    /*
       access()
       System call used to check:
       F_OK -> whether file exists
       R_OK -> whether file can be read
    */

    if (access(filename, F_OK) == 0 &&
        access(filename, R_OK) == 0)
    {
        printf("File validation successful: %s\n", filename);

        log_activity("Main Controller",
                     "File access validation successful");

        return 1;
    }

    printf("File validation failed: %s\n", filename);

    log_activity("Main Controller",
                 "File access validation failed");

    return 0;
}


/* ============================================================
   BACKUP WORKSPACE FILE
   ============================================================ */

int backup_file(const char *source)
{
    char backup_path[PATH_MAX];

    const char *filename = strrchr(source, '/');

    if (filename == NULL)
    {
        printf("Invalid source file.\n");
        return 0;
    }

    filename++;


    /*
       Create the backup filename.

       Example:
       workspace/input/factorial.c

       becomes:

       workspace/backup/factorial.c.bak
    */

    snprintf(backup_path,
             sizeof(backup_path),
             "%s/%s.bak",
             BACKUP_DIR,
             filename);


    /*
       open()
       Opens the original workspace file for reading.
    */

    int source_fd = open(source, O_RDONLY);

    if (source_fd == -1)
    {
        perror("Backup source opening failed");
        return 0;
    }


    /*
       open()
       Creates/opens the backup file.

       O_CREAT -> create if it doesn't exist
       O_TRUNC -> clear previous contents
       O_WRONLY -> write only
    */

    int backup_fd = open(backup_path,
                         O_WRONLY | O_CREAT | O_TRUNC,
                         0644);

    if (backup_fd == -1)
    {
        perror("Backup file creation failed");

        close(source_fd);

        return 0;
    }


    char buffer[1024];
    ssize_t bytes_read;


    /*
       read()
       Reads data from the original file.

       write()
       Copies the data into the backup file.
    */

    while ((bytes_read = read(source_fd,
                              buffer,
                              sizeof(buffer))) > 0)
    {
        ssize_t total_written = 0;

        while (total_written < bytes_read)
        {
            ssize_t bytes_written =
                write(backup_fd,
                      buffer + total_written,
                      bytes_read - total_written);

            if (bytes_written == -1)
            {
                perror("Backup writing failed");

                close(source_fd);
                close(backup_fd);

                return 0;
            }

            total_written += bytes_written;
        }
    }


    close(source_fd);
    close(backup_fd);


    printf("Backup created: %s\n", backup_path);

    log_activity("Execution Agent",
                 "Backup copy created before workspace operation");

    return 1;
}


/* ============================================================
   FILE SYNCHRONIZATION
   ============================================================ */

int synchronize_file(const char *filename)
{
    printf("\n========================================\n");
    printf("FILE SYNCHRONIZATION\n");
    printf("========================================\n");


    if (!validate_workspace_path(filename))
    {
        printf("Synchronization denied.\n");
        return 0;
    }


    /*
       open()
       Opens the workspace file for protected access.
    */

    int fd = open(filename, O_RDWR);

    if (fd == -1)
    {
        perror("File opening failed");
        return 0;
    }

    printf("Requesting lock for: %s\n", filename);


    /*
       flock()
       Linux system call used for file synchronization.

       LOCK_EX = exclusive lock

       This prevents multiple agents from performing
       the protected operation at the same time.
    */

    if (flock(fd, LOCK_EX) == -1)
    {
        perror("File lock failed");

        close(fd);

        return 0;
    }


    printf("Exclusive file lock acquired.\n");
    printf("Only one agent can access the file now.\n");


    printf("Creating backup before protected operation...\n");


    if (!backup_file(filename))
    {
        printf("Backup failed. Protected operation cancelled.\n");

        /*
           LOCK_UN
           Releases the file lock.
        */

        flock(fd, LOCK_UN);

        close(fd);

        return 0;
    }


    printf("Performing protected file operation...\n");

    sleep(2);

    printf("Protected file operation completed.\n");


    /*
       flock(LOCK_UN)
       Releases the exclusive file lock.
    */

    if (flock(fd, LOCK_UN) == -1)
    {
        perror("File unlock failed");

        close(fd);

        return 0;
    }

    printf("File lock released successfully.\n");

    close(fd);

    printf("File synchronization completed.\n");

    log_activity("Execution Agent",
                 "Workspace file synchronized using exclusive lock");

    return 1;
}


/* ============================================================
   SIGNAL HANDLER
   ============================================================ */

void handle_signal(int signal_number)
{
    if (signal_number == SIGTERM)
    {
        printf("\nExecution Agent received SIGTERM.\n");
        printf("Resource limit exceeded.\n");
        printf("Execution Agent is terminating safely.\n");

        log_activity("Execution Agent",
                     "Execution Agent terminated due to resource limit");


        /*
           _exit()
           Terminates the current process immediately.
        */

        _exit(2);
    }
}


/* ============================================================
   CODE GENERATOR AGENT
   ============================================================ */

void code_generator()
{
    printf("\n[Code Generator Agent]\n");
    printf("Agent PID: %d\n", getpid());
    printf("Generating code...\n");

    log_activity("Code Generator",
                 "Code generation started");

    sleep(2);


    /*
       open()
       Opens the named FIFO for writing.

       FIFO:
       generator_to_executor
    */

    int fd = open(FIFO_GENERATOR, O_WRONLY);

    if (fd == -1)
    {
        perror("Generator FIFO open failed");

        log_activity("Code Generator",
                     "Generator FIFO open failed");

        exit(1);
    }


    /*
       Create structured task information.
    */

    TaskMessage message;

    memset(&message, 0, sizeof(message));

    strcpy(message.agent, "Code Generator");
    strcpy(message.task, "Generate Code");
    strcpy(message.file, "workspace/input/factorial.c");
    strcpy(message.status, "SUCCESS");
    strcpy(message.result, "Code generated successfully");


    /*
       write()
       Sends the structured TaskMessage through
       the named FIFO to the Execution Agent.
    */

    write(fd, &message, sizeof(message));

    printf("Code Generator sent structured task information.\n");

    log_activity("Code Generator",
                 "Structured task information sent to Execution Agent");

    close(fd);

    printf("Code Generator finished.\n");

    log_activity("Code Generator",
                 "Code Generator finished");

    exit(0);
}


/* ============================================================
   EXECUTION AGENT
   ============================================================ */

void execution_agent()
{
    /*
       signal()
       Registers the SIGTERM signal handler.

       If the resource monitor sends SIGTERM,
       handle_signal() will execute.
    */

    signal(SIGTERM, handle_signal);

    printf("\n[Execution Agent]\n");
    printf("Agent PID: %d\n", getpid());
    printf("Waiting for code from Code Generator...\n");

    log_activity("Execution Agent",
                 "Execution Agent started");


    /*
       open()
       Opens the Generator FIFO for reading.
    */

    int fd_read = open(FIFO_GENERATOR, O_RDONLY);

    if (fd_read == -1)
    {
        perror("Generator FIFO open failed");

        log_activity("Execution Agent",
                     "Generator FIFO open failed");

        exit(1);
    }


    TaskMessage message;

    memset(&message, 0, sizeof(message));


    /*
       read()
       Receives the structured TaskMessage
       from the Code Generator Agent.
    */

    ssize_t bytes =
        read(fd_read,
             &message,
             sizeof(message));

    close(fd_read);


    if (bytes != sizeof(message))
    {
        printf("Invalid structured message received.\n");

        log_activity("Execution Agent",
                     "Invalid structured message received");

        exit(1);
    }


    printf("\nStructured Task Information Received:\n");

    printf("Agent  : %s\n", message.agent);
    printf("Task   : %s\n", message.task);
    printf("File   : %s\n", message.file);
    printf("Status : %s\n", message.status);
    printf("Result : %s\n", message.result);

    log_activity("Execution Agent",
                 "Structured task information received");


    /* ========================================================
       FILE ACCESS VALIDATION
       ======================================================== */

    printf("\nChecking requested workspace file...\n");

    if (!validate_file(message.file))
    {
        printf("Execution denied due to invalid file access.\n");

        log_activity("Execution Agent",
                     "Execution denied due to file access validation");

        exit(1);
    }


    /* ========================================================
       FILE SYNCHRONIZATION + BACKUP
       ======================================================== */

    if (!synchronize_file(message.file))
    {
        printf("Workspace operation failed.\n");

        log_activity("Execution Agent",
                     "Workspace operation failed");

        exit(1);
    }


    /* ========================================================
       COMPILE PROGRAM
       ======================================================== */

    printf("\nCompiling factorial.c...\n");

    log_activity("Execution Agent",
                 "Compilation started");


    /*
       fork()
       Creates a child process.

       The child will run GCC.
    */

    pid_t compiler_pid = fork();

    if (compiler_pid < 0)
    {
        perror("Compiler process creation failed");

        log_activity("Execution Agent",
                     "Compiler process creation failed");

        exit(1);
    }


    if (compiler_pid == 0)
    {
        /*
           execlp()
           Replaces this child process with the GCC compiler.

           gcc workspace/input/factorial.c
               -o workspace/output/factorial
        */

        execlp("gcc",
               "gcc",
               message.file,
               "-o",
               "workspace/output/factorial",
               NULL);

        perror("Compilation failed");

        _exit(1);
    }


    /*
       waitpid()
       Parent waits for the compiler child process
       to finish.
    */

    int compiler_status;

    waitpid(compiler_pid,
            &compiler_status,
            0);


    if (WIFEXITED(compiler_status) &&
        WEXITSTATUS(compiler_status) == 0)
    {
        printf("Compilation successful!\n");

        log_activity("Execution Agent",
                     "Compilation successful");
    }
    else
    {
        printf("Compilation failed!\n");

        log_activity("Execution Agent",
                     "Compilation failed");

        exit(1);
    }


    /* ========================================================
       EXECUTE PROGRAM
       ======================================================== */

    printf("\nExecuting factorial program...\n");

    log_activity("Execution Agent",
                 "Program execution started");

    sleep(2);


    /*
       fork()
       Creates another child process for running
       the compiled factorial program.
    */

    pid_t program_pid = fork();

    if (program_pid < 0)
    {
        perror("Program process creation failed");

        log_activity("Execution Agent",
                     "Program process creation failed");

        exit(1);
    }


    if (program_pid == 0)
    {
        /*
           execl()
           Replaces the child process with the
           compiled factorial program.
        */

        execl("./workspace/output/factorial",
              "./workspace/output/factorial",
              NULL);

        perror("Program execution failed");

        _exit(1);
    }


    /*
       waitpid()
       Parent waits for the factorial program to finish.
    */

    int program_status;

    waitpid(program_pid,
            &program_status,
            0);


    if (WIFEXITED(program_status) &&
        WEXITSTATUS(program_status) == 0)
    {
        printf("Program executed successfully!\n");

        log_activity("Execution Agent",
                     "Program execution successful");
    }
    else
    {
        printf("Program execution failed!\n");

        log_activity("Execution Agent",
                     "Program execution failed");

        exit(1);
    }


    /* ========================================================
       SEND EXECUTION RESULT TO REVIEW AGENT
       ======================================================== */

    /*
       open()
       Opens the second FIFO for writing.
    */

    int fd_write = open(FIFO_REVIEWER, O_WRONLY);

    if (fd_write == -1)
    {
        perror("Reviewer FIFO open failed");

        log_activity("Execution Agent",
                     "Reviewer FIFO open failed");

        exit(1);
    }


    TaskMessage result;

    memset(&result, 0, sizeof(result));

    strcpy(result.agent, "Execution Agent");
    strcpy(result.task, "Execute Program");
    strcpy(result.file, "workspace/input/factorial.c");
    strcpy(result.status, "SUCCESS");

    strcpy(result.result,
           "factorial.c compiled and executed successfully");


    /*
       write()
       Sends structured execution information
       to the Code Review Agent.
    */

    write(fd_write,
          &result,
          sizeof(result));

    printf("Execution Agent sent structured execution result.\n");

    log_activity("Execution Agent",
                 "Structured execution result sent to Code Review Agent");

    close(fd_write);

    printf("Execution Agent finished.\n");

    log_activity("Execution Agent",
                 "Execution Agent finished");

    exit(0);
}


/* ============================================================
   CODE REVIEW AGENT
   ============================================================ */

void code_review_agent()
{
    printf("\n[Code Review Agent]\n");
    printf("Agent PID: %d\n", getpid());
    printf("Waiting for execution result...\n");

    log_activity("Code Review Agent",
                 "Code Review Agent started");


    /*
       open()
       Opens the Execution-to-Reviewer FIFO for reading.
    */

    int fd_read = open(FIFO_REVIEWER, O_RDONLY);

    if (fd_read == -1)
    {
        perror("Reviewer FIFO open failed");

        log_activity("Code Review Agent",
                     "Reviewer FIFO open failed");

        exit(1);
    }


    TaskMessage result;

    memset(&result, 0, sizeof(result));


    /*
       read()
       Receives structured execution information.
    */

    ssize_t bytes =
        read(fd_read,
             &result,
             sizeof(result));

    close(fd_read);


    if (bytes != sizeof(result))
    {
        printf("Invalid execution information received.\n");

        log_activity("Code Review Agent",
                     "Invalid execution information received");

        exit(1);
    }


    printf("\nStructured Execution Information Received:\n");

    printf("Agent  : %s\n", result.agent);
    printf("Task   : %s\n", result.task);
    printf("File   : %s\n", result.file);
    printf("Status : %s\n", result.status);
    printf("Result : %s\n", result.result);

    log_activity("Code Review Agent",
                 "Structured execution result received");


    /* ========================================================
       CODE REVIEW
       ======================================================== */

    printf("\nReviewing code...\n");

    log_activity("Code Review Agent",
                 "Code review started");

    sleep(2);

    printf("Code Review completed.\n");
    printf("Result: Code is ready for testing.\n");

    log_activity("Code Review Agent",
                 "Code review completed successfully");


    /* ========================================================
       SEND REVIEW RESULT TO TESTING AGENT
       ======================================================== */

    /*
       open()
       Opens the third FIFO for writing.
    */

    int fd_write = open(FIFO_TESTER, O_WRONLY);

    if (fd_write == -1)
    {
        perror("Tester FIFO open failed");

        log_activity("Code Review Agent",
                     "Tester FIFO open failed");

        exit(1);
    }


    TaskMessage review;

    memset(&review, 0, sizeof(review));

    strcpy(review.agent, "Code Review Agent");
    strcpy(review.task, "Review Code");
    strcpy(review.file, "workspace/input/factorial.c");
    strcpy(review.status, "SUCCESS");

    strcpy(review.result,
           "Code review passed and ready for testing");


    /*
       write()
       Sends review result to Testing Agent.
    */

    write(fd_write,
          &review,
          sizeof(review));

    printf("Code Review Agent sent structured review result.\n");

    log_activity("Code Review Agent",
                 "Structured review result sent to Testing Agent");

    close(fd_write);

    printf("Code Review Agent finished.\n");

    log_activity("Code Review Agent",
                 "Code Review Agent finished");

    exit(0);
}


/* ============================================================
   TESTING AGENT
   ============================================================ */

void testing_agent()
{
    printf("\n[Testing Agent]\n");
    printf("Agent PID: %d\n", getpid());
    printf("Waiting for code review result...\n");

    log_activity("Testing Agent",
                 "Testing Agent started");


    /*
       open()
       Opens the Tester FIFO for reading.
    */

    int fd = open(FIFO_TESTER, O_RDONLY);

    if (fd == -1)
    {
        perror("Tester FIFO open failed");

        log_activity("Testing Agent",
                     "Tester FIFO open failed");

        exit(1);
    }


    TaskMessage review;

    memset(&review, 0, sizeof(review));


    /*
       read()
       Receives the review result.
    */

    ssize_t bytes =
        read(fd,
             &review,
             sizeof(review));

    close(fd);


    if (bytes != sizeof(review))
    {
        printf("Invalid review information received.\n");

        log_activity("Testing Agent",
                     "Invalid review information received");

        exit(1);
    }


    printf("\nStructured Review Information Received:\n");

    printf("Agent  : %s\n", review.agent);
    printf("Task   : %s\n", review.task);
    printf("File   : %s\n", review.file);
    printf("Status : %s\n", review.status);
    printf("Result : %s\n", review.result);

    log_activity("Testing Agent",
                 "Structured review result received");


    /* ========================================================
       RUN ACTUAL TEST
       ======================================================== */

    printf("\nRunning actual test...\n");

    log_activity("Testing Agent",
                 "Testing started");


    /*
       popen()
       Opens a process and allows the Testing Agent
       to read the program's output.
    */

    FILE *process;

    char output[200] = {0};

    process = popen("./workspace/output/factorial",
                    "r");


    if (process == NULL)
    {
        perror("Testing process failed");

        log_activity("Testing Agent",
                     "Testing process failed");

        exit(1);
    }


    /*
       fgets()
       Reads the output produced by the factorial program.
    */

    if (fgets(output,
              sizeof(output),
              process) != NULL)
    {
        printf("Program output: %s", output);
    }


    /*
       pclose()
       Closes the process opened using popen().
    */

    int status = pclose(process);


    /* ========================================================
       CHECK EXPECTED RESULT
       ======================================================== */

    if (strstr(output,
               "Factorial of 5 = 120") != NULL &&
        status == 0)
    {
        printf("\nTEST RESULT: PASS\n");

        printf("Expected: Factorial of 5 = 120\n");

        printf("Actual:   %s", output);

        log_activity("Testing Agent",
                     "TEST RESULT: PASS");
    }
    else
    {
        printf("\nTEST RESULT: FAIL\n");

        printf("Expected: Factorial of 5 = 120\n");

        printf("Actual:   %s", output);

        log_activity("Testing Agent",
                     "TEST RESULT: FAIL");
    }


    printf("\nTesting Agent finished.\n");

    log_activity("Testing Agent",
                 "Testing Agent finished");

    exit(0);
}


/* ============================================================
   RESOURCE MONITORING
   ============================================================ */

void monitor_process(pid_t pid)
{
    char status_path[100];
    char stat_path[100];


    /*
       /proc/<PID>/status
       Linux virtual file containing process information,
       including memory usage.

       /proc/<PID>/stat
       Contains process CPU statistics.
    */

    snprintf(status_path,
             sizeof(status_path),
             "/proc/%d/status",
             pid);

    snprintf(stat_path,
             sizeof(stat_path),
             "/proc/%d/stat",
             pid);


    printf("\n========================================\n");
    printf("RESOURCE MONITOR\n");
    printf("Monitoring Agent PID: %d\n", pid);
    printf("Memory Limit: %d KB\n", MEMORY_LIMIT);
    printf("CPU Limit: %d ticks\n", CPU_LIMIT);
    printf("========================================\n");


    log_activity("Resource Monitor",
                 "Resource monitoring started");


    for (int i = 0; i < 5; i++)
    {
        /*
           fopen()
           Opens the /proc status file to read
           memory information.
        */

        FILE *status_file =
            fopen(status_path, "r");


        if (status_file == NULL)
        {
            printf("\nProcess %d has finished.\n",
                   pid);

            log_activity("Resource Monitor",
                         "Execution Agent has finished");

            break;
        }


        char line[256];

        long memory = 0;


        /*
           Read /proc/<PID>/status line by line.
        */

        while (fgets(line,
                     sizeof(line),
                     status_file))
        {
            if (strncmp(line,
                        "VmRSS:",
                        6) == 0)
            {
                sscanf(line,
                       "VmRSS: %ld",
                       &memory);

                break;
            }
        }

        fclose(status_file);


        /* ====================================================
           CPU MONITORING
           ==================================================== */

        /*
           fopen()
           Opens /proc/<PID>/stat.
        */

        FILE *stat_file =
            fopen(stat_path, "r");

        long utime = 0;
        long stime = 0;


        if (stat_file != NULL)
        {
            char buffer[2048];


            if (fgets(buffer,
                      sizeof(buffer),
                      stat_file))
            {
                char *closing_bracket =
                    strrchr(buffer, ')');


                if (closing_bracket != NULL)
                {
                    char *fields =
                        closing_bracket + 2;

                    int field = 3;

                    char *token =
                        strtok(fields, " ");


                    /*
                       Extract CPU user time and system time
                       from /proc/<PID>/stat.
                    */

                    while (token != NULL)
                    {
                        if (field == 14)
                        {
                            utime = atol(token);
                        }

                        if (field == 15)
                        {
                            stime = atol(token);
                            break;
                        }

                        token = strtok(NULL, " ");

                        field++;
                    }
                }
            }

            fclose(stat_file);
        }


        long cpu_time =
            utime + stime;


        printf("\nAgent PID: %d\n",
               pid);

        printf("Memory Usage: %ld KB\n",
               memory);

        printf("CPU Time: %ld ticks\n",
               cpu_time);


        /* ====================================================
           MEMORY LIMIT
           ==================================================== */

        if (memory > MEMORY_LIMIT)
        {
            printf("\nWARNING: Memory limit exceeded!\n");
            printf("Sending SIGTERM to Execution Agent...\n");


            log_activity("Resource Monitor",
                         "Memory limit exceeded - SIGTERM sent");


            /*
               kill()
               Sends SIGTERM to the Execution Agent.
            */

            kill(pid, SIGTERM);

            break;
        }


        /* ====================================================
           CPU LIMIT
           ==================================================== */

        if (cpu_time > CPU_LIMIT)
        {
            printf("\nWARNING: CPU limit exceeded!\n");
            printf("Sending SIGTERM to Execution Agent...\n");


            log_activity("Resource Monitor",
                         "CPU limit exceeded - SIGTERM sent");


            /*
               kill()
               Sends SIGTERM to the monitored process.
            */

            kill(pid, SIGTERM);

            break;
        }


        sleep(1);
    }


    printf("\nResource monitoring completed.\n");

    log_activity("Resource Monitor",
                 "Resource monitoring completed");
}


/* ============================================================
   RUN COMPLETE MULTI-AGENT SYSTEM
   ============================================================ */

void run_multi_agent_system()
{
    printf("\n========================================\n");
    printf("     STARTING MULTI-AGENT SYSTEM\n");
    printf("========================================\n");


    log_activity("Main Controller",
                 "Multi-Agent System execution started");


    /* ========================================================
       CHECK REQUIRED DIRECTORIES
       ======================================================== */

    /*
       access()
       Checks whether the required workspace directories
       exist before starting the agents.
    */

    if (access(WORKSPACE, F_OK) != 0 ||
        access(INPUT_DIR, F_OK) != 0 ||
        access(OUTPUT_DIR, F_OK) != 0 ||
        access(BACKUP_DIR, F_OK) != 0)
    {
        printf("\nRequired workspace directories are missing.\n");

        printf("Please create the project directories first.\n");

        log_activity("Main Controller",
                     "Required workspace directories missing");

        return;
    }


    printf("\nChecking workspace file...\n");


    if (!validate_file("workspace/input/factorial.c"))
    {
        printf("Workspace file is not available.\n");

        return;
    }


    printf("Workspace file is available.\n");


    /* ========================================================
       CREATE CODE GENERATOR PROCESS
       ======================================================== */

    /*
       fork()
       Creates a separate process representing
       the Code Generator Agent.
    */

    pid_t generator_pid = fork();


    if (generator_pid < 0)
    {
        perror("Generator process creation failed");
        return;
    }


    if (generator_pid == 0)
    {
        code_generator();
    }


    /* ========================================================
       CREATE EXECUTION AGENT PROCESS
       ======================================================== */

    /*
       fork()
       Creates a separate Execution Agent process.
    */

    pid_t execution_pid = fork();


    if (execution_pid < 0)
    {
        perror("Execution process creation failed");
        return;
    }


    if (execution_pid == 0)
    {
        execution_agent();
    }


    /* ========================================================
       CREATE CODE REVIEW AGENT PROCESS
       ======================================================== */

    /*
       fork()
       Creates a separate Code Review Agent process.
    */

    pid_t reviewer_pid = fork();


    if (reviewer_pid < 0)
    {
        perror("Review process creation failed");
        return;
    }


    if (reviewer_pid == 0)
    {
        code_review_agent();
    }


    /* ========================================================
       CREATE TESTING AGENT PROCESS
       ======================================================== */

    /*
       fork()
       Creates a separate Testing Agent process.
    */

    pid_t tester_pid = fork();


    if (tester_pid < 0)
    {
        perror("Testing process creation failed");
        return;
    }


    if (tester_pid == 0)
    {
        testing_agent();
    }


    /* ========================================================
       RESOURCE MONITORING
       ======================================================== */

    monitor_process(execution_pid);


    /* ========================================================
       WAIT FOR ALL AGENTS
       ======================================================== */

    /*
       waitpid()
       Makes the Main Controller wait for every
       agent process to finish.
    */

    waitpid(generator_pid, NULL, 0);

    waitpid(execution_pid, NULL, 0);

    waitpid(reviewer_pid, NULL, 0);

    waitpid(tester_pid, NULL, 0);


    printf("\n========================================\n");
    printf("All AI Agents Completed Successfully!\n");
    printf("========================================\n");


    log_activity("Main Controller",
                 "All AI Agents completed");
}


/* ============================================================
   VIEW ACTIVITY LOG
   ============================================================ */

void view_activity_log()
{
    printf("\n========================================\n");
    printf("          ACTIVITY LOG\n");
    printf("========================================\n");


    /*
       fopen()
       Opens the activity log for reading.
    */

    FILE *file = fopen(LOG_FILE, "r");


    if (file == NULL)
    {
        printf("No activity log found yet.\n");
        return;
    }


    char line[500];


    /*
       fgets()
       Reads and displays the log one line at a time.
    */

    while (fgets(line,
                 sizeof(line),
                 file))
    {
        printf("%s", line);
    }


    fclose(file);


    printf("\n========================================\n");
}


/* ============================================================
   VIEW BACKUP FILE
   ============================================================ */

void view_backup_file()
{
    printf("\n========================================\n");
    printf("          BACKUP FILE\n");
    printf("========================================\n");


    /*
       fopen()
       Opens the backup copy for reading.
    */

    FILE *file =
        fopen("workspace/backup/factorial.c.bak",
              "r");


    if (file == NULL)
    {
        printf("Backup file not found.\n");

        printf("Run the Multi-Agent System first.\n");

        return;
    }


    char line[500];


    /*
       fgets()
       Reads the backup file line by line.
    */

    while (fgets(line,
                 sizeof(line),
                 file))
    {
        printf("%s", line);
    }


    fclose(file);


    printf("\n========================================\n");
}


/* ============================================================
   MENU
   ============================================================ */

void show_menu()
{
    printf("\n\n");

    printf("========================================\n");
    printf("   MULTI-AGENT AI EXECUTION SYSTEM\n");
    printf("========================================\n");

    printf("\n1. Run Multi-Agent System\n");
    printf("2. View Activity Log\n");
    printf("3. View Backup File\n");
    printf("4. Exit\n");

    printf("\nEnter your choice: ");
}


/* ============================================================
   MAIN CONTROLLER
   ============================================================ */

int main()
{
    int choice;


    printf("========================================\n");
    printf("     MULTI-AGENT AI EXECUTION SYSTEM\n");
    printf("========================================\n");

    printf("\nMain Controller Started\n");


    log_activity("Main Controller",
                 "Main Controller started");


    /* ========================================================
       MENU LOOP
       ======================================================== */

    while (1)
    {
        show_menu();


        /*
           scanf()
           Reads the user's menu choice.
        */

        if (scanf("%d", &choice) != 1)
        {
            printf("\nInvalid input. Please enter a number.\n");


            /*
               getchar()
               Clears invalid input from the input buffer.
            */

            while (getchar() != '\n');

            continue;
        }


        /*
           switch()
           Selects the requested menu operation.
        */

        switch (choice)
        {
            case 1:

                /*
                   Runs the complete multi-agent workflow.
                */

                run_multi_agent_system();

                break;


            case 2:

                /*
                   Displays automatically generated
                   agent activity and execution logs.
                */

                view_activity_log();

                break;


            case 3:

                /*
                   Displays the backup copy of the
                   workspace file.
                */

                view_backup_file();

                break;


            case 4:

                printf("\n========================================\n");

                printf("Exiting Multi-Agent AI Execution System...\n");

                printf("========================================\n");


                log_activity("Main Controller",
                             "System exited");


                /*
                   return 0
                   Terminates the Main Controller normally.
                */

                return 0;


            default:

                printf("\nInvalid choice.\n");

                printf("Please select 1, 2, 3, or 4.\n");
        }
    }


    return 0;
}
