#include "os_compat.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

/* ---------- process listing via /proc ---------- */

int os_list_processes(ProcInfo *procs, int max) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int out = 0;
    while ((e = readdir(d)) && out < max) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;

        char path[256];
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        char comm[256] = "?";
        long rss_kb = 0;
        int ppid = 0, uid = 0;
        while (fgets(line, sizeof line, f)) {
            if      (strncmp(line, "Name:",  5) == 0) sscanf(line, "Name: %255s", comm);
            else if (strncmp(line, "PPid:",  5) == 0) sscanf(line, "PPid: %d", &ppid);
            else if (strncmp(line, "Uid:",   4) == 0) sscanf(line, "Uid: %d", &uid);
            else if (strncmp(line, "VmRSS:", 6) == 0) sscanf(line, "VmRSS: %ld kB", &rss_kb);
        }
        fclose(f);
        procs[out].pid  = pid;
        procs[out].ppid = ppid;
        procs[out].uid  = uid;
        procs[out].rss_kb = rss_kb;
        procs[out].cpu_pct = 0.0;
        strncpy(procs[out].comm, comm, sizeof procs[out].comm - 1);
        procs[out].comm[sizeof procs[out].comm - 1] = '\0';
        out++;
    }
    closedir(d);
    return out;
}

/* ---------- sandbox ----------
 *
 * Linux confinement is intentionally fail-closed until a real seccomp/namespace
 * implementation exists. Returning success here would make exec_command appear
 * sandboxed while actually running with the agent's normal privileges.
 */
int os_apply_sandbox(const char *profile) {
    if (profile && strcmp(profile, "none") == 0) return 0;
    return -1;
}

/* ---------- watch_path via inotify ---------- */

int os_watch_path(const char *path, int timeout_ms, WatchEvent *out) {
    if (!out) return -1;
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return -1;

    int wd = inotify_add_watch(fd, path,
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
        IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF | IN_ATTRIB);
    if (wd < 0) { close(fd); return -1; }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int n = poll(&pfd, 1, timeout_ms);
    if (n < 0) { close(fd); return -1; }

    strncpy(out->path, path, sizeof out->path - 1);
    out->path[sizeof out->path - 1] = '\0';

    if (n == 0) {
        strncpy(out->kind, "timeout", sizeof out->kind - 1);
        out->kind[sizeof out->kind - 1] = '\0';
        close(fd);
        return 1;
    }

    char buf[4096];
    ssize_t rn = read(fd, buf, sizeof buf);
    close(fd);
    if (rn <= 0) {
        strncpy(out->kind, "modified", sizeof out->kind - 1);
        out->kind[sizeof out->kind - 1] = '\0';
        return 0;
    }
    struct inotify_event *ev = (struct inotify_event *)buf;
    if      (ev->mask & (IN_DELETE | IN_DELETE_SELF))         strncpy(out->kind, "deleted",  sizeof out->kind - 1);
    else if (ev->mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF))
                                                              strncpy(out->kind, "renamed",  sizeof out->kind - 1);
    else if (ev->mask & IN_CREATE)                            strncpy(out->kind, "created",  sizeof out->kind - 1);
    else                                                       strncpy(out->kind, "modified", sizeof out->kind - 1);
    out->kind[sizeof out->kind - 1] = '\0';
    return 0;
}
