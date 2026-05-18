#include "auth.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AUTH_OUTPUT_MAX (256 * 1024)
#define AUTH_TIMEOUT_MAX_MS 600000

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void set_err(char *err, size_t err_cap, const char *msg) {
    if (!err || err_cap == 0) return;
    snprintf(err, err_cap, "%s", msg ? msg : "auth error");
}

int auth_build_login_argv(const char *provider,
                          const char *host,
                          int free_flow,
                          char **argv,
                          size_t argv_cap,
                          char *err,
                          size_t err_cap) {
    if (!argv || argv_cap < 4) {
        set_err(err, err_cap, "argv buffer is too small");
        return -1;
    }
    if (!provider || !*provider) {
        set_err(err, err_cap, "provider must be codex/openai or copilot/github");
        return -1;
    }

    size_t i = 0;
    if (streq_ci(provider, "codex") ||
        streq_ci(provider, "openai") ||
        streq_ci(provider, "openai/codex")) {
        argv[i++] = "codex";
        argv[i++] = free_flow ? "--free" : "--login";
        argv[i] = NULL;
        return 0;
    }

    if (streq_ci(provider, "copilot") || streq_ci(provider, "github")) {
        argv[i++] = "copilot";
        argv[i++] = "login";
        if (host && *host) {
            if (i + 3 > argv_cap) {
                set_err(err, err_cap, "argv buffer is too small for host");
                return -1;
            }
            argv[i++] = "--host";
            argv[i++] = (char *)host;
        }
        argv[i] = NULL;
        return 0;
    }

    set_err(err, err_cap, "unknown auth provider; use codex/openai or copilot/github");
    return -1;
}

char *auth_login_preview(const char *provider, const char *host, int free_flow) {
    char *argv[CEZAR_AUTH_ARGV_MAX] = {0};
    char err[160] = {0};
    if (auth_build_login_argv(provider, host, free_flow,
            argv, CEZAR_AUTH_ARGV_MAX, err, sizeof err) != 0) {
        Buf b;
        buf_init(&b);
        buf_printf(&b, "ERROR: %s\n", err[0] ? err : "could not build auth command");
        return b.data;
    }

    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "AUTH_LOGIN dry_run=true\n---\n");
    for (size_t i = 0; argv[i]; i++) {
        if (i) buf_append_cstr(&out, " ");
        buf_append_cstr(&out, argv[i]);
    }
    buf_append_cstr(&out, "\n");
    return out.data;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void append_readable(int fd, Buf *out, int *eof, int *truncated) {
    if (*eof) return;
    char tmp[4096];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n == 0) {
        *eof = 1;
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        *eof = 1;
        return;
    }
    size_t room = out->len < AUTH_OUTPUT_MAX ? AUTH_OUTPUT_MAX - out->len : 0;
    size_t take = (size_t)n < room ? (size_t)n : room;
    if (take > 0) buf_append(out, tmp, take);
    if ((size_t)n > take) *truncated = 1;
}

char *auth_login_capture(const char *provider,
                         const char *host,
                         int free_flow,
                         int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 300000;
    if (timeout_ms > AUTH_TIMEOUT_MAX_MS) timeout_ms = AUTH_TIMEOUT_MAX_MS;

    char *argv[CEZAR_AUTH_ARGV_MAX] = {0};
    char err[160] = {0};
    if (auth_build_login_argv(provider, host, free_flow,
            argv, CEZAR_AUTH_ARGV_MAX, err, sizeof err) != 0) {
        Buf b;
        buf_init(&b);
        buf_printf(&b, "ERROR: %s\n", err[0] ? err : "could not build auth command");
        return b.data;
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        return strdup("ERROR: pipe failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return strdup("ERROR: fork failed");
    }
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    Buf so, se;
    buf_init(&so);
    buf_init(&se);
    int out_eof = 0, err_eof = 0, out_trunc = 0, err_trunc = 0;
    int status = 0, timed_out = 0;
    long start = now_ms();
    for (;;) {
        struct pollfd pfds[2] = {
            { .fd = out_eof ? -1 : out_pipe[0], .events = POLLIN },
            { .fd = err_eof ? -1 : err_pipe[0], .events = POLLIN },
        };
        int remaining = timeout_ms - (int)(now_ms() - start);
        if (remaining <= 0) {
            timed_out = 1;
            break;
        }
        int pn = poll(pfds, 2, remaining < 250 ? remaining : 250);
        if (pn > 0) {
            if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                append_readable(out_pipe[0], &so, &out_eof, &out_trunc);
            }
            if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                append_readable(err_pipe[0], &se, &err_eof, &err_trunc);
            }
        }
        int wn = waitpid(pid, &status, WNOHANG);
        if (wn == pid) {
            append_readable(out_pipe[0], &so, &out_eof, &out_trunc);
            append_readable(err_pipe[0], &se, &err_eof, &err_trunc);
            break;
        }
    }
    if (timed_out) {
        kill(pid, SIGTERM);
        usleep(200 * 1000);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    int exit_code = -1;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);

    Buf out;
    buf_init(&out);
    buf_printf(&out,
        "AUTH_LOGIN provider=%s exit=%d timed_out=%s duration_ms=%ld\n"
        "--- stdout (%zu bytes%s) ---\n",
        provider ? provider : "",
        exit_code,
        timed_out ? "true" : "false",
        now_ms() - start,
        so.len,
        out_trunc ? ", truncated" : "");
    if (so.data) buf_append(&out, so.data, so.len);
    buf_printf(&out, "\n--- stderr (%zu bytes%s) ---\n",
        se.len,
        err_trunc ? ", truncated" : "");
    if (se.data) buf_append(&out, se.data, se.len);
    buf_free(&so);
    buf_free(&se);
    return out.data;
}

int auth_login_interactive(const char *provider,
                           const char *host,
                           int free_flow) {
    char *argv[CEZAR_AUTH_ARGV_MAX] = {0};
    char err[160] = {0};
    if (auth_build_login_argv(provider, host, free_flow,
            argv, CEZAR_AUTH_ARGV_MAX, err, sizeof err) != 0) {
        fprintf(stderr, "auth login: %s\n", err[0] ? err : "could not build auth command");
        return 2;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
