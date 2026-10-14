# AURA — Autonomous Unified Runtime & Agent Orchestration

AURA is a local C-program execution control center. The dashboard shows the system architecture, FIFO live flow, and the work performed by all six components:

- **Controllers:** Orchestrator and Monitor
- **Agents:** Generator, Executor, Reviewer, and Tester
- **IPC:** Linux named FIFOs between each pipeline stage
- **Execution:** GCC compiles each generated or supplied C program; AURA captures compiler diagnostics and program output
- **Review:** Reviewer takes an exclusive `flock`, backs up the source, records its decision, and releases the lock
- **Testing:** Tester compares actual output with the expected output

## Run the application

The runtime uses Linux FIFOs, so run it from WSL on the Linux side of the toolchain:

```bash
cd "/mnt/c/Users/APPLE/Desktop/2nd_year files/OS/final_project"
python3 aura_service.py
```

Open [http://localhost:8000](http://localhost:8000). Use the **Overview**, **Live Flow**, and **Agents Work** menu items to move between the architecture view, current IPC/events, and program submission/work details. Live Flow includes a timestamped event timeline with Monitor samples of worker PID, resident memory, and CPU ticks.

## Run a program

In **Agents Work**, enter a request such as `factorial of 5` and press **Start run**. The built-in catalog contains 15 C programs: factorial, Fibonacci, prime check, number palindrome, sum, even/odd, GCD, LCM, reverse number, Armstrong number, multiplication table, array maximum, array sort, Celsius-to-Fahrenheit, and basic calculator.

For a custom program, expand **Run custom C source instead**, paste C source, and provide its expected output. Start/Pause/Resume/Stop control the real worker processes. Run artifacts—including `program.c`, the compiled executable, captured output, FIFO nodes, lock file, and `backups/program.c.bak`—are stored under:

```text
/tmp/aura_runtime_workspace/runs/<run-id>/
```

## Safety and platform notes

- The service binds to `127.0.0.1`; it is intended for local use.
- Custom C source is compiled and executed on the local machine. Only run source code you trust.
- GCC must be installed in WSL.
- Named pipes are created in the Linux filesystem under `/tmp`, not on the Windows-mounted workspace, because the runtime relies on Linux FIFO and `flock` behavior.
