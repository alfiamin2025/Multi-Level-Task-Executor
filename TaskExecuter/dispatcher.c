/*
 * dispatcher.c  ->  compiles to dispatcher.exe
 * ----------------------------------------------
 * This is the program you actually run. It:
 *   1. Spawns NUM_WORKERS copies of worker.exe (CreateProcess)
 *   2. Wires each worker's stdin/stdout to pipes it controls (CreatePipe)
 *   3. Hands out tasks to whichever worker looks least busy
 *   4. Reads results back via a dedicated reader thread per worker
 *   5. Traces each worker's OS-level status (alive? priority? CPU time?)
 *   6. Serves a Web Dashboard via HTTP server on port 8888
 *   7. Shuts everything down cleanly
 */

/* Needed so <windows.h> exposes QueryProcessCycleTime (added in Vista). */
#define _WIN32_WINNT 0x0600

#include <time.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <tlhelp32.h>   /* For CreateToolhelp32Snapshot - Real thread counting */

#include "common.h"
#include "server/http_server.h"

#define TOTAL_TASKS 24

typedef struct {
    HANDLE hProcess;
    DWORD  pid;
    HANDLE hWriteToChild;   /* dispatcher writes tasks here (child's stdin)   */
    HANDLE hReadFromChild;  /* dispatcher reads results here (child's stdout) */
    HANDLE hReaderThread;

    /*
     * These counters are touched by TWO different threads: the main
     * thread increments tasksAssigned when it hands out work, and each
     * worker's private reader thread increments tasksCompleted when a
     * result arrives. Rather than wrap a CRITICAL_SECTION around a
     * single integer increment, we use the Interlocked* family -- these
     * compile to a single locked CPU instruction. It's the cheapest
     * correct way to share a counter between threads, and it's a nice
     * contrast to the queueLock in threadpool.c, which protects more
     * than one field at once and genuinely needs a real lock.
     */
    volatile LONG tasksAssigned;
    volatile LONG tasksCompleted;

    /*
     * tasksAssigned/tasksCompleted above are PER-LIFE counters: they get
     * reset to 0 every time this slot's process is (re)launched, which
     * is exactly right for computing "how much is queued right now" --
     * a freshly restarted process genuinely does have an empty queue.
     * But it means the dashboard's "Done" column would drop to 0 after
     * a crash+restart, silently hiding real work the PREVIOUS instance
     * finished. tasksCompletedTotal fixes that: it only ever increases,
     * survives restarts, and is what the dashboard actually displays.
     */
    volatile LONG tasksCompletedTotal;
} WorkerHandle;

static WorkerHandle g_workers[NUM_WORKERS];
static volatile LONG g_totalCompleted = 0;

/*
 * FEATURE: fault tolerance via crash detection + automatic restart.
 * When a worker dies unexpectedly (not because WE told it to QUIT), any
 * tasks that were still assigned to it and hadn't reported a result yet
 * are gone for good in this simple design -- there's no re-queue/retry
 * logic. We count them as "lost" so the dispatcher's completion check
 * still knows when to stop waiting, and we're upfront about it on the
 * dashboard instead of hiding it or hanging forever.
 */
static volatile LONG g_lostTasks = 0;

/*
 * A log of crash/restart events, printed once at the very end instead
 * of immediately when detected. Printing immediately mid-run doesn't
 * work reliably here: CheckWorkerHealth() runs inside the same loop as
 * DrawDashboard(), and later plain printf's (like "All tasks
 * completed...") can land on the exact same console row and silently
 * erase whatever was printed there moments earlier. Logging to memory
 * and printing the whole list at the end, after the dashboard has
 * stopped redrawing, guarantees you actually get to read it.
 */
#define MAX_RESTART_LOG 32
static char g_restartLog[MAX_RESTART_LOG][160];
static int  g_restartLogCount = 0;

/*
 * FEATURE: latency/throughput measurement.
 * g_dispatchTime[id] is stamped the instant a task is written to a pipe;
 * when its RESULT comes back, the gap between those two timestamps is
 * that task's round-trip LATENCY -- queue wait + compute + IPC, all of it.
 * Multiple reader threads (one per worker) can finish tasks at the same
 * moment and all want to update the running min/max/sum together, so
 * that aggregation step (not the timestamp itself) is protected by a
 * real CRITICAL_SECTION -- unlike the simple counters above, more than
 * one field has to move together here for the numbers to stay honest.
 */
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_dispatchTime[TOTAL_TASKS];
static CRITICAL_SECTION g_statsLock;
static double g_latencyMinMs = -1.0;
static double g_latencyMaxMs = 0.0;
static double g_latencySumMs = 0.0;
static LONG   g_latencyCount = 0;

/*
 * Per-task-type execution time tracking.
 * Aggregates execution time per command type (FACT, FIB, PRIME, etc.)
 * so we can show meaningful breakdown of which task types are fast/slow.
 */
static struct {
    char name[32];
    double totalMs;
    double minMs;
    double maxMs;
    LONG count;
} g_taskTypeStats[10];
static int g_taskTypeCount = 0;
static CRITICAL_SECTION g_taskTypeLock;

