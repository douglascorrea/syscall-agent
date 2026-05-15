#include "tools_proc.h"
#include "os_compat.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXEC_OUTPUT_MAX  (256 * 1024)
#define EXEC_ARG_MAX     128
#define EXEC_TIMEOUT_MAX_MS 120000
#define BG_WAIT_MAX_MS    30000

/* ============================================================ *
 *                        helpers                                *
 * ============================================================ */

static char *dup_or_default(cJSON *node, const char *def) {
    if (cJSON_IsString(node) && node->valuestring) return strdup(node->valuestring);
    return def ? strdup(def) : NULL;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static cJSON *make_function_tool(const char *name, const char *desc, cJSON *params) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "description", desc);
    cJSON_AddItemToObject(fn, "parameters", params);
    return tool;
}

static cJSON *param_obj(const char *const *required) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "object");
    cJSON_AddObjectToObject(p, "properties");
    cJSON *req = cJSON_AddArrayToObject(p, "required");
    if (required) {
        for (size_t i = 0; required[i]; i++) {
            cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
        }
    }
    return p;
}

static void add_prop(cJSON *params, const char *name, const char *type, const char *desc) {
    cJSON *props = cJSON_GetObjectItem(params, "properties");
    cJSON *p = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
}

/* Convert a cJSON string array to argv (NULL-terminated). Returns NULL on error. */
static char **json_to_argv(cJSON *arr) {
    if (!cJSON_IsArray(arr)) return NULL;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return NULL;
    if (n > EXEC_ARG_MAX) return NULL;
    char **argv = calloc(n + 1, sizeof *argv);
    if (!argv) return NULL;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(it) || !it->valuestring) {
            for (int j = 0; j < i; j++) free(argv[j]);
            free(argv);
            return NULL;
        }
        argv[i] = strdup(it->valuestring);
    }
    return argv;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static char **json_to_env(cJSON *obj) {
    if (!cJSON_IsObject(obj)) return NULL;
    int n = cJSON_GetArraySize(obj);
    char **env = calloc(n + 1, sizeof *env);
    if (!env) return NULL;
    int i = 0;
    cJSON *kv;
    cJSON_ArrayForEach(kv, obj) {
        if (!cJSON_IsString(kv) || !kv->string || !kv->valuestring) continue;
        size_t len = strlen(kv->string) + 1 + strlen(kv->valuestring) + 1;
        env[i] = malloc(len);
        snprintf(env[i], len, "%s=%s", kv->string, kv->valuestring);
        i++;
    }
    env[i] = NULL;
    return env;
}

/* ============================================================ *
 *               list_processes (always available)               *
 * ============================================================ */

static int rss_desc(const void *a, const void *b) {
    long ra = ((const ProcInfo *)a)->rss_kb;
    long rb = ((const ProcInfo *)b)->rss_kb;
    return (rb > ra) - (rb < ra);
}

static char *tool_list_processes(cJSON *args) {
    cJSON *top_j = cJSON_GetObjectItem(args, "top_n");
    int top_n = cJSON_IsNumber(top_j) ? (int)top_j->valueint : 20;
    if (top_n <= 0) top_n = 20;
    if (top_n > 200) top_n = 200;

    int cap = 4096;
    ProcInfo *procs = calloc(cap, sizeof *procs);
    int n = os_list_processes(procs, cap);
    if (n < 0) {
        free(procs);
        return strdup("ERROR: could not enumerate processes");
    }
    qsort(procs, n, sizeof *procs, rss_desc);

    Buf out; buf_init(&out);
    buf_printf(&out, "LIST_PROCESSES (top %d by RSS, total=%d)\n---\n",
        top_n, n);
    buf_printf(&out, "%-7s  %-7s  %-10s  %s\n", "PID", "PPID", "RSS(kB)", "COMM");
    int shown = n < top_n ? n : top_n;
    for (int i = 0; i < shown; i++) {
        buf_printf(&out, "%-7d  %-7d  %-10ld  %s\n",
            procs[i].pid, procs[i].ppid, procs[i].rss_kb, procs[i].comm);
    }
    free(procs);
    return out.data;
}

/* ============================================================ *
 *               exec_command (P1) + bg machinery (P2)           *
 * ============================================================ */

