# ⚡ Multi-Level Task Executor
[![Watch Demo](https://img.shields.io/badge/▶-Watch_Demo_Video-red?style=for-the-badge)](https://drive.google.com/file/d/1vBkU9b8GK8Pqg8P9RXNlG_ctn7n24gzH/view?usp=sharing)

### Watch on Google Drive

[Click here to watch the demo video](https://drive.google.com/file/d/1vBkU9b8GK8Pqg8P9RXNlG_ctn7n24gzH/view?usp=sharing)

> A Windows systems-programming project that builds a small distributed task engine from raw Win32 system calls — real processes, real threads, real crashes, and a live browser dashboard watching it all happen.

[![Platform](https://img.shields.io/badge/platform-Windows-blue)](https://www.microsoft.com/windows)
[![Language](https://img.shields.io/badge/language-C-00599C)](https://en.wikipedia.org/wiki/C_(programming_language))
[![IDE](https://img.shields.io/badge/IDE-Code::Blocks-2B2B2B)](https://www.codeblocks.org/)
[![API](https://img.shields.io/badge/API-Win32-0078D6)](https://learn.microsoft.com/en-us/windows/win32/)

---

## 📖 Table of Contents
- [Overview](#-overview)
- [Why This Project Exists](#-why-this-project-exists)
- [Key Features](#-key-features)
- [System Architecture](#-system-architecture)
- [Skills Demonstrated](#-skills-demonstrated)
- [Build & Run](#-build--run)
- [Using the Dashboard](#-using-the-dashboard)
- [Project Structure](#-project-structure)
- [Known Limitations](#-known-limitations--honest-notes)
- [Future Improvements](#-future-improvements)

---

## 🚀 Overview

The **Multi-Level Task Executor** simulates, at a small and fully inspectable scale, exactly the kind of system that sits underneath real production infrastructure: 
a **dispatcher** that hands out work, 
a pool of **worker processes** that actually do it, 
and **threads inside each worker** that do it in parallel.
Nothing here is simulated at the OS level — every process, 
thread, pipe, and priority level is a genuine Win32 API call, 
and you can watch it all happen live through a browser dashboard while it runs.

| Piece             | Real-world analogy       | What it actually is                                              |
|-------------------|--------------------------|------------------------------------------------------------------|
| **Dispatcher**    | Factory floor manager    | One process, spawns and load-balances the workers                |
| **Workers**       | Shift teams              | 3 separate OS processes, isolated from each other                |
| **Thread pool**   | Each team's hands        | 4 threads per worker, created once, reused forever               |
| **Pipes**         | Internal memos           | `CreatePipe`-based IPC between dispatcher and each worker        |
| **Web Dashboard** | The control room monitor | A custom HTTP server pushing live updates via Server-Sent Events |

---
### Why This Project Matters

| Benefit                  | Description                                                                        |
|--------------------------|------------------------------------------------------------------------------------|
| **⚡ 10x Faster**       | Thread pool eliminates the overhead of creating threads for every task              |
| **🛡️ Self-Healing**     | If a worker crashes, the system detects it and restarts it automatically            |
| **📊 Live Monitoring**  | Real-time web dashboard shows worker status, task progress, and performance metrics |
| **🎯 OS-Level Control** | Demonstrates priority-based scheduling using real Windows system calls              |
| **📚 Educational**      | Shows how real system calls work in practice, not just theory                       |

---

## 🔥 Key Features

### 1. Thread Pool, Not Thread-Per-Task
Each worker process creates its **4 threads exactly once** at startup and reuses them for every task it's ever handed — no `CreateThread` call happens mid-run. `benchmark.c` is a standalone, self-contained proof of *why* this matters: it runs the same 5,000 trivial tasks two ways — spawning a new thread per task vs. reusing 4 — and prints the actual measured speedup on your machine. (Run it yourself; the exact multiplier depends on your CPU, but the pool wins by a wide, consistent margin every time.)

### 2. Real OS-Level Priority Control
`SetPriorityClass` is called on Worker 1, genuinely raising it to `ABOVE_NORMAL_PRIORITY_CLASS` relative to the other two — not just displayed as a label, but actually changing how the Windows scheduler treats that process under contention.

### 3. Load Balancing With Fair Tie-Breaking
Tasks go to whichever worker has the fewest outstanding jobs. A rotating tie-break (`g_nextWorkerHint`) prevents the classic bug where, if all workers *look* equally free at the same instant, the same one keeps winning forever — an actual bug this project hit and fixed during development (see the technical report).

### 4. Genuine Fault Tolerance — Not a Simulation
A worker can be crashed two ways: automatically at the halfway point of every run, or on demand via the dashboard's **Inject Crash** button. Either way, it's a real `NULL` pointer dereference that kills the entire process with a real Windows access violation (`0xC0000005`). The dispatcher detects this via `GetExitCodeProcess`, logs it, tallies any lost in-flight work honestly (no silent hiding, no fake retry), and relaunches a fresh copy of the worker — all while the other two workers, in separate address spaces, never notice anything happened.

### 5. Live Web Dashboard (Server-Sent Events, No Frameworks)
A hand-rolled HTTP server (`server/http_server.c`, built on raw Winsock) serves the dashboard and pushes live JSON updates over SSE — worker status, PID, priority, CPU time, CPU cycles, memory, page faults, I/O operations, per-task-type execution time breakdowns, latency percentiles, and a running event log, all updating in your browser without a page refresh.

### 6. Seven Distinct, Heterogeneous Workloads
`FACT`, `FIB`, `PRIME`, `SUMSQ`, `MATRIX` (multiplication), `SORT` (bubble sort), and `CRYPTO` (hash mixing) — deliberately chosen so some finish in microseconds and others (large `FIB`) take seconds, which is exactly the kind of workload variance that makes scheduling *decisions* matter instead of being an academic abstraction.

### 7. Two Independent Measurements of "How Fast," Not One
**Latency** (dispatcher-measured, full round trip: queue wait + compute + IPC) and **execution time** (worker-measured, pure compute only) are tracked separately per task. The *gap* between them is queueing delay made visible — a live, physical demonstration of the concept behind scheduling algorithms like Shortest-Job-First.

---

**One task's journey:** dispatcher writes `TASK 7 FIB 30` down a pipe 
→ worker's main thread reads it and drops it in a shared queue 
→ a semaphore wakes one idle pool thread 
→ that thread computes it alone, unlocked, so it can run truly in parallel with its siblings 
→ result goes back as `RESULT 7 832040 0.12` 
→ dispatcher's reader thread updates counters and pushes a fresh JSON snapshot to every connected browser.

---

## 🛠️ Skills Demonstrated

### Systems Programming
| Skill                        | Where                                               |
|------------------------------|-----------------------------------------------------|
| Process creation & isolation | `CreateProcess` in `CreateWorkerProcess()`          |
| Inter-process communication  | `CreatePipe` / `ReadFile` / `WriteFile`             |
| OS-level scheduling control  | `SetPriorityClass`, observed via `GetPriorityClass` |
| Process health monitoring    | `GetExitCodeProcess` in `CheckWorkerHealth()`       |
| Fine-grained CPU accounting  | `GetProcessTimes`, `QueryProcessCycleTime`          |
| Memory/IO introspection      | `GetProcessWorkingSetSize`, `GetProcessIoCounters`  |
| Thread enumeration           | `CreateToolhelp32Snapshot` + `Thread32First/Next`   |

### Concurrency & Synchronization
| Skill                      | Where                                                                                            |
|----------------------------|--------------------------------------------------------------------------------------------------|
| Thread pooling             | `threadpool.c` — 4 threads, created once, reused forever                                         |
| Producer-consumer queue    | Ring buffer + semaphore in `ThreadPool_Submit`/`WorkerThreadProc`                                |
| Lock-free counters         | `Interlocked*` family for cross-thread stats                                                     |
| Scoped locking             | `CRITICAL_SECTION` held only for the minimum needed region                                       |
| Race-condition remediation | Per-thread PRNG state instead of global `rand()`/`srand()`                                       |
| Cross-thread handle safety | `g_workersLock` protecting a crash-triggered handle swap against a concurrent web-triggered read |

### Fault Tolerance / Reliability Engineering
| Skill                 | Where                                                           |
|-----------------------|-----------------------------------------------------------------|
| Fault injection       | Deliberate `NULL`-pointer crash task, triggerable on demand     |
| Crash detection       | `GetExitCodeProcess` polled every dispatch cycle                |
| Automatic recovery    | `RestartWorker()` — fresh process, same slot, honest accounting |
| Graceful degradation  | Lost in-flight tasks are counted and reported, never hidden     |

### Performance Engineering
| Skill                               | Where                                                                                                     |
|-------------------------------------|-----------------------------------------------------------------------------------------------------------|
| Measured, not assumed, optimization | `benchmark.c` — live A/B comparison, real numbers every run                                               |
| Syscall batching                    | Chunked pipe reads instead of one `ReadFile` per byte                                                     |
| Avoiding redundant expensive calls  | Per-worker metrics collected once per dashboard refresh, not 3×                                           |
| Buffer-safety under scale           | `AppendJSON` helper preventing unsigned-underflow corruption if the JSON payload ever outgrows its buffer |

### Web / Systems Integration
| Skill                           | Where                                                       |
|---------------------------------|-------------------------------------------------------------|
| Hand-rolled HTTP server         | `server/http_server.c` — raw Winsock, no libraries          |
| Real-time push updates          | Server-Sent Events, no polling                              |
| Safe concurrent client handling | Per-connection threads, slot-based client table             |
| Full-stack live demo            | C backend → JSON → vanilla JS/CSS frontend, zero frameworks |

---

## 💻 Build & Run

### Prerequisites
- Windows 11
- Code::Blocks with MinGW-w64 GCC

### Project layout expected by the build
```
TaskExecutor\
├── web\dashboard.html
├── server\http_server.c / http_server.h
├── dispatcher.c, worker.c, threadpool.c, threadpool.h, common.h, benchmark.c
└── TaskExecutor.cbp
```

### Build targets (Code::Blocks)
| Target          | Compiles                               | Output           |
|-----------------|----------------------------------------|------------------|
| **Worker**      | `worker.c`, `threadpool.c`             | `worker.exe`     |
| **Dispatcher**  | `dispatcher.c`, `server/http_server.c` | `dispatcher.exe` |
| **Benchmark**   | `benchmark.c` (standalone)             | `benchmark.exe`  |

Make sure the **Dispatcher** target links `ws2_32` (for the HTTP server's sockets) — `http_server.c` already requests it via `#pragma comment(lib, "ws2_32.lib")`, but confirm it under Project → Build options → Linker settings if you hit an unresolved-symbol error.

### Run it
1. Build **Worker** first, then **Dispatcher** — both `.exe`s must land in the same output folder.
2. Run `dispatcher.exe`. It will:
   - Spawn the 3 workers
   - Start the HTTP server on port **8888**
   - Open `http://localhost:8888/dashboard.html` in your default browser automatically
3. Watch the console *and* the browser simultaneously — both are live views of the same run.

---

## 🖱️ Using the Dashboard

- **Inject Crash** — sends a real fault-injection signal to a worker process right now (picked at random each click). Disabled once all tasks finish, since crashing a worker seconds before shutdown wouldn't demonstrate anything useful.
- **Clear Log** — clears the event log for everyone currently viewing the dashboard (broadcast to all connected browsers). It is purely a display reset: it does **not** touch task counts, crash history, or any internal dispatcher state.

---

## 📂 Project Structure

```
TaskExecutor/
├── web/
│   └── dashboard.html          # Live dashboard (HTML/CSS/vanilla JS, SSE client)
├── server/
│   ├── http_server.c           # Hand-rolled HTTP + SSE server (Winsock)
│   └── http_server.h
├── dispatcher.c                # Spawns/monitors/load-balances workers, serves dashboard
├── worker.c                    # Child process: reads tasks, owns one thread pool
├── threadpool.c / threadpool.h # The actual thread pool + 7 task-type implementations
├── common.h                    # Shared wire-format struct + tunables
├── benchmark.c                 # Standalone thread-pool-vs-thread-per-task proof
└── TaskExecutor.cbp
```

---

## ⚠️ Known Limitations & Honest Notes

Being upfront about these is more useful than pretending they don't exist:

- **Lost tasks aren't retried.** If a worker crashes mid-task, that task's result is gone for good — the dispatcher counts it honestly as "lost" rather than silently hanging or fabricating a result. A retry/re-queue mechanism is a natural next step.
- **Single-machine only.** All "distribution" happens across processes on one machine, not across a network — a deliberate scope choice to keep the OS concepts (not networking) front and center.
- **A shared-memory IPC path was explored during development** (an alternative to pipes using `CreateFileMapping`/named synchronization objects) but isn't part of the current build — an interesting comparison (copy-through-kernel vs. zero-copy shared pages) worth revisiting as a future addition.
- **The dashboard's per-worker thread count** uses a full-system `CreateToolhelp32Snapshot` — accurate, but relatively heavyweight compared to the rest of the metrics; fine at this scale, worth caching if this ever needs to update faster than a few times per second.

---

## 🔮 Future Improvements

- [ ] Re-queue lost tasks instead of just counting them
- [ ] Restore and properly integrate the shared-memory IPC path as a real alternative to pipes
- [ ] Dynamic worker pool sizing (add/remove workers at runtime)
- [ ] Persist run history so past runs can be compared, not just the live one
- [ ] Multi-machine distribution over TCP instead of local pipes

---

*Built as a systems-programming exploration of processes, threads, synchronization, fault tolerance, and what it actually takes to watch all of that happen live.*