/* Global pointer to tasks array - used by ReaderThreadProc for task type stats */
static Task *g_tasks = NULL;

/*
 * Performance metrics collected per worker using Windows APIs.
 */
typedef struct {
    double totalCPUTimeMs;
    double userCPUTimeMs;
    double kernelCPUTimeMs;
    SIZE_T pageFaults;
    SIZE_T currentWorkingSetKB;
    SIZE_T peakWorkingSetKB;
    SIZE_T privateBytesKB;
    ULONGLONG bytesRead;
    ULONGLONG bytesWritten;
    DWORD ioOperations;
    DWORD threadCount;
} PerformanceMetrics;

/* ===================================================================
 * FORWARD DECLARATIONS - All functions used before their definitions
 * =================================================================== */
static DWORD GetProcessThreadCount(DWORD pid);
static void GetWorkerCpuAndCycles(HANDLE hp, double *cpuMsOut, double *cyclesMillionsOut);
static void SendCrashSignal(int workerIndex);
static void SendWebLog(const char* message);
static void SendWebDashboardUpdate(void);
static void BuildDashboardJSON(char* buffer, size_t bufferSize);
static void CollectPerformanceMetrics(HANDLE hProcess, PerformanceMetrics *pm);

/* ================ WEB DASHBOARD GLOBALS ================ */
static char g_jsonBuffer[16384];
static CRITICAL_SECTION g_jsonLock;
static LARGE_INTEGER g_batchStartTime;
static int g_webDashboardAvailable = 0;

/* ================ WEB DASHBOARD FUNCTIONS ================ */

