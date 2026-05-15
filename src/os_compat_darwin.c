#include "os_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/proc_info.h>
#include <libproc.h>
#endif

/* ---------- process listing (sysctl KERN_PROC_ALL) ---------- */

int os_list_processes(ProcInfo *procs, int max) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) return -1;
    struct kinfo_proc *list = malloc(len);
    if (!list) return -1;
    if (sysctl(mib, 3, list, &len, NULL, 0) != 0) {
        free(list);
        return -1;
    }
    int n = (int)(len / sizeof *list);
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        struct kinfo_proc *p = &list[i];
        if (p->kp_proc.p_pid == 0) continue;
        procs[out].pid  = p->kp_proc.p_pid;
        procs[out].ppid = p->kp_eproc.e_ppid;
        procs[out].uid  = p->kp_eproc.e_ucred.cr_uid;
        procs[out].cpu_pct = 0.0;
        procs[out].rss_kb  = 0;
        strncpy(procs[out].comm, p->kp_proc.p_comm, sizeof procs[out].comm - 1);
        procs[out].comm[sizeof procs[out].comm - 1] = '\0';

        /* RSS via libproc */
        struct proc_taskinfo ti;
        int got = proc_pidinfo(procs[out].pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti);
        if (got == (int)sizeof ti) {
            procs[out].rss_kb = (long)(ti.pti_resident_size / 1024);
        }
        out++;
    }
    free(list);
    return out;
}

/* ---------- sandbox (macOS Sandbox/seatbelt) ----------
 *
 * sandbox_init_with_parameters is deprecated but still works on every shipping
 * macOS. We declare it ourselves to avoid the deprecation warning. The SBPL
 * profiles below are intentionally simple. */

extern int sandbox_init_with_parameters(const char *profile,
                                        uint64_t flags,
                                        const char *const params[],
                                        char **errorbuf);

static const char *profile_sbpl(const char *name) {
    if (!name) name = "default";

    /* Common allowlist any non-trivial command needs: /dev/null, tmpdirs,
     * tty I/O. Inserted into every profile below. */
#define LLA_COMMON_FILE_WRITES \
    "(allow file-write* (literal \"/dev/null\") (literal \"/dev/dtracehelper\"))" \
    "(allow file-write* (subpath \"/private/tmp\") (subpath \"/tmp\") (subpath \"/var/tmp\") (subpath \"/private/var/folders\"))" \
    "(allow file-write-data (literal \"/dev/tty\"))"

    /* "readonly": exec allowed (otherwise the command can't even run),
     * but no network and no fs writes outside /dev/null + tmpdirs. */
    if (strcmp(name, "readonly") == 0)
        return
        "(version 1)"
        "(deny default)"
        "(allow process-fork)"
        "(allow process-exec)"
        "(allow process-info*)"
        "(allow signal (target self))"
        "(allow file-read*)"
        LLA_COMMON_FILE_WRITES
        "(allow sysctl-read)"
        "(allow mach-lookup)"
        "(allow ipc-posix-shm)";

    /* "default": read FS, fork/exec OK, no network */
    if (strcmp(name, "default") == 0)
        return
        "(version 1)"
        "(deny default)"
        "(allow process*)"
        "(allow signal (target self))"
        "(allow file-read*)"
        LLA_COMMON_FILE_WRITES
        "(allow sysctl-read)"
        "(allow mach-lookup)"
        "(allow ipc-posix-shm)";

    /* "network": default + outbound network + DNS */
    if (strcmp(name, "network") == 0)
        return
        "(version 1)"
        "(deny default)"
        "(allow process*)"
        "(allow signal (target self))"
        "(allow file-read*)"
        LLA_COMMON_FILE_WRITES
        "(allow sysctl-read)"
        "(allow mach-lookup)"
        "(allow ipc-posix-shm)"
        "(allow network*)"
        "(allow system-socket)";

    /* "build": default + write everywhere (enforcement is best-effort
     * since SBPL has no convenient "cwd subtree" predicate). */
    if (strcmp(name, "build") == 0)
        return
        "(version 1)"
        "(deny default)"
        "(allow process*)"
        "(allow signal (target self))"
        "(allow file*)"
        "(allow sysctl-read)"
        "(allow mach-lookup)"
        "(allow ipc-posix-shm)";

    return NULL; /* "none" or unknown: caller should not apply sandbox */
}

int os_apply_sandbox(const char *profile) {
    if (!profile || strcmp(profile, "none") == 0) return 0;
    const char *sbpl = profile_sbpl(profile);
    if (!sbpl) return -1;
    char *err = NULL;
    if (sandbox_init_with_parameters(sbpl, 0, NULL, &err) != 0) {
        if (err) free(err);
        return -1;
    }
    return 0;
}

/* ---------- watch_path via kqueue ---------- */

int os_watch_path(const char *path, int timeout_ms, WatchEvent *out) {
    if (!out) return -1;
    int fd = open(path, O_EVTONLY);
    if (fd < 0) return -1;

    int kq = kqueue();
    if (kq < 0) { close(fd); return -1; }

    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB,
           0, NULL);

    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms > 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    struct kevent event;
    int n = kevent(kq, &change, 1, &event, 1, tsp);
    close(kq);
    close(fd);

    if (n < 0) return -1;
    if (n == 0) {
        strncpy(out->path, path, sizeof out->path - 1);
        out->path[sizeof out->path - 1] = '\0';
        strncpy(out->kind, "timeout", sizeof out->kind - 1);
        out->kind[sizeof out->kind - 1] = '\0';
        return 1;
    }
    strncpy(out->path, path, sizeof out->path - 1);
    out->path[sizeof out->path - 1] = '\0';
    if      (event.fflags & NOTE_DELETE) strncpy(out->kind, "deleted",  sizeof out->kind - 1);
    else if (event.fflags & NOTE_RENAME) strncpy(out->kind, "renamed",  sizeof out->kind - 1);
    else if (event.fflags & NOTE_WRITE)  strncpy(out->kind, "modified", sizeof out->kind - 1);
    else if (event.fflags & NOTE_EXTEND) strncpy(out->kind, "modified", sizeof out->kind - 1);
    else if (event.fflags & NOTE_ATTRIB) strncpy(out->kind, "modified", sizeof out->kind - 1);
    else                                  strncpy(out->kind, "modified", sizeof out->kind - 1);
    out->kind[sizeof out->kind - 1] = '\0';
    return 0;
}
