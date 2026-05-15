#ifndef LLA_OS_COMPAT_H
#define LLA_OS_COMPAT_H

#include <stddef.h>
#include "util.h"

/* ---------- process listing ---------- */

typedef struct {
    int pid;
    int ppid;
    char comm[256];   /* command name (basename) */
    long rss_kb;      /* resident set size */
    double cpu_pct;   /* approx CPU% (may be 0 if not derivable cheaply) */
    int uid;
} ProcInfo;

/* Fills procs[] up to max, returns number written, or -1 on error. */
int os_list_processes(ProcInfo *procs, int max);

/* ---------- sandbox ---------- */

/* Apply a named sandbox profile to the current process. Must be called
 * AFTER fork() and BEFORE execvp(). Returns 0 on success.
 * Known profiles: "readonly", "default", "network", "build", "none". */
int os_apply_sandbox(const char *profile);

/* ---------- filesystem watch ---------- */

typedef struct {
    char path[1024];
    char kind[32];   /* "modified", "created", "deleted", "renamed", "timeout" */
} WatchEvent;

/* Block until path (file or its parent dir's contents) changes or timeout.
 * Returns 0 on event, 1 on timeout, -1 on error. */
int os_watch_path(const char *path, int timeout_ms, WatchEvent *out);

#endif