/* Build JSON data for the web dashboard */
static void BuildDashboardJSON(char* buffer, size_t bufferSize) {
    EnterCriticalSection(&g_jsonLock);

    size_t offset = 0;
    offset += snprintf(buffer + offset, bufferSize - offset, "{");

    /* Workers */
    offset += snprintf(buffer + offset, bufferSize - offset, "\"workers\":[");
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (i > 0) offset += snprintf(buffer + offset, bufferSize - offset, ",");

        DWORD exitCode = 0;
        const char* status = "IDLE";
        if (g_workers[i].hProcess) {
            GetExitCodeProcess(g_workers[i].hProcess, &exitCode);
            status = (exitCode == STILL_ACTIVE) ? "RUNNING" : "EXITED";
        }

        DWORD priorityClass = GetPriorityClass(g_workers[i].hProcess);
        const char* prioStr = "Normal";
        if (priorityClass == ABOVE_NORMAL_PRIORITY_CLASS) prioStr = "AboveNorm";
        else if (priorityClass == BELOW_NORMAL_PRIORITY_CLASS) prioStr = "BelowNorm";
        else if (priorityClass == HIGH_PRIORITY_CLASS) prioStr = "High";
        else if (priorityClass == IDLE_PRIORITY_CLASS) prioStr = "Idle";

        double cpuMs = 0.0, cyclesMillions = 0.0;
        GetWorkerCpuAndCycles(g_workers[i].hProcess, &cpuMs, &cyclesMillions);

        PerformanceMetrics pm;
        CollectPerformanceMetrics(g_workers[i].hProcess, &pm);

        offset += snprintf(buffer + offset, bufferSize - offset,
            "{"
            "\"pid\":%lu,"
            "\"status\":\"%s\","
            "\"priority\":\"%s\","
            "\"tasksDone\":%ld,"
            "\"tasksQueued\":%ld,"
            "\"threadCount\":%ld,"
            "\"cpuMs\":%.1f,"
            "\"cyclesMillions\":%.1f,"
            "\"memoryKB\":%ld,"
            "\"pageFaults\":%ld"
            "}",
            g_workers[i].pid,
            status,
            prioStr,
            g_workers[i].tasksCompletedTotal,
            g_workers[i].tasksAssigned - g_workers[i].tasksCompleted,
            GetProcessThreadCount(g_workers[i].pid),
            cpuMs,
            cyclesMillions,
            (unsigned long)pm.currentWorkingSetKB,
            (unsigned long)pm.pageFaults);
    }
    offset += snprintf(buffer + offset, bufferSize - offset, "],");

    /* Statistics */
    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"totalTasks\":%d,", TOTAL_TASKS);
    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"completedTasks\":%ld,", g_totalCompleted);
    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"lostTasks\":%ld,", g_lostTasks);

    /* Latency */
    double avgLatency = (g_latencyCount > 0) ? g_latencySumMs / g_latencyCount : 0.0;
    double minLatency = (g_latencyMinMs < 0) ? 0.0 : g_latencyMinMs;
    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"latency\":{"
        "\"min\":%.1f,"
        "\"avg\":%.1f,"
        "\"max\":%.1f"
        "},",
        minLatency, avgLatency, g_latencyMaxMs);

    /* Throughput */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - g_batchStartTime.QuadPart) / (double)g_freq.QuadPart;
    double throughput = (elapsed > 0) ? (double)g_totalCompleted / elapsed : 0;
    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"throughput\":%.1f,", throughput);

    /* Task Types */
    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"taskTypes\":{");
    for (int i = 0; i < g_taskTypeCount; i++) {
        if (i > 0) offset += snprintf(buffer + offset, bufferSize - offset, ",");
        offset += snprintf(buffer + offset, bufferSize - offset,
            "\"%s\":%ld",
            g_taskTypeStats[i].name, g_taskTypeStats[i].count);
    }
    offset += snprintf(buffer + offset, bufferSize - offset, "},");

    /* ===== Detailed Metrics ===== */
    PerformanceMetrics pm[3];
    for (int i = 0; i < NUM_WORKERS; i++) {
        CollectPerformanceMetrics(g_workers[i].hProcess, &pm[i]);
    }

    double totalCPU = 0, totalUser = 0, totalKernel = 0;
    SIZE_T totalMemory = 0, totalPeak = 0, totalPageFaults = 0;
    DWORD totalThreads = 0, totalIO = 0;

    for (int i = 0; i < NUM_WORKERS; i++) {
        totalCPU += pm[i].totalCPUTimeMs;
        totalUser += pm[i].userCPUTimeMs;
        totalKernel += pm[i].kernelCPUTimeMs;
        totalMemory += pm[i].currentWorkingSetKB;
        totalPeak += pm[i].peakWorkingSetKB;
        totalPageFaults += pm[i].pageFaults;
        totalThreads += pm[i].threadCount;
        totalIO += pm[i].ioOperations;
    }

    offset += snprintf(buffer + offset, bufferSize - offset,
        "\"metrics\":{"
        "\"cpuTotal\":%.1f,\"cpuTotal1\":%.1f,\"cpuTotal2\":%.1f,\"cpuTotal3\":%.1f,"
        "\"userTotal\":%.1f,\"user1\":%.1f,\"user2\":%.1f,\"user3\":%.1f,"
        "\"kernelTotal\":%.1f,\"kernel1\":%.1f,\"kernel2\":%.1f,\"kernel3\":%.1f,"
        "\"memoryTotal\":%lu,\"memory1\":%lu,\"memory2\":%lu,\"memory3\":%lu,"
        "\"peakTotal\":%lu,\"peak1\":%lu,\"peak2\":%lu,\"peak3\":%lu,"
        "\"pageFaultsTotal\":%lu,\"pageFaults1\":%lu,\"pageFaults2\":%lu,\"pageFaults3\":%lu,"
        "\"threadsTotal\":%lu,\"threads1\":%lu,\"threads2\":%lu,\"threads3\":%lu,"
        "\"ioTotal\":%lu,\"io1\":%lu,\"io2\":%lu,\"io3\":%lu"
        "}",
        totalCPU, pm[0].totalCPUTimeMs, pm[1].totalCPUTimeMs, pm[2].totalCPUTimeMs,
        totalUser, pm[0].userCPUTimeMs, pm[1].userCPUTimeMs, pm[2].userCPUTimeMs,
        totalKernel, pm[0].kernelCPUTimeMs, pm[1].kernelCPUTimeMs, pm[2].kernelCPUTimeMs,
        (unsigned long)totalMemory, (unsigned long)pm[0].currentWorkingSetKB,
        (unsigned long)pm[1].currentWorkingSetKB, (unsigned long)pm[2].currentWorkingSetKB,
        (unsigned long)totalPeak, (unsigned long)pm[0].peakWorkingSetKB,
        (unsigned long)pm[1].peakWorkingSetKB, (unsigned long)pm[2].peakWorkingSetKB,
        (unsigned long)totalPageFaults, (unsigned long)pm[0].pageFaults,
        (unsigned long)pm[1].pageFaults, (unsigned long)pm[2].pageFaults,
        (unsigned long)totalThreads, (unsigned long)pm[0].threadCount,
        (unsigned long)pm[1].threadCount, (unsigned long)pm[2].threadCount,
        (unsigned long)totalIO, (unsigned long)pm[0].ioOperations,
        (unsigned long)pm[1].ioOperations, (unsigned long)pm[2].ioOperations);

    offset += snprintf(buffer + offset, bufferSize - offset, "}");

    LeaveCriticalSection(&g_jsonLock);
}

/* Send update to web dashboard */
static void SendWebDashboardUpdate(void) {
    if (!g_webDashboardAvailable) return;
    BuildDashboardJSON(g_jsonBuffer, sizeof(g_jsonBuffer));
    SendDashboardUpdate(g_jsonBuffer);
}

/* Send log message to web dashboard */
static void SendWebLog(const char* message) {
    if (!g_webDashboardAvailable || !message) return;
    SendLogToDashboard(message);
}

/* ===== Crash Injection Callback for Web Dashboard ===== */
static void WebCrashCallback(void) {
    /* Crash Worker 2 (index 1) */
    if (NUM_WORKERS > 1) {
        SendCrashSignal(1);
        printf("[Web] Crash injected into Worker 2 via dashboard button\n");
        SendWebLog("💥 Crash injected via web dashboard!");
    }
}

/* ================ END WEB DASHBOARD FUNCTIONS ================ */

/*
 * Updates per-task-type execution time statistics.
 * Called from ReaderThreadProc when a RESULT arrives.
 * Thread-safe using g_taskTypeLock.
 */
