/*
 * threadpool.c
 * ------------
 * The actual thread pool logic. This is the file where you'll learn the
 * most about synchronization, so every step is commented.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "threadpool.h"

/* ---- Per-thread random number generator to avoid data races -------------
 * FIX: Instead of using srand()/rand() which use a single global seed
 * and race when multiple threads call them simultaneously, each thread
 * gets its own seeded generator. The seed combines time + thread ID to
 * ensure uniqueness without any shared state.
 * ------------------------------------------------------------------- */
static unsigned int GetThreadRandom(unsigned int threadId) {
    /* Combine time, thread ID, and a constant to get unique seed */
    unsigned int seed = (unsigned int)time(NULL) ^ (threadId * 0x9E3779B9) ^ 0x12345678;
    /* Simple xorshift to generate random numbers */
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* ---- the "real work" the threads do -----------------------------------
 * These are intentionally simple, CPU-bound functions. The point isn't
 * the math, it's that they take a measurable amount of time so you can
 * SEE threads working in parallel on your dashboard.
 * ------------------------------------------------------------------- */

static long Fibonacci(long n) {
    if (n < 2) return n;
    return Fibonacci(n - 1) + Fibonacci(n - 2); /* deliberately naive/slow */
}

static int IsPrime(long n) {
    if (n < 2) return 0;
    for (long i = 2; i * i <= n; i++) {
        if (n % i == 0) return 0;
    }
    return 1;
}

/* NEW: Matrix multiplication - simulates scientific computing workload */
static long MatrixMultiply(long size, unsigned int *seed) {
    /* Limit size to avoid stack overflow */
    if (size > 30) size = 30;
    if (size < 5) size = 5;

    /* Use heap allocation for larger matrices */
    int *A = (int*)malloc(size * size * sizeof(int));
    int *B = (int*)malloc(size * size * sizeof(int));
    int *C = (int*)malloc(size * size * sizeof(int));

    if (!A || !B || !C) {
        free(A); free(B); free(C);
        return -1;
    }

    /* Initialize matrices with random values using per-thread seed */
    for (int i = 0; i < size * size; i++) {
        /* Simple random using the seed */
        *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
        A[i] = *seed % 10;
        *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
        B[i] = *seed % 10;
        C[i] = 0;
    }

    /* Matrix multiplication - CPU intensive */
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            int sum = 0;
            for (int k = 0; k < size; k++) {
                sum += A[i * size + k] * B[k * size + j];
            }
            C[i * size + j] = sum;
        }
    }

    long result = C[0]; /* Return first element as sample */
    free(A); free(B); free(C);
    return result;
}