typedef struct {
    int fd;
    Buf buf;
    int eof;
    size_t cap;       /* hard cap */
    int truncated;
} StreamReader;

static void sr_init(StreamReader *s, int fd, size_t cap) {
    s->fd = fd;
    buf_init(&s->buf);
    s->eof = 0;
    s->cap = cap;
    s->truncated = 0;
}

static void sr_pump(StreamReader *s) {
    if (s->eof) return;
    char tmp[4096];
    ssize_t n = read(s->fd, tmp, sizeof tmp);
    if (n == 0) { s->eof = 1; return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        s->eof = 1; return;
    }
    size_t room = (s->buf.len < s->cap) ? (s->cap - s->buf.len) : 0;
    size_t take = (size_t)n < room ? (size_t)n : room;
    if (take > 0) buf_append(&s->buf, tmp, take);
    if ((size_t)n > take) s->truncated = 1;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void apply_rlimits_default(void) {
    struct rlimit r;
    r.rlim_cur = r.rlim_max = 30;             setrlimit(RLIMIT_CPU,    &r);
    r.rlim_cur = r.rlim_max = 512UL<<20;      setrlimit(RLIMIT_AS,     &r);
    r.rlim_cur = r.rlim_max = 256UL<<20;      setrlimit(RLIMIT_FSIZE,  &r);
    r.rlim_cur = r.rlim_max = 256;            setrlimit(RLIMIT_NOFILE, &r);
}

static int valid_profile(const char *profile) {
    if (!profile) return 1;
    return strcmp(profile, "readonly") == 0 ||
           strcmp(profile, "default") == 0 ||
           strcmp(profile, "network") == 0 ||
           strcmp(profile, "build") == 0 ||
           strcmp(profile, "none") == 0;
}

static char *validate_exec_profile(const ToolCtx *ctx, const char *profile) {
    if (!valid_profile(profile)) {
        return strdup("ERROR: invalid sandbox profile (expected readonly, default, network, build, or none)");
    }
    if (profile && strcmp(profile, "none") == 0 && !ctx->allow_unsafe_exec) {
        return strdup("ERROR: profile='none' requires --allow-unsafe-exec");
    }
    return NULL;
}

typedef struct {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
} SpawnHandle;

static void close_if_open(int fd) {
    if (fd >= 0) close(fd);
}

/* Spawn a child. Returns 0 on success, -1 on fork failure.
 * On success, fills h. Child inherits the sandbox profile and rlimits. */
static int spawn_child(char *const argv[], const char *cwd, char *const env[],
                       const char *sandbox_profile, int unsafe,
                       SpawnHandle *h, char **err_out) {
    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (err_out) *err_out = strdup("pipe() failed");
        close_if_open(in_pipe[0]);  close_if_open(in_pipe[1]);
        close_if_open(out_pipe[0]); close_if_open(out_pipe[1]);
        close_if_open(err_pipe[0]); close_if_open(err_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (err_out) *err_out = strdup("fork() failed");
        close_if_open(in_pipe[0]);  close_if_open(in_pipe[1]);
        close_if_open(out_pipe[0]); close_if_open(out_pipe[1]);
        close_if_open(err_pipe[0]); close_if_open(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        if (cwd && *cwd) {
            if (chdir(cwd) != 0) {
                fprintf(stderr, "chdir(%s): %s\n", cwd, strerror(errno));
                _exit(126);
            }
        }
        apply_rlimits_default();

        if (!unsafe) {
            if (os_apply_sandbox(sandbox_profile ? sandbox_profile : "default") != 0) {
                fprintf(stderr, "sandbox_init failed for profile '%s'\n",
                    sandbox_profile ? sandbox_profile : "default");
                _exit(126);
            }
        }

        if (env) {
            extern char **environ;
            environ = (char **)env;
        }
        execvp(argv[0], argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    h->pid       = pid;
    h->stdin_fd  = in_pipe[1];
    h->stdout_fd = out_pipe[0];
    h->stderr_fd = err_pipe[0];
    return 0;
}

/* ---------- exec_command (blocking) ---------- */

static char *tool_exec_command(ToolCtx *ctx, cJSON *args) {
    cJSON *argv_j = cJSON_GetObjectItem(args, "argv");
    if (!cJSON_IsArray(argv_j)) return strdup("ERROR: 'argv' must be an array of strings");
    char **argv = json_to_argv(argv_j);
    if (!argv) return strdup("ERROR: 'argv' contains non-string elements");

    char *cwd            = dup_or_default(cJSON_GetObjectItem(args, "cwd"), NULL);
    char *profile        = dup_or_default(cJSON_GetObjectItem(args, "profile"), "default");
    char *stdin_str      = dup_or_default(cJSON_GetObjectItem(args, "stdin"), NULL);
    cJSON *timeout_j     = cJSON_GetObjectItem(args, "timeout_ms");
    int timeout_ms       = cJSON_IsNumber(timeout_j) ? (int)timeout_j->valueint : 30000;
    int unsafe           = ctx->allow_unsafe_exec && profile && (strcmp(profile, "none") == 0);
    if (timeout_ms <= 0) timeout_ms = 30000;
    if (timeout_ms > EXEC_TIMEOUT_MAX_MS) timeout_ms = EXEC_TIMEOUT_MAX_MS;

    char *profile_err = validate_exec_profile(ctx, profile);
    if (profile_err) {
        free_argv(argv); free(cwd); free(profile); free(stdin_str);
        return profile_err;
    }

    char **env = json_to_env(cJSON_GetObjectItem(args, "env"));

    SpawnHandle h = {0};
    char *spawn_err = NULL;
    if (spawn_child(argv, cwd, env, profile, unsafe, &h, &spawn_err) != 0) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: spawn: %s", spawn_err ? spawn_err : "unknown");
        free(spawn_err); free_argv(argv); free_argv(env);
        free(cwd); free(profile); free(stdin_str);
        return b.data;
    }

    if (stdin_str) {
        size_t len = strlen(stdin_str);
        ssize_t off = 0;
        while ((size_t)off < len) {
            ssize_t w = write(h.stdin_fd, stdin_str + off, len - off);
            if (w <= 0) break;
            off += w;
        }
    }
    close(h.stdin_fd); h.stdin_fd = -1;

    StreamReader so, se;
    sr_init(&so, h.stdout_fd, EXEC_OUTPUT_MAX);
    sr_init(&se, h.stderr_fd, EXEC_OUTPUT_MAX);

    long start = now_ms();
    int timed_out = 0;
    int status = 0;
    while (1) {
        struct pollfd pfds[2] = {
            { .fd = so.eof ? -1 : so.fd, .events = POLLIN },
            { .fd = se.eof ? -1 : se.fd, .events = POLLIN },
        };
        int remaining = timeout_ms - (int)(now_ms() - start);
        if (remaining <= 0) { timed_out = 1; break; }
        int pn = poll(pfds, 2, remaining < 250 ? remaining : 250);
        if (pn > 0) {
            if (pfds[0].revents & (POLLIN|POLLHUP|POLLERR)) sr_pump(&so);
            if (pfds[1].revents & (POLLIN|POLLHUP|POLLERR)) sr_pump(&se);
        }
        /* check if child exited */
        int wn = waitpid(h.pid, &status, WNOHANG);
        if (wn == h.pid) {
            sr_pump(&so); sr_pump(&se);
            break;
        }
        if (so.eof && se.eof) {
            waitpid(h.pid, &status, 0);
            break;
        }
    }
    if (timed_out) {
        kill(h.pid, SIGTERM);
        usleep(200 * 1000);
        kill(h.pid, SIGKILL);
        waitpid(h.pid, &status, 0);
    }

    close(h.stdout_fd);
    close(h.stderr_fd);

    struct rusage ru;
    long peak_kb = 0;
    if (getrusage(RUSAGE_CHILDREN, &ru) == 0) {
#if defined(__APPLE__)
        peak_kb = (long)(ru.ru_maxrss / 1024); /* bytes on macOS */
#else
        peak_kb = (long)ru.ru_maxrss;          /* kilobytes on Linux */
#endif
    }

    int exit_code = -1;
    const char *sig_name = NULL;
    if (WIFEXITED(status))      exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) sig_name = strsignal(WTERMSIG(status));

    Buf out; buf_init(&out);
    buf_printf(&out,
        "EXEC argv[0]=%s profile=%s exit=%d%s%s timed_out=%s duration_ms=%ld peak_mem_kb=%ld\n"
        "--- stdout (%zu bytes%s) ---\n",
        argv[0],
        profile,
        exit_code,
        sig_name ? " signal=" : "",
        sig_name ? sig_name : "",
        timed_out ? "true" : "false",
        now_ms() - start,
        peak_kb,
        so.buf.len, so.truncated ? ", truncated" : "");
    if (so.buf.data) buf_append(&out, so.buf.data, so.buf.len);
    buf_printf(&out, "\n--- stderr (%zu bytes%s) ---\n",
        se.buf.len, se.truncated ? ", truncated" : "");
    if (se.buf.data) buf_append(&out, se.buf.data, se.buf.len);

    buf_free(&so.buf); buf_free(&se.buf);
    free_argv(argv); free_argv(env);
    free(cwd); free(profile); free(stdin_str);
    return out.data;
}

/* ============================================================ *
 *               background process table (P2)                   *
 * ============================================================ */

#define BG_MAX 32

typedef struct {
    int   alive;
    int   handle_id;       /* 1-based; 0 = empty slot */
    pid_t pid;
    int   stdout_fd;
    int   stderr_fd;
    int   stdout_eof;
    int   stderr_eof;
    Buf   stdout_buf;       /* grows up to EXEC_OUTPUT_MAX */
    Buf   stderr_buf;
    int   exited;
    int   exit_code;
    int   term_signal;
    long  started_ms;
    char  argv0[256];
} BgSlot;

struct BgTable {
    BgSlot slots[BG_MAX];
    int    next_id;
};

BgTable *bg_table_new(void) {
    BgTable *t = calloc(1, sizeof *t);
    if (t) t->next_id = 1;
    return t;
}

static void bg_pump_slot(BgSlot *s) {
    if (!s->alive) return;
    if (!s->stdout_eof) {
        char tmp[4096];
        ssize_t n = read(s->stdout_fd, tmp, sizeof tmp);
        if (n == 0) s->stdout_eof = 1;
        else if (n > 0) {
            size_t room = (s->stdout_buf.len < EXEC_OUTPUT_MAX) ?
                EXEC_OUTPUT_MAX - s->stdout_buf.len : 0;
            size_t take = (size_t)n < room ? (size_t)n : room;
            if (take) buf_append(&s->stdout_buf, tmp, take);
        }
    }
    if (!s->stderr_eof) {
        char tmp[4096];
        ssize_t n = read(s->stderr_fd, tmp, sizeof tmp);
        if (n == 0) s->stderr_eof = 1;
        else if (n > 0) {
            size_t room = (s->stderr_buf.len < EXEC_OUTPUT_MAX) ?
                EXEC_OUTPUT_MAX - s->stderr_buf.len : 0;
            size_t take = (size_t)n < room ? (size_t)n : room;
            if (take) buf_append(&s->stderr_buf, tmp, take);
        }
    }
    /* check exit */
    int status = 0;
    int rc = waitpid(s->pid, &status, WNOHANG);
    if (rc == s->pid) {
        s->exited = 1;
        if (WIFEXITED(status))      s->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) s->term_signal = WTERMSIG(status);
    }
}

static BgSlot *bg_find(BgTable *t, int handle) {
    for (int i = 0; i < BG_MAX; i++) {
        if (t->slots[i].alive && t->slots[i].handle_id == handle) return &t->slots[i];
    }
    return NULL;
}

static char *tool_spawn_bg(ToolCtx *ctx, cJSON *args) {
    if (!ctx->bg) return strdup("ERROR: bg disabled");

    cJSON *argv_j = cJSON_GetObjectItem(args, "argv");
    if (!cJSON_IsArray(argv_j)) return strdup("ERROR: 'argv' must be an array of strings");
    char **argv = json_to_argv(argv_j);
    if (!argv) return strdup("ERROR: 'argv' contains non-string elements");

    char *cwd     = dup_or_default(cJSON_GetObjectItem(args, "cwd"), NULL);
    char *profile = dup_or_default(cJSON_GetObjectItem(args, "profile"), "default");
    char **env    = json_to_env(cJSON_GetObjectItem(args, "env"));
    int unsafe    = ctx->allow_unsafe_exec && profile && (strcmp(profile, "none") == 0);
    char *profile_err = validate_exec_profile(ctx, profile);
    if (profile_err) {
        free_argv(argv); free_argv(env); free(cwd); free(profile);
        return profile_err;
    }

    int slot_idx = -1;
    for (int i = 0; i < BG_MAX; i++) {
        if (!ctx->bg->slots[i].alive) { slot_idx = i; break; }
    }
    if (slot_idx < 0) {
        free_argv(argv); free_argv(env); free(cwd); free(profile);
        return strdup("ERROR: bg table full (max 32)");
    }

    SpawnHandle h = {0};
    char *spawn_err = NULL;
    if (spawn_child(argv, cwd, env, profile, unsafe, &h, &spawn_err) != 0) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: spawn: %s", spawn_err ? spawn_err : "?");
        free(spawn_err); free_argv(argv); free_argv(env); free(cwd); free(profile);
        return b.data;
    }
    close(h.stdin_fd);

    BgSlot *s = &ctx->bg->slots[slot_idx];
    memset(s, 0, sizeof *s);
    s->alive = 1;
    s->handle_id = ctx->bg->next_id++;
    s->pid = h.pid;
    s->stdout_fd = h.stdout_fd;
    s->stderr_fd = h.stderr_fd;
    s->started_ms = now_ms();
    strncpy(s->argv0, argv[0], sizeof s->argv0 - 1);
    buf_init(&s->stdout_buf);
    buf_init(&s->stderr_buf);

    Buf out; buf_init(&out);
    buf_printf(&out, "SPAWN_BG handle=%d pid=%d argv[0]=%s profile=%s\n",
        s->handle_id, (int)s->pid, s->argv0, profile);

    free_argv(argv); free_argv(env); free(cwd); free(profile);
    return out.data;
}

