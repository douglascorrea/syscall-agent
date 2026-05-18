#include "extensions.h"
#include "session_store.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXT_OUTPUT_MAX (256 * 1024)
#define EXT_TIMEOUT_MS 120000
#define EXT_ARGV_MAX 32

typedef int (*ManifestVisitor)(const char *path, void *userdata);

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int safe_tool_name(const char *name) {
    if (!name || !*name || strlen(name) > 64) return 0;
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) return 0;
    for (const char *p = name + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    return 1;
}

static cJSON *make_function_tool(const char *name, const char *desc, cJSON *params) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "description", desc ? desc : "Extension tool.");
    cJSON_AddItemToObject(fn, "parameters", params);
    return tool;
}

static cJSON *empty_params(void) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "object");
    cJSON_AddObjectToObject(p, "properties");
    cJSON_AddArrayToObject(p, "required");
    return p;
}

static void append_extension_roots(Buf *roots) {
    const char *custom = getenv("CEZAR_EXTENSIONS_DIR");
    if (custom && *custom) buf_printf(roots, "%s\n", custom);
    buf_append_cstr(roots, "extensions\n");
    const char *home = getenv("HOME");
    if (home && *home) buf_printf(roots, "%s/.cezar/extensions\n", home);
}

static int has_json_suffix(const char *name) {
    size_t n = name ? strlen(name) : 0;
    return n > 5 && strcmp(name + n - 5, ".json") == 0;
}

static int visit_manifest_path(const char *path, ManifestVisitor visitor, void *userdata) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (S_ISREG(st.st_mode) && has_json_suffix(path)) return visitor(path, userdata);
    if (S_ISDIR(st.st_mode)) {
        char manifest[PATH_MAX];
        snprintf(manifest, sizeof manifest, "%s/extension.json", path);
        if (stat(manifest, &st) == 0 && S_ISREG(st.st_mode)) {
            return visitor(manifest, userdata);
        }
    }
    return 0;
}

static int visit_manifests(ManifestVisitor visitor, void *userdata) {
    Buf roots;
    buf_init(&roots);
    append_extension_roots(&roots);
    char *copy = roots.data ? strdup(roots.data) : NULL;
    int rc = 0;
    for (char *save = NULL, *root = copy ? strtok_r(copy, "\n", &save) : NULL;
         root && rc == 0;
         root = strtok_r(NULL, "\n", &save)) {
        DIR *d = opendir(root);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && rc == 0) {
            if (e->d_name[0] == '.') continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", root, e->d_name);
            rc = visit_manifest_path(path, visitor, userdata);
        }
        closedir(d);
    }
    free(copy);
    buf_free(&roots);
    return rc;
}

static cJSON *load_manifest(const char *path) {
    size_t len = 0;
    char *text = read_text_file(path, &len);
    if (!text) return NULL;
    cJSON *root = cJSON_Parse(text);
    free(text);
    return root;
}

static int add_manifest_tools(const char *path, void *userdata) {
    cJSON *arr = userdata;
    cJSON *root = load_manifest(path);
    if (!root) return 0;
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    int n = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *params = cJSON_GetObjectItem(tool, "parameters");
        if (!cJSON_IsString(name) || !safe_tool_name(name->valuestring)) continue;
        cJSON *params_copy = cJSON_IsObject(params) ? cJSON_Duplicate(params, 1) : empty_params();
        cJSON_AddItemToArray(arr, make_function_tool(
            name->valuestring,
            cJSON_IsString(desc) ? desc->valuestring : "Extension tool.",
            params_copy));
    }
    cJSON_Delete(root);
    return 0;
}

static int append_manifest_text(const char *path, void *userdata) {
    Buf *out = userdata;
    cJSON *root = load_manifest(path);
    if (!root) return 0;
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    buf_printf(out, "%s\t%s\t%s\n",
        cJSON_IsString(name) ? name->valuestring : "(unnamed)",
        cJSON_IsString(desc) ? desc->valuestring : "",
        path);
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    int n = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *tn = cJSON_GetObjectItem(tool, "name");
        cJSON *td = cJSON_GetObjectItem(tool, "description");
        if (cJSON_IsString(tn)) {
            buf_printf(out, "  tool:%s\t%s\n",
                tn->valuestring,
                cJSON_IsString(td) ? td->valuestring : "");
        }
    }
    cJSON_Delete(root);
    return 0;
}

