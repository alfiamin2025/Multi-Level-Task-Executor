/*
 * worker.c  ->  compiles to worker.exe
 * -------------------------------------
 * This is a SEPARATE PROGRAM. The dispatcher launches several copies of
 * this .exe with CreateProcess, and rewires each copy's stdin/stdout to
 * pipes instead of the console. So when this program calls
 * GetStdHandle(STD_INPUT_HANDLE) and reads from it, it's actually reading
 * whatever the dispatcher WriteFile'd into the pipe -- and this program
 * has no idea (and doesn't need to know) that there's no real console
 * on the other end.
 *
 * Protocol this program understands, one line at a time from stdin:
 *   TASK <id> <COMMAND> <value>      -> hand it to the thread pool
 *   QUIT                             -> finish up and exit
 *
 * Protocol this program writes to stdout, produced by pool threads:
 *   RESULT <id> <value>
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "threadpool.h"

/* Parse one complete line and act on it. */
static int HandleLine(char *line, ThreadPool *pool) {
    if (strncmp(line, "QUIT", 4) == 0) {
        return 0; /* signal caller to stop the main loop */
    }
    if (strncmp(line, "TASK", 4) == 0) {
        Task t;
        char cmd[32];
        int id;
        long val;
        if (sscanf(line, "TASK %d %31s %ld", &id, cmd, &val) == 3) {
            t.taskId = id;
            strncpy(t.command, cmd, sizeof(t.command) - 1);
            t.command[sizeof(t.command) - 1] = '\0';
            t.value = val;
            ThreadPool_Submit(pool, t);
        }
    }
    return 1; /* keep going */
}

int main(void) {
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    ThreadPool pool;
    ThreadPool_Init(&pool, hOut);

    /*
     * OPTIMIZATION NOTE: we could call ReadFile(hIn, &oneByte, 1, ...) in
     * a loop and check for '\n' each time, but that's one expensive
     * kernel-mode system call PER BYTE. Instead we read a whole chunk at
     * once (cheap: one syscall for up to 512 bytes) and split it into
     * lines ourselves in user-mode memory, which is essentially free.
     * This is the same trick real stdio line-buffering uses internally.
     */
    char chunk[512];
    char lineBuf[MAX_LINE];
    int  lineLen = 0;
    int  running = 1;

    while (running) {
        DWORD nread = 0;
        BOOL ok = ReadFile(hIn, chunk, sizeof(chunk), &nread, NULL);
        if (!ok || nread == 0) {
            break; /* dispatcher closed the pipe -> nothing more will ever arrive */
        }

        for (DWORD i = 0; i < nread; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (lineLen > 0) {
                    lineBuf[lineLen] = '\0';
                    running = HandleLine(lineBuf, &pool);
                    lineLen = 0;
                    if (!running) break;
                }
            } else if (lineLen < MAX_LINE - 1) {
                lineBuf[lineLen++] = c;
            }
        }
    }

    ThreadPool_Shutdown(&pool);
    return 0;
}