static char *tool_bg_read(ToolCtx *ctx, cJSON *args) {
    if (!ctx->bg) return strdup("ERROR: bg disabled");
    cJSON *h_j = cJSON_GetObjectItem(args, "handle");
    if (!cJSON_IsNumber(h_j)) return strdup("ERROR: 'handle' required");
    cJSON *off_j = cJSON_GetObjectItem(args, "since_offset_stdout");
    cJSON *eoff_j = cJSON_GetObjectItem(args, "since_offset_stderr");
    cJSON *wait_j = cJSON_GetObjectItem(args, "wait_ms");
    int wait_ms = cJSON_IsNumber(wait_j) ? (int)wait_j->valueint : 0;
    if (wait_ms < 0) wait_ms = 0;
    if (wait_ms > BG_WAIT_MAX_MS) wait_ms = BG_WAIT_MAX_MS;

    BgSlot *s = bg_find(ctx->bg, (int)h_j->valueint);
    if (!s) return strdup("ERROR: no such bg handle");

    long start = now_ms();
    do {
        bg_pump_slot(s);
        if (wait_ms <= 0 || s->exited) break;
        if (now_ms() - start >= wait_ms) break;
        struct pollfd pfds[2] = {
            { .fd = s->stdout_eof ? -1 : s->stdout_fd, .events = POLLIN },
            { .fd = s->stderr_eof ? -1 : s->stderr_fd, .events = POLLIN },
        };
        int remaining = wait_ms - (int)(now_ms() - start);
        if (remaining <= 0) break;
        poll(pfds, 2, remaining < 250 ? remaining : 250);
    } while (1);

    size_t off_o = cJSON_IsNumber(off_j)  ? (size_t)off_j->valuedouble  : 0;
    size_t off_e = cJSON_IsNumber(eoff_j) ? (size_t)eoff_j->valuedouble : 0;
    if (off_o > s->stdout_buf.len) off_o = s->stdout_buf.len;
    if (off_e > s->stderr_buf.len) off_e = s->stderr_buf.len;

    Buf out; buf_init(&out);
    buf_printf(&out,
        "BG_READ handle=%d pid=%d exited=%s exit=%d signal=%d "
        "stdout_total=%zu stderr_total=%zu next_off_stdout=%zu next_off_stderr=%zu\n",
        s->handle_id, (int)s->pid,
        s->exited ? "true" : "false",
        s->exit_code, s->term_signal,
        s->stdout_buf.len, s->stderr_buf.len,
        s->stdout_buf.len, s->stderr_buf.len);
    buf_printf(&out, "--- stdout[%zu:%zu] ---\n", off_o, s->stdout_buf.len);
    if (s->stdout_buf.data && off_o < s->stdout_buf.len) {
        buf_append(&out, s->stdout_buf.data + off_o, s->stdout_buf.len - off_o);
    }
    buf_printf(&out, "\n--- stderr[%zu:%zu] ---\n", off_e, s->stderr_buf.len);
    if (s->stderr_buf.data && off_e < s->stderr_buf.len) {
        buf_append(&out, s->stderr_buf.data + off_e, s->stderr_buf.len - off_e);
    }
    return out.data;
}

