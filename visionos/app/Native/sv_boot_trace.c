// See sv_boot_trace.h.
#include "sv_boot_trace.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int g_trace_fd = -1;

static void open_trace(void)
{
    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        home = getenv("CFFIXED_USER_HOME");
    if (home == NULL || home[0] == '\0')
        return;
    // A fresh install may not have Documents yet when the first constructor runs.
    char path[1024];
    snprintf(path, sizeof path, "%s/Documents", home);
    (void)mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Documents/arranque.txt", home);
    g_trace_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
}

void sv_boot_trace(const char* step)
{
    if (g_trace_fd < 0)
        open_trace();
    if (g_trace_fd < 0 || step == NULL)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[640];
    int n = snprintf(line, sizeof line, "[%ld.%03ld] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000L, step);
    if (n > (int)sizeof line - 1)
        n = (int)sizeof line - 1;
    if (n > 0)
        (void)write(g_trace_fd, line, (size_t)n);
    fsync(g_trace_fd);
}

static void note_exit(void)
{
    sv_boot_trace("exit(): the process is ending through exit() (not a crash)");
}

// Priority 101: the earliest a constructor may ask for, so this runs before the other constructors
// of its own object; across objects the link order decides, and this file is part of the app target.
__attribute__((constructor(101))) static void sv_boot_first(void)
{
    struct utsname u;
    char line[512];
    if (uname(&u) == 0)
        snprintf(line, sizeof line, "1 process loaded (load-time constructors): %s %s %s", u.sysname, u.release, u.machine);
    else
        snprintf(line, sizeof line, "1 process loaded (load-time constructors)");
    sv_boot_trace(line);
    if (g_trace_fd >= 0)
    {
        // Until sv_game_start moves stderr to the game log, anything written to it (the port's crash
        // handler backtrace, Swift and ObjC runtime errors routed to stderr) is kept here.
        dup2(g_trace_fd, 2);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    atexit(note_exit);
}