static void UpdateTaskTypeStats(int taskId, double executionMs) {
    if (taskId < 0 || taskId >= TOTAL_TASKS || !g_tasks) return;

    EnterCriticalSection(&g_taskTypeLock);

    char *cmd = g_tasks[taskId].command;
    int found = 0;

    for (int i = 0; i < g_taskTypeCount; i++) {
        if (strcmp(g_taskTypeStats[i].name, cmd) == 0) {
            g_taskTypeStats[i].totalMs += executionMs;
            g_taskTypeStats[i].count++;
            if (executionMs < g_taskTypeStats[i].minMs) {
                g_taskTypeStats[i].minMs = executionMs;
            }
            if (executionMs > g_taskTypeStats[i].maxMs) {
                g_taskTypeStats[i].maxMs = executionMs;
            }
            found = 1;
            break;
        }
    }

    if (!found && g_taskTypeCount < 10) {
        strcpy(g_taskTypeStats[g_taskTypeCount].name, cmd);
        g_taskTypeStats[g_taskTypeCount].totalMs = executionMs;
        g_taskTypeStats[g_taskTypeCount].minMs = executionMs;
        g_taskTypeStats[g_taskTypeCount].maxMs = executionMs;
        g_taskTypeStats[g_taskTypeCount].count = 1;
        g_taskTypeCount++;
    }

    LeaveCriticalSection(&g_taskTypeLock);
}

/* ------------------------------------------------------------------ *
 * Spawning a worker process with its stdin/stdout redirected to pipes
 * ------------------------------------------------------------------ */
static BOOL CreateWorkerProcess(int index) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE childStdinRead, childStdinWrite;
    HANDLE childStdoutRead, childStdoutWrite;

    if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)) return FALSE;
    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) return FALSE;

    SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = childStdinRead;
    si.hStdOutput = childStdoutWrite;
    si.hStdError  = childStdoutWrite;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcess(
        NULL,
        "worker.exe",
        NULL, NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL, NULL,
        &si, &pi
    );

    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);

    if (!ok) {
        fprintf(stderr, "CreateProcess failed for worker %d (error %lu)\n",
                index, GetLastError());
        return FALSE;
    }

    CloseHandle(pi.hThread);

    SetPriorityClass(pi.hProcess,
        (index == 0) ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);

    g_workers[index].hProcess      = pi.hProcess;
    g_workers[index].pid           = pi.dwProcessId;
    g_workers[index].hWriteToChild = childStdinWrite;
    g_workers[index].hReadFromChild= childStdoutRead;
    g_workers[index].tasksAssigned = 0;
    g_workers[index].tasksCompleted= 0;
    return TRUE;
}

/* ------------------------------------------------------------------ *
 * One reader thread per worker: blocks on ReadFile until a RESULT line
 * arrives, then updates that worker's completed counter.
 * ------------------------------------------------------------------ */
static DWORD WINAPI ReaderThreadProc(LPVOID param) {
    int index = (int)(intptr_t)param;
    WorkerHandle *w = &g_workers[index];

    char chunk[512];
    char lineBuf[MAX_LINE];
    int  lineLen = 0;

    for (;;) {
        DWORD nread = 0;
        BOOL ok = ReadFile(w->hReadFromChild, chunk, sizeof(chunk), &nread, NULL);
        if (!ok || nread == 0) break;

        for (DWORD i = 0; i < nread; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (lineLen > 0) {
                    lineBuf[lineLen] = '\0';
                    int id; long val; double execMs;
                    if (sscanf(lineBuf, "RESULT %d %ld %lf", &id, &val, &execMs) == 3) {
                        if (id >= 0 && id < TOTAL_TASKS) {
                            LARGE_INTEGER now;
                            QueryPerformanceCounter(&now);
                            double ms = (double)(now.QuadPart - g_dispatchTime[id].QuadPart)
                                        * 1000.0 / (double)g_freq.QuadPart;

                            EnterCriticalSection(&g_statsLock);
                            g_latencySumMs += ms;
                            g_latencyCount++;
                            if (g_latencyMinMs < 0.0 || ms < g_latencyMinMs) g_latencyMinMs = ms;
                            if (ms > g_latencyMaxMs) g_latencyMaxMs = ms;
                            LeaveCriticalSection(&g_statsLock);

                            UpdateTaskTypeStats(id, execMs);
                        }
                        InterlockedIncrement(&w->tasksCompleted);
                        InterlockedIncrement(&w->tasksCompletedTotal);
                        InterlockedIncrement(&g_totalCompleted);

                        SendWebDashboardUpdate();
                    }
                    lineLen = 0;
                }
            } else if (lineLen < MAX_LINE - 1) {
                lineBuf[lineLen++] = c;
            }
        }
    }
    return 0;
}

/*
 * Pick the worker with the fewest OUTSTANDING tasks (assigned - completed).
 */
static int g_nextWorkerHint = 0;

static int PickLeastBusyWorker(void) {
    int best = g_nextWorkerHint;
    LONG bestOutstanding = g_workers[best].tasksAssigned - g_workers[best].tasksCompleted;

    for (int step = 1; step < NUM_WORKERS; step++) {
        int i = (g_nextWorkerHint + step) % NUM_WORKERS;
        LONG outstanding = g_workers[i].tasksAssigned - g_workers[i].tasksCompleted;
        if (outstanding < bestOutstanding) {
            bestOutstanding = outstanding;
            best = i;
        }
    }

    g_nextWorkerHint = (best + 1) % NUM_WORKERS;
    return best;
}