static char *tool_bg_kill(ToolCtx *ctx, cJSON *args) {
    if (!ctx->bg) return strdup("ERROR: bg disabled");
    cJSON *h_j = cJSON_GetObjectItem(args, "handle");
    if (!cJSON_IsNumber(h_j)) return strdup("ERROR: 'handle' required");
    cJSON *sig_j = cJSON_GetObjectItem(args, "signal");
    int sig = cJSON_IsNumber(sig_j) ? (int)sig_j->valueint : SIGTERM;

    BgSlot *s = bg_find(ctx->bg, (int)h_j->valueint);
    if (!s) return strdup("ERROR: no such bg handle");

    if (!s->exited) {
        kill(s->pid, sig);
        for (int i = 0; i < 20; i++) {
            int status = 0;
            if (waitpid(s->pid, &status, WNOHANG) == s->pid) {
                s->exited = 1;
                if (WIFEXITED(status)) s->exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) s->term_signal = WTERMSIG(status);
                break;
            }
            usleep(100 * 1000);
        }
        if (!s->exited) {
            kill(s->pid, SIGKILL);
            int status = 0;
            waitpid(s->pid, &status, 0);
            s->exited = 1;
            if (WIFSIGNALED(status)) s->term_signal = WTERMSIG(status);
        }
    }
    Buf out; buf_init(&out);
    buf_printf(&out, "BG_KILL handle=%d exited=true exit=%d signal=%d\n",
        s->handle_id, s->exit_code, s->term_signal);
    close(s->stdout_fd); close(s->stderr_fd);
    buf_free(&s->stdout_buf); buf_free(&s->stderr_buf);
    s->alive = 0;
    return out.data;
}