/* NEW: Sort large array - simulates data processing workload */
static long SortArray(long size, unsigned int *seed) {
    if (size > 5000) size = 5000;
    if (size < 100) size = 100;

    int *arr = (int*)malloc(size * sizeof(int));
    if (!arr) return -1;

    /* Initialize array with random values using per-thread seed */
    for (int i = 0; i < size; i++) {
        *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
        arr[i] = *seed % 10000;
    }

    /* Bubble sort - intentionally inefficient to show CPU usage */
    for (int i = 0; i < size - 1; i++) {
        for (int j = 0; j < size - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }

    long result = arr[size / 2]; /* Return median */
    free(arr);
    return result;
}

/* NEW: Simple encryption - simulates cryptographic workload */
static long CryptoSHA(long value) {
    /* Simple hash-like computation to simulate encryption */
    unsigned long hash = (unsigned long)value;
    for (int i = 0; i < 10000; i++) {
        hash = (hash * 31 + 17) ^ (hash >> 3);
        hash ^= (hash << 5);
        hash = hash % 1000000007;
    }
    return (long)hash;
}

static long ExecuteTask(const Task *t, unsigned int *seed) {
    if (strcmp(t->command, "CRASH") == 0) {
        /*
         * DELIBERATE FAULT INJECTION -- for demonstrating crash recovery.
         * Dereferencing a NULL pointer triggers an access violation,
         * which Windows has no handler for here, so it terminates this
         * entire process immediately. This is intentional: it simulates
         * the kind of real bug (null pointer, bad array index, etc.)
         * that could happen in production code, so the dispatcher's
         * crash-detection + auto-restart logic has something real to
         * detect and react to.
         */
        int *crashPtr = NULL;
        *crashPtr = 1;
        return 0; /* unreachable */
    }
    if (strcmp(t->command, "FACT") == 0) {
        long result = 1;
        for (long i = 2; i <= t->value; i++) result *= i;
        return result;
    }
    if (strcmp(t->command, "FIB") == 0) {
        return Fibonacci(t->value);
    }
    if (strcmp(t->command, "PRIME") == 0) {
        return IsPrime(t->value);
    }
    if (strcmp(t->command, "SUMSQ") == 0) {
        long sum = 0;
        for (long i = 1; i <= t->value; i++) sum += i * i;
        return sum;
    }
    /* NEW: Matrix multiplication */
    if (strcmp(t->command, "MATRIX") == 0) {
        return MatrixMultiply(t->value, seed);
    }
    /* NEW: Sort array */
    if (strcmp(t->command, "SORT") == 0) {
        return SortArray(t->value, seed);
    }
    /* NEW: Crypto simulation */
    if (strcmp(t->command, "CRYPTO") == 0) {
        return CryptoSHA(t->value);
    }
    return -1; /* unknown command */
}

/* Argument bundle passed into each worker thread at creation time. */
typedef struct {
    ThreadPool *pool;
    int index; /* this thread's slot in pool->activeThreads[] */
    unsigned int seed; /* Per-thread random seed to avoid data races */
} ThreadArg;

/* ---- the function every pool thread runs forever ----------------------
 * This is the heart of the whole project. Read it top to bottom:
 *   1. sleep until there is work           (semaphore)
 *   2. take one task off the queue safely  (critical section)
 *   3. do the work                         (no locks needed here!)
 *   4. report the result safely            (critical section)
 *   5. go back to step 1
 * ------------------------------------------------------------------- */
static DWORD WINAPI WorkerThreadProc(LPVOID param) {
    ThreadArg *arg = (ThreadArg *)param;
    ThreadPool *pool = arg->pool;
    int myIndex = arg->index;
    unsigned int seed = arg->seed; /* Each thread gets its own seed */
    free(arg);

    LARGE_INTEGER startTime, endTime, freq;
    QueryPerformanceFrequency(&freq);

    for (;;) {
        /* Step 1: block here with ZERO CPU usage until a task exists,
         * or until Shutdown wakes us up. */
        WaitForSingleObject(pool->taskAvailable, INFINITE);

        if (pool->shuttingDown) {
            break; /* Shutdown() released the semaphore just to wake us */
        }

        /* Step 2: pull one task out of the shared ring buffer.
         * We hold the lock for the SHORTEST time possible -- just the
         * few array/pointer operations -- never while doing the actual
         * (potentially slow) work. That's the golden rule of locking. */
        Task task;
        EnterCriticalSection(&pool->queueLock);
        task = pool->tasks[pool->head];
        pool->head = (pool->head + 1) % QUEUE_CAPACITY;
        pool->count--;
        LeaveCriticalSection(&pool->queueLock);

        InterlockedExchange(&pool->activeThreads[myIndex], 1); /* mark busy */

        /* Step 3: the actual work. No locks held -- this is what lets
         * multiple threads genuinely run in parallel on multiple cores. */
        QueryPerformanceCounter(&startTime);
        long result = ExecuteTask(&task, &seed);
        QueryPerformanceCounter(&endTime);
        double executionMs = (double)(endTime.QuadPart - startTime.QuadPart)
                             * 1000.0 / (double)freq.QuadPart;

        InterlockedExchange(&pool->activeThreads[myIndex], 0); /* mark idle */

        /* Step 4: report back to the dispatcher. Every thread in this
         * pool shares ONE pipe handle (outputPipe), and WriteFile is not
         * guaranteed atomic for interleaved writers, so we serialize
         * writes with outputLock -- otherwise two results could get
         * garbled together into one unreadable line. */
        char line[MAX_LINE];
        int len = sprintf(line, "RESULT %d %ld %.2f\n", task.taskId, result, executionMs);

        DWORD written;
        EnterCriticalSection(&pool->outputLock);
        WriteFile(pool->outputPipe, line, (DWORD)len, &written, NULL);
        LeaveCriticalSection(&pool->outputLock);
    }
    return 0;
}

void ThreadPool_Init(ThreadPool *pool, HANDLE outputPipe) {
    memset(pool, 0, sizeof(*pool));
    pool->outputPipe = outputPipe;
    pool->threadCount = THREADS_PER_WORKER;

    InitializeCriticalSection(&pool->queueLock);
    InitializeCriticalSection(&pool->outputLock);

    /* Semaphore starts at 0 (no work yet), can never exceed QUEUE_CAPACITY
     * (can't have more "ready" signals than possible queued tasks). */
    pool->taskAvailable = CreateSemaphore(NULL, 0, QUEUE_CAPACITY, NULL);

    for (int i = 0; i < THREADS_PER_WORKER; i++) {
        ThreadArg *arg = (ThreadArg *)malloc(sizeof(ThreadArg));
        arg->pool = pool;
        arg->index = i;
        arg->seed = GetThreadRandom((unsigned int)i + 1);
        pool->threads[i] = CreateThread(NULL, 0, WorkerThreadProc, arg, 0, NULL);
    }
}

BOOL ThreadPool_Submit(ThreadPool *pool, Task task) {
    BOOL accepted = FALSE;
    EnterCriticalSection(&pool->queueLock);
    if (pool->count < QUEUE_CAPACITY) {
        pool->tasks[pool->tail] = task;
        pool->tail = (pool->tail + 1) % QUEUE_CAPACITY;
        pool->count++;
        accepted = TRUE;
    }
    LeaveCriticalSection(&pool->queueLock);

    if (accepted) {
        ReleaseSemaphore(pool->taskAvailable, 1, NULL); /* wake one idle thread */
    }
    return accepted; /* FALSE means the queue was full -- caller should back off */
}

void ThreadPool_Shutdown(ThreadPool *pool) {
    /* Setting this flag alone isn't enough: threads are asleep inside
     * WaitForSingleObject and won't check the flag until they wake up.
     * So we release the semaphore once per thread to wake every single
     * one of them, and each will see shuttingDown==1 and exit its loop. */
    InterlockedExchange(&pool->shuttingDown, 1);
    ReleaseSemaphore(pool->taskAvailable, pool->threadCount, NULL);

    WaitForMultipleObjects(pool->threadCount, pool->threads, TRUE, INFINITE);

    for (int i = 0; i < pool->threadCount; i++) {
        CloseHandle(pool->threads[i]);
    }
    CloseHandle(pool->taskAvailable);
    DeleteCriticalSection(&pool->queueLock);
    DeleteCriticalSection(&pool->outputLock);
}
