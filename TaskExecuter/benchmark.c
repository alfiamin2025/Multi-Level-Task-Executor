/*
 * benchmark.c  ->  compiles to benchmark.exe
 * ---------------------------------------------
 * Standalone demo that directly measures the claim your project relies
 * on: "reusing a small pool of threads is more efficient than creating
 * a brand-new thread for every task."
 *
 * It runs the exact same number of tiny, identical tasks two different
 * ways and times each with a high-resolution counter:
 *
 *   Approach A: create a brand-new thread for every single task, wait
 *               for it, destroy it. (The "naive" way -- what your
 *               previous project pointed out you should NOT do for
 *               small, frequent tasks.)
 *   Approach B: 4 threads are created ONCE, stay alive the whole time,
 *               and simply get handed task after task. (What
 *               threadpool.c in the main project actually does.)
 *
 * The "work" itself is identical and deliberately tiny in both cases,
 * so whatever time difference shows up is coming purely from thread
 * creation/teardown overhead, not from the work.
 *
 * This file is fully self-contained -- it doesn't depend on any other
 * file in the project, so it can be its own build target.
 */

#include <windows.h>
#include <stdio.h>

#define BENCH_TASKS   5000
#define POOL_THREADS     4

/* Deliberately tiny, identical "work" for both approaches. */
static void DoTinyWork(void) {
    volatile long sum = 0;
    for (long i = 0; i < 2000; i++) sum += i;
}

/* ================= Approach A: one thread per task ================= */

static DWORD WINAPI OneShotThreadProc(LPVOID param) {
    (void)param;
    DoTinyWork();
    return 0;
}

static double RunThreadPerTask(void) {
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    for (int i = 0; i < BENCH_TASKS; i++) {
        HANDLE h = CreateThread(NULL, 0, OneShotThreadProc, NULL, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    QueryPerformanceCounter(&end);
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

/* ================= Approach B: small persistent pool ================= */

typedef struct {
    HANDLE threads[POOL_THREADS];
    HANDLE taskAvailable;    /* semaphore: how many tasks are ready to grab */
    volatile LONG completed;
    volatile LONG shuttingDown;
} MiniPool;

static DWORD WINAPI PoolThreadProc(LPVOID param) {
    MiniPool *pool = (MiniPool *)param;
    for (;;) {
        WaitForSingleObject(pool->taskAvailable, INFINITE);
        if (pool->shuttingDown) break;
        DoTinyWork();
        InterlockedIncrement(&pool->completed);
    }
    return 0;
}

static double RunThreadPool(void) {
    MiniPool pool;
    pool.taskAvailable = CreateSemaphore(NULL, 0, BENCH_TASKS + POOL_THREADS, NULL);
    pool.completed = 0;
    pool.shuttingDown = 0;

    /* The only 4 CreateThread calls in this entire approach. */
    for (int i = 0; i < POOL_THREADS; i++) {
        pool.threads[i] = CreateThread(NULL, 0, PoolThreadProc, &pool, 0, NULL);
    }

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    for (int i = 0; i < BENCH_TASKS; i++) {
        ReleaseSemaphore(pool.taskAvailable, 1, NULL);
    }
    while (pool.completed < BENCH_TASKS) {
        Sleep(0); /* yield without burning a core spinning */
    }

    QueryPerformanceCounter(&end);

    pool.shuttingDown = 1;
    ReleaseSemaphore(pool.taskAvailable, POOL_THREADS, NULL); /* wake all to exit */
    WaitForMultipleObjects(POOL_THREADS, pool.threads, TRUE, INFINITE);
    for (int i = 0; i < POOL_THREADS; i++) CloseHandle(pool.threads[i]);
    CloseHandle(pool.taskAvailable);

    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

int main(void) {
    printf("Benchmarking %d tiny tasks, two ways...\n\n", BENCH_TASKS);

    double poolMs = RunThreadPool();
    printf("Approach B (persistent pool, %d threads created ONCE):  %.2f ms\n",
           POOL_THREADS, poolMs);

    double naiveMs = RunThreadPerTask();
    printf("Approach A (new thread per task, %d threads total):     %.2f ms\n",
           BENCH_TASKS, naiveMs);

    printf("\nThe pool was %.1fx faster for the exact same work.\n", naiveMs / poolMs);
    printf("CreateThread/ExitThread actually ran %d times vs just %d.\n",
           BENCH_TASKS, POOL_THREADS);

    /*
     * Running this .exe directly (double-click, or outside Code::Blocks'
     * own console-runner) creates a console window that Windows destroys
     * the instant main() returns -- so the results flash and vanish.
     * Waiting for a keypress here makes the window stick around no
     * matter how the program was launched.
     */
    printf("\nPress Enter to exit...");
    getchar();

    return 0;
}