static char *tool_bg_list(ToolCtx *ctx, cJSON *args) {
    (void)args;
    if (!ctx->bg) return strdup("ERROR: bg disabled");
    Buf out; buf_init(&out);
    buf_append_cstr(&out, "BG_LIST\n---\n");
    int any = 0;
    for (int i = 0; i < BG_MAX; i++) {
        BgSlot *s = &ctx->bg->slots[i];
        if (!s->alive) continue;
        bg_pump_slot(s);
        buf_printf(&out,
            "handle=%d pid=%d argv[0]=%s exited=%s stdout_bytes=%zu stderr_bytes=%zu age_ms=%ld\n",
            s->handle_id, (int)s->pid, s->argv0,
            s->exited ? "true" : "false",
            s->stdout_buf.len, s->stderr_buf.len,
            now_ms() - s->started_ms);
        any = 1;
    }
    if (!any) buf_append_cstr(&out, "(no background processes)\n");
    return out.data;
}

void bg_table_free_kill_all(BgTable *t) {
    if (!t) return;
    for (int i = 0; i < BG_MAX; i++) {
        BgSlot *s = &t->slots[i];
        if (!s->alive) continue;
        if (!s->exited) {
            kill(s->pid, SIGTERM);
            usleep(100 * 1000);
            int status = 0;
            if (waitpid(s->pid, &status, WNOHANG) != s->pid) {
                kill(s->pid, SIGKILL);
                waitpid(s->pid, &status, 0);
            }
        }
        if (s->stdout_fd > 0) close(s->stdout_fd);
        if (s->stderr_fd > 0) close(s->stderr_fd);
        buf_free(&s->stdout_buf); buf_free(&s->stderr_buf);
    }
    free(t);
}