static void SendTask(int workerIndex, Task t) {
    char line[MAX_LINE];
    int len = sprintf(line, "TASK %d %s %ld\n", t.taskId, t.command, t.value);
    DWORD written;
    QueryPerformanceCounter(&g_dispatchTime[t.taskId]);
    WriteFile(g_workers[workerIndex].hWriteToChild, line, (DWORD)len, &written, NULL);
    InterlockedIncrement(&g_workers[workerIndex].tasksAssigned);
}

/*
 * Sends a deliberate fault-injection signal, NOT a real unit of work.
 */
static void SendCrashSignal(int workerIndex) {
    const char *line = "TASK -1 CRASH 0\n";
    DWORD written;
    WriteFile(g_workers[workerIndex].hWriteToChild, line, (DWORD)strlen(line), &written, NULL);
}

/*
 * Tears down a dead worker's old handles and launches a fresh copy in
 * the same slot.
 */
static void RestartWorker(int index) {
    CloseHandle(g_workers[index].hWriteToChild);
    WaitForSingleObject(g_workers[index].hReaderThread, 2000);
    CloseHandle(g_workers[index].hReaderThread);
    CloseHandle(g_workers[index].hReadFromChild);
    CloseHandle(g_workers[index].hProcess);

    if (!CreateWorkerProcess(index)) {
        fprintf(stderr, "Failed to restart worker %d -- giving up on this slot.\n", index);
        return;
    }
    g_workers[index].hReaderThread = CreateThread(
        NULL, 0, ReaderThreadProc, (LPVOID)(intptr_t)index, 0, NULL);
}

/*
 * Called periodically from the main loop. Checks every worker's real OS
 * exit status; if one has died and we never told it to, that's a crash.
 */
static void CheckWorkerHealth(void) {
    for (int i = 0; i < NUM_WORKERS; i++) {
        DWORD exitCode = 0;
        GetExitCodeProcess(g_workers[i].hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) {
            LONG lost = g_workers[i].tasksAssigned - g_workers[i].tasksCompleted;
            if (lost < 0) lost = 0;
            InterlockedExchangeAdd(&g_lostTasks, lost);

            if (g_restartLogCount < MAX_RESTART_LOG) {
                snprintf(g_restartLog[g_restartLogCount], sizeof(g_restartLog[0]),
                    "Worker %d (PID %lu) exited unexpectedly (code %lu). "
                    "%ld in-flight task(s) lost. Restarted.",
                    i + 1, g_workers[i].pid, exitCode, lost);
                g_restartLogCount++;

                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg), "Worker %d crashed! (PID %lu)", i + 1, g_workers[i].pid);
                SendWebLog(logMsg);
            }

            RestartWorker(i);
            SendWebDashboardUpdate();
        }
    }
}

/*
 * Shared by TraceWorker() and the GUI update code -- computes the same
 * real CPU-time and cycle numbers.
 */
