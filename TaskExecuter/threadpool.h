/*
 * threadpool.h
 * ------------
 * A worker PROCESS owns exactly one ThreadPool. The pool owns a fixed set
 * of THREADS, a circular queue of Tasks, and the synchronization objects
 * that keep the queue safe when multiple threads touch it at once.
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <windows.h>
#include "common.h"

typedef struct {
    /* --- the work queue (a ring buffer, so no shifting elements around) --- */
    Task            tasks[QUEUE_CAPACITY];
    int             head;          /* index we take the next task from       */
    int             tail;          /* index we insert the next task at       */
    int             count;         /* how many tasks currently queued        */
    CRITICAL_SECTION queueLock;    /* protects head/tail/count/tasks[]       */

    /*
     * taskAvailable is a SEMAPHORE, not a mutex.
     * A mutex protects "only one thread in here at a time".
     * A semaphore counts "how many units of work are ready".
     * Every time we enqueue a task we ReleaseSemaphore(+1).
     * Every idle thread blocks on WaitForSingleObject(taskAvailable),
     * which only returns once there is something to do. This means idle
     * threads consume ZERO CPU while waiting -- no polling, no busy loop.
     */
    HANDLE          taskAvailable;

    /* --- the threads themselves --- */
    HANDLE          threads[THREADS_PER_WORKER];
    int             threadCount;

    /* --- where finished results get reported --- */
    HANDLE           outputPipe;    /* worker's stdout, connected to dispatcher */
    CRITICAL_SECTION outputLock;    /* multiple threads write here -> must serialize */

    volatile LONG   shuttingDown;                    /* 0 or 1, read by all threads */
    volatile LONG   activeThreads[THREADS_PER_WORKER]; /* 0=idle 1=busy, for status  */
} ThreadPool;

void ThreadPool_Init(ThreadPool *pool, HANDLE outputPipe);
BOOL ThreadPool_Submit(ThreadPool *pool, Task task);
void ThreadPool_Shutdown(ThreadPool *pool);

#endif /* THREADPOOL_H */