/* ============================================================ *
 *                     registration / dispatch                   *
 * ============================================================ */

void tools_proc_register(cJSON *arr, int allow_exec) {
    {
        cJSON *p = param_obj(NULL);
        add_prop(p, "top_n", "integer", "Top N by RSS (default 20, max 200).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "list_processes",
            "List running processes (pid, ppid, RSS, command). Sorted by RSS descending.",
            p));
    }
    if (!allow_exec) return;
    {
        const char *req[] = {"argv", NULL};
        cJSON *p = param_obj(req);
        cJSON *props = cJSON_GetObjectItem(p, "properties");
        cJSON *argv_def = cJSON_AddObjectToObject(props, "argv");
        cJSON_AddStringToObject(argv_def, "type", "array");
        cJSON_AddStringToObject(argv_def, "description",
            "Argument vector. argv[0] is the executable; rest are arguments. NO shell metacharacters — this is execvp, not bash -c.");
        cJSON *items = cJSON_AddObjectToObject(argv_def, "items");
        cJSON_AddStringToObject(items, "type", "string");

        add_prop(p, "cwd",        "string",  "Working directory.");
        add_prop(p, "profile",    "string",  "Sandbox profile: 'readonly', 'default', 'network', 'build', or 'none' (requires --allow-unsafe-exec).");
        add_prop(p, "stdin",      "string",  "Data to pipe into the child's stdin.");
        add_prop(p, "timeout_ms", "integer", "Kill the child after N ms (default 30000).");

        cJSON_AddItemToArray(arr, make_function_tool(
            "exec_command",
            "Run a command with argv (no shell). Captures stdout/stderr/exit. Per-call sandbox profile controls filesystem/network access. Default profile is 'default' (read-only FS, no network).",
            p));
    }
    {
        const char *req[] = {"argv", NULL};
        cJSON *p = param_obj(req);
        cJSON *props = cJSON_GetObjectItem(p, "properties");
        cJSON *argv_def = cJSON_AddObjectToObject(props, "argv");
        cJSON_AddStringToObject(argv_def, "type", "array");
        cJSON_AddStringToObject(argv_def, "description", "Argument vector.");
        cJSON *items = cJSON_AddObjectToObject(argv_def, "items");
        cJSON_AddStringToObject(items, "type", "string");
        add_prop(p, "cwd",     "string", "Working directory.");
        add_prop(p, "profile", "string", "Sandbox profile name.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "spawn_bg",
            "Spawn a command in the background; returns a handle. Use bg_read to drain output, bg_kill to stop. Output is buffered up to 256KB per stream.",
            p));
    }
    {
        const char *req[] = {"handle", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "handle",              "integer", "Handle from spawn_bg.");
        add_prop(p, "since_offset_stdout", "integer", "Byte offset to resume from (use next_off_stdout from previous call).");
        add_prop(p, "since_offset_stderr", "integer", "Same for stderr.");
        add_prop(p, "wait_ms",             "integer", "Block up to N ms for new output (default 0).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "bg_read",
            "Read pending output (and exit status) from a background process. Returns offsets so the next call can resume without re-reading.",
            p));
    }
    {
        const char *req[] = {"handle", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "handle", "integer", "Handle from spawn_bg.");
        add_prop(p, "signal", "integer", "Signal number (default SIGTERM=15). SIGKILL=9.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "bg_kill",
            "Send a signal to a background process. Default SIGTERM, escalates to SIGKILL after 2s if not exited.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "bg_list",
            "List all background processes this agent owns: handle, pid, argv[0], exited, bytes buffered, age.",
            p));
    }
}

char *tools_proc_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    if (!name) return NULL;
    if (strcmp(name, "list_processes") == 0) return tool_list_processes(args);
    if (!ctx->allow_exec) return NULL;
    if (strcmp(name, "exec_command") == 0) return tool_exec_command(ctx, args);
    if (strcmp(name, "spawn_bg") == 0)     return tool_spawn_bg(ctx, args);
    if (strcmp(name, "bg_read") == 0)      return tool_bg_read(ctx, args);
    if (strcmp(name, "bg_kill") == 0)      return tool_bg_kill(ctx, args);
    if (strcmp(name, "bg_list") == 0)      return tool_bg_list(ctx, args);
    return NULL;
}
