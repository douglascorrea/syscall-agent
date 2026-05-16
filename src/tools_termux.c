#include "tools_termux.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TERMUX_OUTPUT_MAX        (64 * 1024)
#define TERMUX_TIMEOUT_DEFAULT_MS 15000
#define TERMUX_TIMEOUT_MAX_MS     30000
#define TERMUX_TITLE_MAX_BYTES      160
#define TERMUX_TEXT_MAX_BYTES      4096
#define TERMUX_VIBRATE_MAX_MS      5000

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

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
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
    size_t room = out->len < TERMUX_OUTPUT_MAX ? TERMUX_OUTPUT_MAX - out->len : 0;
    size_t take = (size_t)n < room ? (size_t)n : room;
    if (take > 0) buf_append(out, tmp, take);
    if ((size_t)n > take) *truncated = 1;
}

static int command_exists(const char *program) {
    if (!program || !*program) return 0;
    if (strchr(program, '/')) return access(program, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path || !*path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;

    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char candidate[PATH_MAX];
        const char *base = *dir ? dir : ".";
        int n = snprintf(candidate, sizeof candidate, "%s/%s", base, program);
        if (n < 0 || (size_t)n >= sizeof candidate) continue;
        if (access(candidate, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static void append_cmd_presence(Buf *out, const char *program) {
    buf_printf(out, "  %-30s %s\n", program, command_exists(program) ? "present" : "missing");
}

static int is_termux_env(void) {
    const char *prefix = getenv("PREFIX");
    if (prefix && strstr(prefix, "com.termux/files/usr")) return 1;
    if (getenv("TERMUX_VERSION")) return 1;
    return file_exists("/data/data/com.termux/files/usr/bin/pkg");
}

static char *missing_command_error(const char *program) {
    Buf out;
    buf_init(&out);
    buf_printf(&out, "ERROR: %s not found on PATH\n", program);
    if (strcmp(program, "termux-wake-lock") == 0 || strcmp(program, "termux-wake-unlock") == 0) {
        buf_append_cstr(&out,
            "Install or update Termux tools:\n"
            "  pkg install termux-tools\n");
    } else {
        buf_append_cstr(&out,
            "Install the Termux:API app from the same source as Termux, then run:\n"
            "  pkg install termux-api\n");
    }
    return out.data;
}

static char *run_termux_command_input(const char *label, char *const argv[],
                                      const char *stdin_data, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = TERMUX_TIMEOUT_DEFAULT_MS;
    if (timeout_ms > TERMUX_TIMEOUT_MAX_MS) timeout_ms = TERMUX_TIMEOUT_MAX_MS;

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if ((stdin_data && pipe(in_pipe) != 0) ||
        pipe(out_pipe) != 0 ||
        pipe(err_pipe) != 0) {
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        return strdup("ERROR: pipe failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return strdup("ERROR: fork failed");
    }
    if (pid == 0) {
        if (stdin_data) dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    if (in_pipe[0] >= 0) close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (stdin_data) {
        void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
        const char *p = stdin_data;
        size_t left = strlen(stdin_data);
        while (left > 0) {
            ssize_t n = write(in_pipe[1], p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            p += n;
            left -= (size_t)n;
        }
        close(in_pipe[1]);
        if (old_sigpipe != SIG_ERR) signal(SIGPIPE, old_sigpipe);
    }
    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    Buf so, se;
    buf_init(&so);
    buf_init(&se);
    int out_eof = 0, err_eof = 0, out_trunc = 0, err_trunc = 0;
    int status = 0, timed_out = 0, exited = 0;
    long start = now_ms();

    for (;;) {
        if (!exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) exited = 1;
            else if (w < 0 && errno != EINTR) exited = 1;
        }

        if (!exited && now_ms() - start > timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = 1;
            exited = 1;
        }

        append_readable(out_pipe[0], &so, &out_eof, &out_trunc);
        append_readable(err_pipe[0], &se, &err_eof, &err_trunc);
        if (exited && out_eof && err_eof) break;

        struct pollfd pfds[2] = {
            { .fd = out_eof ? -1 : out_pipe[0], .events = POLLIN },
            { .fd = err_eof ? -1 : err_pipe[0], .events = POLLIN },
        };
        poll(pfds, 2, 50);
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    Buf out;
    buf_init(&out);
    buf_printf(&out, "%s\n---\n", label);
    if (so.len) {
        buf_append_cstr(&out, "stdout:\n");
        buf_append(&out, so.data, so.len);
        if (so.data[so.len - 1] != '\n') buf_append_cstr(&out, "\n");
        if (out_trunc) buf_append_cstr(&out, "[stdout truncated]\n");
    }
    if (se.len) {
        buf_append_cstr(&out, "stderr:\n");
        buf_append(&out, se.data, se.len);
        if (se.data[se.len - 1] != '\n') buf_append_cstr(&out, "\n");
        if (err_trunc) buf_append_cstr(&out, "[stderr truncated]\n");
    }
    if (!so.len && !se.len) buf_append_cstr(&out, "(no output)\n");
    if (timed_out) {
        buf_printf(&out, "timed_out: yes after %dms\n", timeout_ms);
    } else if (WIFEXITED(status)) {
        buf_printf(&out, "exit_code: %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        buf_printf(&out, "signal: %d\n", WTERMSIG(status));
    }

    buf_free(&so);
    buf_free(&se);
    return out.data;
}

static char *run_api_command_input(const char *label, const char *program,
                                   char *const argv[], const char *stdin_data) {
    if (!command_exists(program)) return missing_command_error(program);
    return run_termux_command_input(label, argv, stdin_data, TERMUX_TIMEOUT_DEFAULT_MS);
}

static char *run_api_command(const char *label, const char *program, char *const argv[]) {
    return run_api_command_input(label, program, argv, NULL);
}

static const char *string_arg(cJSON *args, const char *name) {
    cJSON *node = cJSON_GetObjectItem(args, name);
    return cJSON_IsString(node) && node->valuestring ? node->valuestring : NULL;
}

static char *required_string_error(const char *name) {
    Buf out;
    buf_init(&out);
    buf_printf(&out, "ERROR: missing required arg '%s'", name);
    return out.data;
}

static char *string_too_long_error(const char *name, size_t max_bytes) {
    Buf out;
    buf_init(&out);
    buf_printf(&out, "ERROR: arg '%s' exceeds %zu bytes", name, max_bytes);
    return out.data;
}

static int path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static void append_storage_path(Buf *out, const char *home, const char *name, const char *rel) {
    char path[PATH_MAX];
    int n;
    if (rel && *rel) n = snprintf(path, sizeof path, "%s/storage/%s", home, rel);
    else            n = snprintf(path, sizeof path, "%s/storage", home);
    if (n < 0 || (size_t)n >= sizeof path) {
        buf_printf(out, "  %-14s too-long\n", name);
        return;
    }
    buf_printf(out, "  %-14s %s  %s\n", name, path_exists(path) ? "present" : "missing", path);
}

static char *tool_termux_info(cJSON *args) {
    (void)args;
    const char *prefix = getenv("PREFIX");
    const char *home = getenv("HOME");
    const char *android_root = getenv("ANDROID_ROOT");
    const char *termux_version = getenv("TERMUX_VERSION");

    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "TERMUX_INFO\n---\n");
    buf_printf(&out, "detected: %s\n", is_termux_env() ? "yes" : "no");
    buf_printf(&out, "prefix: %s\n", prefix && *prefix ? prefix : "(unset)");
    buf_printf(&out, "home: %s\n", home && *home ? home : "(unset)");
    buf_printf(&out, "android_root: %s\n", android_root && *android_root ? android_root : "(unset)");
    buf_printf(&out, "termux_version: %s\n", termux_version && *termux_version ? termux_version : "(unset)");
    buf_append_cstr(&out, "commands:\n");
    append_cmd_presence(&out, "pkg");
    append_cmd_presence(&out, "termux-info");
    append_cmd_presence(&out, "termux-setup-storage");
    append_cmd_presence(&out, "termux-api");
    append_cmd_presence(&out, "termux-battery-status");
    append_cmd_presence(&out, "termux-notification");
    return out.data;
}

static char *tool_termux_api_status(cJSON *args) {
    (void)args;
    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "TERMUX_API_STATUS\n---\n");
    buf_printf(&out, "detected_termux: %s\n", is_termux_env() ? "yes" : "no");
    buf_printf(&out, "termux_api_package: %s\n", command_exists("termux-api") ? "present" : "missing");
    buf_append_cstr(&out, "device_commands:\n");
    append_cmd_presence(&out, "termux-battery-status");
    append_cmd_presence(&out, "termux-wifi-connectioninfo");
    append_cmd_presence(&out, "termux-clipboard-get");
    append_cmd_presence(&out, "termux-clipboard-set");
    append_cmd_presence(&out, "termux-notification");
    append_cmd_presence(&out, "termux-vibrate");
    append_cmd_presence(&out, "termux-wake-lock");
    append_cmd_presence(&out, "termux-wake-unlock");
    buf_append_cstr(&out,
        "hint: install the Termux:API app from the same source as Termux, then run `pkg install termux-api`.\n");
    return out.data;
}

static char *tool_termux_storage_status(cJSON *args) {
    (void)args;
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/data/data/com.termux/files/home";

    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "TERMUX_STORAGE_STATUS\n---\n");
    buf_printf(&out, "home: %s\n", home);
    buf_printf(&out, "termux_setup_storage: %s\n", command_exists("termux-setup-storage") ? "present" : "missing");
    buf_append_cstr(&out, "storage_links:\n");
    append_storage_path(&out, home, "root", "");
    append_storage_path(&out, home, "shared", "shared");
    append_storage_path(&out, home, "downloads", "downloads");
    append_storage_path(&out, home, "dcim", "dcim");
    append_storage_path(&out, home, "pictures", "pictures");
    append_storage_path(&out, home, "music", "music");
    append_storage_path(&out, home, "movies", "movies");
    buf_append_cstr(&out,
        "hint: run `termux-setup-storage` in Termux and approve the Android storage prompt if links are missing.\n");
    return out.data;
}

static char *tool_termux_battery_status(cJSON *args) {
    (void)args;
    char *argv[] = {"termux-battery-status", NULL};
    return run_api_command("TERMUX_BATTERY_STATUS", "termux-battery-status", argv);
}

static char *tool_termux_wifi_info(cJSON *args) {
    (void)args;
    char *argv[] = {"termux-wifi-connectioninfo", NULL};
    return run_api_command("TERMUX_WIFI_INFO", "termux-wifi-connectioninfo", argv);
}

static char *tool_termux_clipboard_get(cJSON *args) {
    (void)args;
    char *argv[] = {"termux-clipboard-get", NULL};
    return run_api_command("TERMUX_CLIPBOARD_GET", "termux-clipboard-get", argv);
}

static char *tool_termux_clipboard_set(cJSON *args) {
    const char *text = string_arg(args, "text");
    if (!text || !*text) return required_string_error("text");
    if (strlen(text) > TERMUX_TEXT_MAX_BYTES) return string_too_long_error("text", TERMUX_TEXT_MAX_BYTES);
    char *argv[] = {"termux-clipboard-set", NULL};
    return run_api_command_input("TERMUX_CLIPBOARD_SET", "termux-clipboard-set", argv, text);
}

static char *tool_termux_notification(cJSON *args) {
    const char *title = string_arg(args, "title");
    const char *content = string_arg(args, "content");
    if (!title || !*title) return required_string_error("title");
    if (!content || !*content) return required_string_error("content");
    if (strlen(title) > TERMUX_TITLE_MAX_BYTES) return string_too_long_error("title", TERMUX_TITLE_MAX_BYTES);
    if (strlen(content) > TERMUX_TEXT_MAX_BYTES) return string_too_long_error("content", TERMUX_TEXT_MAX_BYTES);
    char *argv[] = {
        "termux-notification",
        "--title", (char *)title,
        "--content", (char *)content,
        NULL
    };
    return run_api_command("TERMUX_NOTIFICATION", "termux-notification", argv);
}

static char *tool_termux_vibrate(cJSON *args) {
    cJSON *node = cJSON_GetObjectItem(args, "duration_ms");
    int duration_ms = cJSON_IsNumber(node) ? (int)node->valueint : 300;
    if (duration_ms < 1 || duration_ms > TERMUX_VIBRATE_MAX_MS) {
        return strdup("ERROR: duration_ms must be between 1 and 5000");
    }
    char duration_buf[32];
    snprintf(duration_buf, sizeof duration_buf, "%d", duration_ms);
    char *argv[] = {"termux-vibrate", "-d", duration_buf, NULL};
    return run_api_command("TERMUX_VIBRATE", "termux-vibrate", argv);
}

static char *tool_termux_wake_lock(cJSON *args) {
    const char *action = string_arg(args, "action");
    if (!action || !*action) return required_string_error("action");
    if (strcmp(action, "lock") != 0 && strcmp(action, "unlock") != 0) {
        return strdup("ERROR: action must be 'lock' or 'unlock'");
    }
    const char *program = strcmp(action, "lock") == 0 ? "termux-wake-lock" : "termux-wake-unlock";
    char *argv[] = {(char *)program, NULL};
    return run_api_command("TERMUX_WAKE_LOCK", program, argv);
}

void tools_termux_register(cJSON *arr) {
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_info",
            "Detect Termux/Android environment details and key Termux command availability.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_api_status",
            "Report Termux:API helper command availability and setup hints.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_storage_status",
            "Check Android shared-storage symlinks created by termux-setup-storage.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_battery_status",
            "Read Android battery status through Termux:API.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_wifi_info",
            "Read current Wi-Fi connection details through Termux:API.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_clipboard_get",
            "Read the Android clipboard through Termux:API.",
            p));
    }
    {
        const char *req[] = {"text", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "text", "string", "Text to place on the Android clipboard. Max 4096 bytes.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_clipboard_set",
            "Set the Android clipboard through Termux:API.",
            p));
    }
    {
        const char *req[] = {"title", "content", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "title", "string", "Notification title. Max 160 bytes.");
        add_prop(p, "content", "string", "Notification body. Max 4096 bytes.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_notification",
            "Show an Android notification through Termux:API.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        add_prop(p, "duration_ms", "integer", "Vibration duration in milliseconds. Default 300, range 1..5000.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_vibrate",
            "Vibrate the Android device through Termux:API.",
            p));
    }
    {
        const char *req[] = {"action", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "action", "string", "Either 'lock' to acquire a Termux wake lock or 'unlock' to release it.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "termux_wake_lock",
            "Acquire or release Termux's Android wake lock.",
            p));
    }
}

char *tools_termux_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    (void)ctx;
    if (!name) return NULL;
    if (strcmp(name, "termux_info") == 0)             return tool_termux_info(args);
    if (strcmp(name, "termux_api_status") == 0)       return tool_termux_api_status(args);
    if (strcmp(name, "termux_storage_status") == 0)   return tool_termux_storage_status(args);
    if (strcmp(name, "termux_battery_status") == 0)   return tool_termux_battery_status(args);
    if (strcmp(name, "termux_wifi_info") == 0)        return tool_termux_wifi_info(args);
    if (strcmp(name, "termux_clipboard_get") == 0)    return tool_termux_clipboard_get(args);
    if (strcmp(name, "termux_clipboard_set") == 0)    return tool_termux_clipboard_set(args);
    if (strcmp(name, "termux_notification") == 0)     return tool_termux_notification(args);
    if (strcmp(name, "termux_vibrate") == 0)          return tool_termux_vibrate(args);
    if (strcmp(name, "termux_wake_lock") == 0)        return tool_termux_wake_lock(args);
    return NULL;
}