char *extensions_list_text(void) {
    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "EXTENSIONS\n---\n");
    size_t before = out.len;
    visit_manifests(append_manifest_text, &out);
    if (out.len == before) {
        buf_append_cstr(&out,
            "(no extensions found)\n"
            "Checked CEZAR_EXTENSIONS_DIR, ./extensions, and ~/.cezar/extensions.\n");
    }
    return out.data;
}

void extensions_register(cJSON *arr, const ToolCtx *ctx) {
    {
        cJSON *p = empty_params();
        cJSON_AddItemToArray(arr, make_function_tool(
            "list_extensions",
            "List extension manifests and the tools they register.",
            p));
    }
    if (ctx && ctx->allow_exec) {
        visit_manifests(add_manifest_tools, arr);
    }
}

typedef struct {
    const char *tool_name;
    char *argv[EXT_ARGV_MAX];
    int found;
} FindTool;

static int find_tool_command(const char *path, void *userdata) {
    FindTool *ft = userdata;
    cJSON *root = load_manifest(path);
    if (!root) return 0;
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    int n = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    for (int i = 0; i < n && !ft->found; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        if (!cJSON_IsString(name) || strcmp(name->valuestring, ft->tool_name) != 0) continue;
        cJSON *cmd = cJSON_GetObjectItem(tool, "command");
        int cn = cJSON_IsArray(cmd) ? cJSON_GetArraySize(cmd) : 0;
        if (cn <= 0 || cn >= EXT_ARGV_MAX) continue;
        int ok = 1;
        for (int j = 0; j < cn; j++) {
            cJSON *part = cJSON_GetArrayItem(cmd, j);
            if (!cJSON_IsString(part) || !part->valuestring || !*part->valuestring) {
                ok = 0;
                break;
            }
            ft->argv[j] = strdup(part->valuestring);
            if (!ft->argv[j]) ok = 0;
        }
        if (ok) {
            ft->argv[cn] = NULL;
            ft->found = 1;
        }
    }
    cJSON_Delete(root);
    return ft->found ? 1 : 0;
}

static void free_found_tool(FindTool *ft) {
    for (size_t i = 0; i < EXT_ARGV_MAX && ft->argv[i]; i++) {
        free(ft->argv[i]);
        ft->argv[i] = NULL;
    }
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
    size_t room = out->len < EXT_OUTPUT_MAX ? EXT_OUTPUT_MAX - out->len : 0;
    size_t take = (size_t)n < room ? (size_t)n : room;
    if (take > 0) buf_append(out, tmp, take);
    if ((size_t)n > take) *truncated = 1;
}

static char *run_extension_tool(const char *name, char *const argv[], cJSON *args) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
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
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return strdup("ERROR: fork failed");
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        setenv("CEZAR_EXTENSION_TOOL", name ? name : "", 1);
        execvp(argv[0], argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    char *input = args ? cJSON_PrintUnformatted(args) : strdup("{}");
    if (input) {
        (void)write(in_pipe[1], input, strlen(input));
        (void)write(in_pipe[1], "\n", 1);
    }
    free(input);
    close(in_pipe[1]);
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
        int remaining = EXT_TIMEOUT_MS - (int)(now_ms() - start);
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
        "EXTENSION_TOOL name=%s exit=%d timed_out=%s duration_ms=%ld\n"
        "--- stdout (%zu bytes%s) ---\n",
        name ? name : "",
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

char *extensions_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    if (!name) return NULL;
    if (strcmp(name, "list_extensions") == 0) return extensions_list_text();
    if (!safe_tool_name(name)) return NULL;

    FindTool ft;
    memset(&ft, 0, sizeof ft);
    ft.tool_name = name;
    visit_manifests(find_tool_command, &ft);
    if (!ft.found) return NULL;
    if (!ctx || !ctx->allow_exec) {
        free_found_tool(&ft);
        return strdup("ERROR: extension tools require --allow-exec");
    }
    char *result = run_extension_tool(name, ft.argv, args);
    free_found_tool(&ft);
    return result;
}