static void GetWorkerCpuAndCycles(HANDLE hp, double *cpuMsOut, double *cyclesMillionsOut) {
    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    double cpuMs = 0.0;
    if (GetProcessTimes(hp, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER k, u;
        k.LowPart = ftKernel.dwLowDateTime; k.HighPart = ftKernel.dwHighDateTime;
        u.LowPart = ftUser.dwLowDateTime;   u.HighPart = ftUser.dwHighDateTime;
        cpuMs = (double)(k.QuadPart + u.QuadPart) / 10000.0;
    }
    ULONG64 cycles = 0;
    QueryProcessCycleTime(hp, &cycles);
    *cpuMsOut = cpuMs;
    *cyclesMillionsOut = (double)cycles / 1000000.0;
}

/* ------------------------------------------------------------------ *
 * PROCESS TRACING -- documented Win32 APIs only.
 * ------------------------------------------------------------------ */
static void TraceWorker(int index, char *outLine, size_t outLineSize) {
    HANDLE hp = g_workers[index].hProcess;

    DWORD exitCode = 0;
    GetExitCodeProcess(hp, &exitCode);
    const char *status = (exitCode == STILL_ACTIVE) ? "RUNNING" : "EXITED";

    DWORD priorityClass = GetPriorityClass(hp);
    const char *prioStr = "?";
    switch (priorityClass) {
        case IDLE_PRIORITY_CLASS:         prioStr = "Idle";     break;
        case BELOW_NORMAL_PRIORITY_CLASS: prioStr = "BelowNorm";break;
        case NORMAL_PRIORITY_CLASS:       prioStr = "Normal";   break;
        case ABOVE_NORMAL_PRIORITY_CLASS: prioStr = "AboveNorm";break;
        case HIGH_PRIORITY_CLASS:         prioStr = "High";     break;
        default:                          prioStr = "Unknown";  break;
    }

    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    double cpuMs = 0.0;
    if (GetProcessTimes(hp, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER k, u;
        k.LowPart = ftKernel.dwLowDateTime; k.HighPart = ftKernel.dwHighDateTime;
        u.LowPart = ftUser.dwLowDateTime;   u.HighPart = ftUser.dwHighDateTime;
        cpuMs = (double)(k.QuadPart + u.QuadPart) / 10000.0;
    }

    ULONG64 cycles = 0;
    QueryProcessCycleTime(hp, &cycles);
    double cyclesMillions = (double)cycles / 1000000.0;

    LONG assigned    = g_workers[index].tasksAssigned;
    LONG completed   = g_workers[index].tasksCompleted;
    LONG totalDone   = g_workers[index].tasksCompletedTotal;

    snprintf(outLine, outLineSize,
        "Worker %d (PID %-6lu) [%-7s] Priority:%-9s CPU:%6.1fms Cycles:%7.1fM  Done:%-3ld Queued:%-3ld",
        index + 1, g_workers[index].pid, status, prioStr, cpuMs, cyclesMillions,
        totalDone, assigned - completed);
}

/*
 * Collects detailed performance metrics for a given process.
 */
static void CollectPerformanceMetrics(HANDLE hProcess, PerformanceMetrics *pm) {
    ZeroMemory(pm, sizeof(PerformanceMetrics));

    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    if (GetProcessTimes(hProcess, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER k, u;
        k.LowPart = ftKernel.dwLowDateTime; k.HighPart = ftKernel.dwHighDateTime;
        u.LowPart = ftUser.dwLowDateTime;   u.HighPart = ftUser.dwHighDateTime;
        pm->kernelCPUTimeMs = (double)k.QuadPart / 10000.0;
        pm->userCPUTimeMs = (double)u.QuadPart / 10000.0;
        pm->totalCPUTimeMs = pm->kernelCPUTimeMs + pm->userCPUTimeMs;
    }

    SIZE_T workingSetSize, peakWorkingSetSize;
    if (GetProcessWorkingSetSize(hProcess, &workingSetSize, &peakWorkingSetSize)) {
        pm->currentWorkingSetKB = workingSetSize / 1024;
        pm->peakWorkingSetKB = peakWorkingSetSize / 1024;
    }

    IO_COUNTERS ioCounters;
    if (GetProcessIoCounters(hProcess, &ioCounters)) {
        pm->bytesRead = ioCounters.ReadTransferCount;
        pm->bytesWritten = ioCounters.WriteTransferCount;
        pm->ioOperations = ioCounters.ReadOperationCount + ioCounters.WriteOperationCount;
    }

    DWORD pid = GetProcessId(hProcess);
    pm->threadCount = GetProcessThreadCount(pid);
}

/*
 * Counts actual threads in a process using Toolhelp32 API.
 */
static DWORD GetProcessThreadCount(DWORD pid) {
    DWORD threadCount = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(THREADENTRY32);
        if (Thread32First(hSnapshot, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) {
                    threadCount++;
                }
            } while (Thread32Next(hSnapshot, &te));
        }
        CloseHandle(hSnapshot);
    }
    return threadCount;
}

/* Draw the whole dashboard in the console */
static void DrawDashboard(void) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD topLeft = {0, 0};
    SetConsoleCursorPosition(hConsole, topLeft);

    printf("==================================================================\n");
    printf(" MULTI-LEVEL TASK EXECUTOR - LIVE DASHBOARD                       \n");
    printf("==================================================================\n");
    for (int i = 0; i < NUM_WORKERS; i++) {
        char line[160];
        TraceWorker(i, line, sizeof(line));
        printf(" %s\n", line);

        PerformanceMetrics pm;
        CollectPerformanceMetrics(g_workers[i].hProcess, &pm);
        printf("    Memory:%5luKB (Peak:%5luKB) Private:%5luKB PF:%4lu I/O:%4luKB Threads:%2lu I/O Ops:%4lu\n",
           (unsigned long)pm.currentWorkingSetKB,
           (unsigned long)pm.peakWorkingSetKB,
           (unsigned long)pm.privateBytesKB,
           (unsigned long)pm.pageFaults,
           (unsigned long)(pm.bytesRead / 1024 + pm.bytesWritten / 1024),
           pm.threadCount,
           (unsigned long)(pm.ioOperations));
    }
    printf("------------------------------------------------------------------\n");
    printf(" Total completed: %3ld / %d   (lost to crash: %ld)                 \n",
           g_totalCompleted, TOTAL_TASKS, g_lostTasks);
    if (g_latencyCount > 0) {
        double avgMs = g_latencySumMs / g_latencyCount;
        printf(" Avg Latency: %5.1fms   Min: %5.1fms   Max: %5.1fms             \n",
               avgMs, g_latencyMinMs, g_latencyMaxMs);
    }
    printf("==================================================================\n");
}

/*
 * A plain, SEQUENTIAL print of final worker status.
 */
static void PrintFinalWorkerStatus(void) {
    printf("\nFinal worker status:\n");
    for (int i = 0; i < NUM_WORKERS; i++) {
        char line[160];
        TraceWorker(i, line, sizeof(line));
        printf(" %s\n", line);
    }
}

