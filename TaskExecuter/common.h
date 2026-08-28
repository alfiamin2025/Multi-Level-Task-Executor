/*
 * common.h
 * ---------
 * Anything both the dispatcher (parent) and worker (child) need to agree on
 * lives here: the wire format of a task, and a few tunables.
 *
 * Why a shared header instead of copy-pasting these defines into both files?
 * Because if you ever change QUEUE_CAPACITY or the Task struct, you only
 * change it once and both programs stay in sync.
 */

#ifndef COMMON_H
#define COMMON_H

#define MAX_LINE          256   /* max length of one line of the wire protocol */
#define QUEUE_CAPACITY     64   /* max tasks a single worker can have queued   */
#define THREADS_PER_WORKER  4   /* threads inside each worker's thread pool    */
#define NUM_WORKERS         3   /* worker PROCESSES the dispatcher spawns      */

/*
 * A Task is what flows across the pipe as text, e.g.:
 *   "TASK 7 FIB 30\n"
 * meaning: task id 7, run command FIB on value 30.
 *
 * We send tasks as plain text, not raw bytes of this struct. That's a
 * deliberate choice: text is human-readable (you can literally watch it
 * in a debugger or Process Monitor), and it sidesteps struct-padding /
 * byte-order headaches that come with sending raw bytes between processes.
 */
typedef struct {
    int  taskId;
    char command[32];   /* "FACT" | "FIB" | "PRIME" | "SUMSQ" | "MATRIX" | "SORT" | "CRYPTO" */
    long value;
} Task;

#endif /* COMMON_H */