int main(void) {
    QueryPerformanceFrequency(&g_freq);
    InitializeCriticalSection(&g_statsLock);
    InitializeCriticalSection(&g_taskTypeLock);
    InitializeCriticalSection(&g_jsonLock);

    printf("Starting %d worker processes, %d threads each...\n",
           NUM_WORKERS, THREADS_PER_WORKER);

    for (int i = 0; i < NUM_WORKERS; i++) {
        if (!CreateWorkerProcess(i)) {
            fprintf(stderr, "Failed to start worker %d, aborting.\n", i);
            return 1;
        }
        g_workers[i].hReaderThread = CreateThread(
            NULL, 0, ReaderThreadProc, (LPVOID)(intptr_t)i, 0, NULL);
    }

    /* ===== START WEB DASHBOARD SERVER ===== */
    printf("[Web] Starting HTTP server on port 8888...\n");
    StartHTTPServer();
    RegisterCrashCallback(WebCrashCallback);
    g_webDashboardAvailable = 1;

    printf("[Web] Opening dashboard in browser...\n");
    OpenDashboard();
    printf("[Web] Dashboard available at http://localhost:8888/dashboard.html\n");

    SendWebLog("Task Executor started");
    SendWebLog("3 worker processes launched");

    /* Generate a mixed batch of CPU-bound tasks. */
    const char *commands[] = {"FACT", "FIB", "PRIME", "SUMSQ", "MATRIX", "SORT", "CRYPTO"};
    srand((unsigned)time(NULL));

    Task tasks[TOTAL_TASKS];
    for (int i = 0; i < TOTAL_TASKS; i++) {
        tasks[i].taskId = i;
        strcpy(tasks[i].command, commands[rand() % 7]);
        tasks[i].value = 10 + rand() % 25;
    }

    g_tasks = tasks;

    LARGE_INTEGER batchStart, batchEnd;
    QueryPerformanceCounter(&batchStart);
    g_batchStartTime = batchStart;

    /* ===== DISPATCH LOOP ===== */
    int tasksDispatched = 0;

    while (tasksDispatched < TOTAL_TASKS) {
        /* Dispatch tasks one by one */
        int target = PickLeastBusyWorker();
        SendTask(target, tasks[tasksDispatched]);

        /* Crash injection at halfway point */
        if (tasksDispatched == TOTAL_TASKS / 2) {
            SendCrashSignal(1);
            SendWebLog("💥 Crash injected into Worker 2!");
            printf("[System] Crash injected into Worker 2 at halfway point\n");
        }

        CheckWorkerHealth();

        /* Send update to web dashboard */
        SendWebDashboardUpdate();

        /* Console dashboard */
        DrawDashboard();

        Sleep(150);
        tasksDispatched++;
    }

    /* Wait for all tasks to complete */
    while ((LONG)g_totalCompleted + g_lostTasks < TOTAL_TASKS) {
        CheckWorkerHealth();
        SendWebDashboardUpdate();
        DrawDashboard();
        Sleep(200);
    }

    /* One last check */
    CheckWorkerHealth();
    QueryPerformanceCounter(&batchEnd);

    /* Send final web dashboard updates */
    SendWebLog("All tasks completed successfully!");
    SendWebDashboardUpdate();

    printf("\nAll tasks completed. Shutting down workers...\n");

    if (g_restartLogCount > 0) {
        printf("\nCrash/restart events (%d):\n", g_restartLogCount);
        for (int j = 0; j < g_restartLogCount; j++) {
            printf("  [!] %s\n", g_restartLog[j]);
        }
    } else {
        printf("\nNo worker crashes occurred this run.\n");
    }

    printf("\n=== PERFORMANCE SUMMARY ===\n");
    if (g_latencyCount > 0) {
        double avgMs = g_latencySumMs / g_latencyCount;
        printf("  Latency    - min: %.1fms   avg: %.1fms   max: %.1fms  (over %ld completed tasks)\n",
               g_latencyMinMs, avgMs, g_latencyMaxMs, g_latencyCount);
    } else {
        printf("  Latency    - no tasks completed, nothing to report.\n");
    }
    {
        double elapsedSec = (double)(batchEnd.QuadPart - batchStart.QuadPart) / (double)g_freq.QuadPart;
        double throughput = (elapsedSec > 0.0) ? (double)g_totalCompleted / elapsedSec : 0.0;
        printf("  Throughput - %.1f tasks/sec, %ld tasks in %.2fs total\n",
               throughput, g_totalCompleted, elapsedSec);
    }

    /* Print task type breakdown */
    printf("\n  Task Type Breakdown (with execution times):\n");
    printf("  %-12s %8s %10s %10s %10s\n", "Type", "Count", "Avg(ms)", "Min(ms)", "Max(ms)");
    printf("  %-12s %8s %10s %10s %10s\n", "----", "-----", "-------", "-------", "-------");

    for (int i = 0; i < g_taskTypeCount; i++) {
        double avg = g_taskTypeStats[i].count > 0 ?
                     g_taskTypeStats[i].totalMs / g_taskTypeStats[i].count : 0;
        printf("  %-12s %8ld %10.1f %10.1f %10.1f\n",
               g_taskTypeStats[i].name,
               g_taskTypeStats[i].count,
               avg,
               g_taskTypeStats[i].minMs,
               g_taskTypeStats[i].maxMs);
    }

    /* Detailed performance metrics */
    printf("\n=== DETAILED PERFORMANCE METRICS ===\n");
    printf("Metric            | Worker 1  | Worker 2  | Worker 3  | Total\n");
    printf("------------------+-----------+-----------+-----------+--------\n");

    PerformanceMetrics pm[3];
    for (int i = 0; i < NUM_WORKERS; i++) {
        CollectPerformanceMetrics(g_workers[i].hProcess, &pm[i]);
    }

    double totalCPU = 0, totalUser = 0, totalKernel = 0;
    SIZE_T totalPageFaults = 0, totalMemory = 0, totalPeak = 0;
    ULONGLONG totalIO = 0;
    DWORD totalThreads = 0, totalSwitches = 0;

    for (int i = 0; i < NUM_WORKERS; i++) {
        totalCPU += pm[i].totalCPUTimeMs;
        totalUser += pm[i].userCPUTimeMs;
        totalKernel += pm[i].kernelCPUTimeMs;
        totalPageFaults += pm[i].pageFaults;
        totalMemory += pm[i].currentWorkingSetKB;
        totalPeak += pm[i].peakWorkingSetKB;
        totalIO += pm[i].bytesRead / 1024 + pm[i].bytesWritten / 1024;
        totalThreads += pm[i].threadCount;
        totalSwitches += pm[i].ioOperations;
    }

    printf("CPU Total (ms)    | %9.1f | %9.1f | %9.1f | %8.1f\n",
           pm[0].totalCPUTimeMs, pm[1].totalCPUTimeMs, pm[2].totalCPUTimeMs, totalCPU);
    printf("User Time (ms)    | %9.1f | %9.1f | %9.1f | %8.1f\n",
           pm[0].userCPUTimeMs, pm[1].userCPUTimeMs, pm[2].userCPUTimeMs, totalUser);
    printf("Kernel Time (ms)  | %9.1f | %9.1f | %9.1f | %8.1f\n",
           pm[0].kernelCPUTimeMs, pm[1].kernelCPUTimeMs, pm[2].kernelCPUTimeMs, totalKernel);
    printf("Memory (KB)       | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)pm[0].currentWorkingSetKB,
           (unsigned long)pm[1].currentWorkingSetKB,
           (unsigned long)pm[2].currentWorkingSetKB,
           (unsigned long)totalMemory);
    printf("Peak Memory (KB)  | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)pm[0].peakWorkingSetKB,
           (unsigned long)pm[1].peakWorkingSetKB,
           (unsigned long)pm[2].peakWorkingSetKB,
           (unsigned long)totalPeak);
    printf("Page Faults       | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)pm[0].pageFaults,
           (unsigned long)pm[1].pageFaults,
           (unsigned long)pm[2].pageFaults,
           (unsigned long)totalPageFaults);
    printf("Private Bytes(KB) | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)pm[0].privateBytesKB,
           (unsigned long)pm[1].privateBytesKB,
           (unsigned long)pm[2].privateBytesKB,
           (unsigned long)(pm[0].privateBytesKB + pm[1].privateBytesKB + pm[2].privateBytesKB));
    printf("Threads           | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)pm[0].threadCount,
           (unsigned long)pm[1].threadCount,
           (unsigned long)pm[2].threadCount,
           (unsigned long)totalThreads);
    printf("I/O Total (KB)    | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)(pm[0].bytesRead / 1024 + pm[0].bytesWritten / 1024),
           (unsigned long)(pm[1].bytesRead / 1024 + pm[1].bytesWritten / 1024),
           (unsigned long)(pm[2].bytesRead / 1024 + pm[2].bytesWritten / 1024),
           (unsigned long)totalIO);
    printf("I/O Operations    | %9lu | %9lu | %9lu | %8lu\n",
           (unsigned long)pm[0].ioOperations,
           (unsigned long)pm[1].ioOperations,
           (unsigned long)pm[2].ioOperations,
           (unsigned long)totalSwitches);
    printf("------------------+-----------+-----------+-----------+--------\n");

    /* Shutdown workers */
    for (int i = 0; i < NUM_WORKERS; i++) {
        const char *quit = "QUIT\n";
        DWORD written;
        WriteFile(g_workers[i].hWriteToChild, quit, (DWORD)strlen(quit), &written, NULL);
        CloseHandle(g_workers[i].hWriteToChild);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        WaitForSingleObject(g_workers[i].hProcess, 3000);
        WaitForSingleObject(g_workers[i].hReaderThread, 3000);
    }

    PrintFinalWorkerStatus();

    for (int i = 0; i < NUM_WORKERS; i++) {
        CloseHandle(g_workers[i].hReaderThread);
        CloseHandle(g_workers[i].hReadFromChild);
        CloseHandle(g_workers[i].hProcess);
    }

    /* Send final web log and stop server */
    SendWebLog("All workers exited cleanly");

    printf("[Web] Stopping HTTP server...\n");
    StopHTTPServer();

    printf("Done. All workers exited cleanly.\n");
    DeleteCriticalSection(&g_statsLock);
    DeleteCriticalSection(&g_taskTypeLock);
    DeleteCriticalSection(&g_jsonLock);
    return 0;
}
