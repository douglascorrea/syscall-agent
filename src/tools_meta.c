#include "tools_meta.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_GREP_RESULTS 200
#define MAX_GREP_LINE_BYTES 4096
#define MAX_SKILL_BYTES  (64 * 1024)
#define DELEGATE_OUTPUT_MAX (256 * 1024)
#define DELEGATE_TIMEOUT_MAX_MS 300000

static char *dup_or_default(cJSON *node, const char *def) {
    if (cJSON_IsString(node) && node->valuestring) return strdup(node->valuestring);
    return def ? strdup(def) : NULL;
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
    size_t room = out->len < DELEGATE_OUTPUT_MAX ? DELEGATE_OUTPUT_MAX - out->len : 0;
    size_t take = (size_t)n < room ? (size_t)n : room;
    if (take > 0) buf_append(out, tmp, take);
    if ((size_t)n > take) *truncated = 1;
}

static char *run_delegate(char *const argv[], const char *cwd, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 120000;
    if (timeout_ms > DELEGATE_TIMEOUT_MAX_MS) timeout_ms = DELEGATE_TIMEOUT_MAX_MS;

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
        if (cwd && *cwd && chdir(cwd) != 0) {
            fprintf(stderr, "chdir(%s): %s\n", cwd, strerror(errno));
            _exit(126);
        }
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
            if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) append_readable(out_pipe[0], &so, &out_eof, &out_trunc);
            if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) append_readable(err_pipe[0], &se, &err_eof, &err_trunc);
        }
        int wn = waitpid(pid, &status, WNOHANG);
        if (wn == pid) {
            append_readable(out_pipe[0], &so, &out_eof, &out_trunc);
            append_readable(err_pipe[0], &se, &err_eof, &err_trunc);
            break;
        }
        if (out_eof && err_eof) {
            waitpid(pid, &status, 0);
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
        "DELEGATE argv[0]=%s exit=%d timed_out=%s duration_ms=%ld\n"
        "--- stdout (%zu bytes%s) ---\n",
        argv[0],
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

static int has_sensitive_word(const char *s) {
    if (!s) return 0;
    char buf[256];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[n] = '\0';
    return strstr(buf, "KEY") || strstr(buf, "TOKEN") ||
           strstr(buf, "SECRET") || strstr(buf, "PASSWORD") ||
           strstr(buf, "AUTH") || strstr(buf, "COOKIE");
}

static int safe_env_name(const char *name) {
    if (!name || !*name) return 0;
    if (strncmp(name, "SYSCALL_AGENT_", 14) == 0) return 1;
    if (strncmp(name, "LLA_", 4) == 0) return 1;
    if (strcmp(name, "OPENROUTER_MODEL") == 0) return 1;
    if (strcmp(name, "SYSTEM_PROMPT_PATH") == 0) return 1;
    if (strcmp(name, "MEMORY_PATH") == 0) return 1;
    if (strcmp(name, "BRAVE_SEARCH_API_KEY") == 0) return 1;
    if (strcmp(name, "OPENROUTER_API_KEY") == 0) return 1;
    if (strcmp(name, "OPENAI_API_KEY") == 0) return 1;
    if (strcmp(name, "GITHUB_TOKEN") == 0 || strcmp(name, "GH_TOKEN") == 0) return 1;
    return 0;
}

static int file_readable(const char *path) {
    return path && access(path, R_OK) == 0;
}

static void append_home_path(Buf *out, const char *suffix) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    buf_printf(out, "%s/%s", home, suffix);
}

static char *tool_auth_status(cJSON *args) {
    (void)args;
    const char *provider = getenv_or("SYSCALL_AGENT_AUTH_PROVIDER", "openrouter");
    const char *openrouter_key = getenv("OPENROUTER_API_KEY");
    const char *openai_key = getenv("OPENAI_API_KEY");
    const char *gh_token = getenv("GH_TOKEN");
    if (!gh_token || !*gh_token) gh_token = getenv("GITHUB_TOKEN");

    Buf codex_path;
    buf_init(&codex_path);
    const char *codex_home = getenv("CODEX_HOME");
    if (codex_home && *codex_home) {
        buf_printf(&codex_path, "%s/auth.json", codex_home);
    } else {
        append_home_path(&codex_path, ".codex/auth.json");
    }

    Buf out;
    buf_init(&out);
    buf_printf(&out, "AUTH_STATUS provider=%s\n---\n", provider);
    buf_printf(&out, "openrouter_api_key: %s\n",
        (openrouter_key && *openrouter_key) ? "set" : "missing");
    buf_printf(&out, "openai_api_key: %s\n",
        (openai_key && *openai_key) ? "set" : "missing");
    buf_printf(&out, "codex_oauth_file: %s (%s)\n",
        codex_path.data ? codex_path.data : "(unknown)",
        file_readable(codex_path.data) ? "present" : "missing");
    buf_printf(&out, "github_copilot_token_hint: %s\n",
        (gh_token && *gh_token) ? "github token env is set" : "missing GH_TOKEN/GITHUB_TOKEN");
    buf_append_cstr(&out,
        "\nNotes:\n"
        "- This agent currently sends model requests through OpenRouter.\n"
        "- Codex OAuth and GitHub Copilot subscription credentials are detected for operator visibility only.\n"
        "- It does not scrape, print, or repurpose those subscription tokens as API keys.\n");

    buf_free(&codex_path);
    return out.data;
}

static char *tool_system_info(cJSON *args) {
    (void)args;
    struct utsname u;
    char cwd[PATH_MAX];
    Buf out;
    buf_init(&out);
    if (uname(&u) == 0) {
        buf_printf(&out,
            "SYSTEM_INFO\n---\n"
            "sysname: %s\n"
            "release: %s\n"
            "version: %s\n"
            "machine: %s\n",
            u.sysname, u.release, u.version, u.machine);
    } else {
        buf_append_cstr(&out, "SYSTEM_INFO\n---\nuname: unavailable\n");
    }
    if (getcwd(cwd, sizeof cwd)) buf_printf(&out, "cwd: %s\n", cwd);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    long page = sysconf(_SC_PAGESIZE);
    if (cpus > 0) buf_printf(&out, "cpus_online: %ld\n", cpus);
    if (page > 0) buf_printf(&out, "page_size: %ld\n", page);
    buf_printf(&out, "exec_tools: controlled by --allow-exec\n");
    return out.data;
}

static char *tool_disk_usage(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), ".");
    if (!path) return strdup("ERROR: missing path");
    struct statvfs v;
    if (statvfs(path, &v) != 0) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: statvfs('%s'): %s", path, strerror(errno));
        free(path);
        return b.data;
    }
    unsigned long long block = v.f_frsize ? v.f_frsize : v.f_bsize;
    unsigned long long total = (unsigned long long)v.f_blocks * block;
    unsigned long long freeb = (unsigned long long)v.f_bfree * block;
    unsigned long long avail = (unsigned long long)v.f_bavail * block;
    Buf out; buf_init(&out);
    buf_printf(&out,
        "DISK_USAGE %s\n---\n"
        "block_size: %llu\n"
        "total_bytes: %llu\n"
        "free_bytes: %llu\n"
        "available_bytes: %llu\n"
        "files_total: %llu\n"
        "files_free: %llu\n",
        path, block, total, freeb, avail,
        (unsigned long long)v.f_files,
        (unsigned long long)v.f_ffree);
    free(path);
    return out.data;
}

static char *tool_env_get(cJSON *args) {
    char *name = dup_or_default(cJSON_GetObjectItem(args, "name"), NULL);
    if (!name) return strdup("ERROR: missing required arg 'name'");
    Buf out; buf_init(&out);
    if (!safe_env_name(name)) {
        buf_printf(&out, "ENV %s -> ERROR: not allowlisted\n", name);
        free(name);
        return out.data;
    }
    const char *v = getenv(name);
    if (!v) {
        buf_printf(&out, "ENV %s -> unset\n", name);
    } else if (has_sensitive_word(name)) {
        buf_printf(&out, "ENV %s -> set (redacted)\n", name);
    } else {
        buf_printf(&out, "ENV %s -> %s\n", name, v);
    }
    free(name);
    return out.data;
}

static char *tool_which(cJSON *args) {
    char *program = dup_or_default(cJSON_GetObjectItem(args, "program"), NULL);
    if (!program) return strdup("ERROR: missing required arg 'program'");
    Buf out; buf_init(&out);
    buf_printf(&out, "WHICH %s\n---\n", program);
    if (strchr(program, '/')) {
        buf_printf(&out, "%s: %s\n", program, access(program, X_OK) == 0 ? "executable" : "not executable");
        free(program);
        return out.data;
    }
    const char *path = getenv_or("PATH", "");
    char *copy = strdup(path);
    if (!copy) {
        free(program);
        buf_append_cstr(&out, "ERROR: out of memory\n");
        return out.data;
    }
    int found = 0;
    for (char *save = NULL, *dir = strtok_r(copy, ":", &save);
         dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", *dir ? dir : ".", program);
        if (access(full, X_OK) == 0) {
            buf_printf(&out, "%s\n", full);
            found = 1;
        }
    }
    if (!found) buf_append_cstr(&out, "(not found)\n");
    free(copy);
    free(program);
    return out.data;
}

static char *tool_file_digest(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    if (!path) return strdup("ERROR: missing required arg 'path'");
    FILE *f = fopen(path, "rb");
    if (!f) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: open('%s'): %s", path, strerror(errno));
        free(path);
        return b.data;
    }
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[8192];
    size_t total = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (n > 0) {
            total += n;
            for (size_t i = 0; i < n; i++) {
                h ^= (uint64_t)buf[i];
                h *= 1099511628211ULL;
            }
        }
        if (n < sizeof buf) break;
    }
    int err = ferror(f);
    fclose(f);
    Buf out; buf_init(&out);
    if (err) {
        buf_printf(&out, "ERROR: read('%s') failed", path);
    } else {
        buf_printf(&out,
            "FILE_DIGEST %s\n---\n"
            "algorithm: fnv1a64\n"
            "bytes: %zu\n"
            "digest: %016llx\n"
            "note: non-cryptographic checksum for change detection, not security\n",
            path, total, (unsigned long long)h);
    }
    free(path);
    return out.data;
}

static char *tool_grep_text(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    char *needle = dup_or_default(cJSON_GetObjectItem(args, "pattern"), NULL);
    cJSON *max_j = cJSON_GetObjectItem(args, "max_results");
    int max_results = cJSON_IsNumber(max_j) ? (int)max_j->valueint : 50;
    if (max_results <= 0 || max_results > MAX_GREP_RESULTS) max_results = 50;
    if (!path || !needle) {
        free(path); free(needle);
        return strdup("ERROR: missing required args 'path' and 'pattern'");
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: open('%s'): %s", path, strerror(errno));
        free(path); free(needle);
        return b.data;
    }
    Buf out; buf_init(&out);
    buf_printf(&out, "GREP_TEXT %s pattern='%s'\n---\n", path, needle);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int line_no = 0, shown = 0, total = 0;
    while ((n = getline(&line, &cap, f)) >= 0) {
        (void)n;
        line_no++;
        if (strstr(line, needle)) {
            total++;
            if (shown < max_results) {
                size_t line_len = strlen(line);
                size_t show = line_len > MAX_GREP_LINE_BYTES ? MAX_GREP_LINE_BYTES : line_len;
                buf_printf(&out, "%d:", line_no);
                buf_append(&out, line, show);
                if (line_len > show) buf_append_cstr(&out, "... [line truncated]\n");
                else if (show == 0 || line[show - 1] != '\n') buf_append_cstr(&out, "\n");
                shown++;
            }
        }
    }
    if (total == 0) buf_append_cstr(&out, "(no matches)\n");
    else if (total > shown) buf_printf(&out, "... (%d total, showing first %d)\n", total, shown);
    free(line);
    fclose(f);
    free(path); free(needle);
    return out.data;
}

static void append_skill_roots(Buf *roots) {
    const char *custom = getenv("SYSCALL_AGENT_SKILLS_DIR");
    if (custom && *custom) buf_printf(roots, "%s\n", custom);
    buf_append_cstr(roots, "skills\n");
    const char *home = getenv("HOME");
    if (home && *home) buf_printf(roots, "%s/.syscall-agent/skills\n", home);
}

static int safe_skill_name(const char *name) {
    if (!name || !*name || strlen(name) > 128) return 0;
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) return 0;
    }
    return strstr(name, "..") == NULL;
}

static char *tool_list_skills(cJSON *args) {
    (void)args;
    Buf roots; buf_init(&roots);
    append_skill_roots(&roots);

    Buf out; buf_init(&out);
    buf_append_cstr(&out, "LIST_SKILLS\n---\n");
    int any = 0;
    char *copy = roots.data ? strdup(roots.data) : NULL;
    for (char *save = NULL, *root = copy ? strtok_r(copy, "\n", &save) : NULL;
         root;
         root = strtok_r(NULL, "\n", &save)) {
        DIR *d = opendir(root);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char full[PATH_MAX];
            snprintf(full, sizeof full, "%s/%s/SKILL.md", root, e->d_name);
            if (file_readable(full)) {
                buf_printf(&out, "%s\t%s\n", e->d_name, full);
                any = 1;
            }
        }
        closedir(d);
    }
    if (!any) {
        buf_append_cstr(&out,
            "(no skills found)\n"
            "Checked SYSCALL_AGENT_SKILLS_DIR, ./skills, and ~/.syscall-agent/skills.\n");
    }
    free(copy);
    buf_free(&roots);
    return out.data;
}

static char *tool_read_skill(cJSON *args) {
    char *name = dup_or_default(cJSON_GetObjectItem(args, "name"), NULL);
    if (!safe_skill_name(name)) {
        free(name);
        return strdup("ERROR: invalid skill name");
    }

    Buf roots; buf_init(&roots);
    append_skill_roots(&roots);

    char found[PATH_MAX] = {0};
    char *copy = roots.data ? strdup(roots.data) : NULL;
    for (char *save = NULL, *root = copy ? strtok_r(copy, "\n", &save) : NULL;
         root && !found[0];
         root = strtok_r(NULL, "\n", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s/SKILL.md", root, name);
        if (file_readable(full)) snprintf(found, sizeof found, "%s", full);
    }
    free(copy);
    buf_free(&roots);

    if (!found[0]) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: skill '%s' not found", name);
        free(name);
        return b.data;
    }

    size_t len = 0;
    char *text = read_text_file(found, &len);
    if (!text) {
        free(name);
        return strdup("ERROR: could not read skill");
    }
    size_t shown = len > MAX_SKILL_BYTES ? MAX_SKILL_BYTES : len;
    Buf out; buf_init(&out);
    buf_printf(&out, "READ_SKILL %s\npath: %s\nbytes: %zu%s\n---\n",
        name, found, len, len > shown ? " (truncated)" : "");
    buf_append(&out, text, shown);
    free(text);
    free(name);
    return out.data;
}

static char *tool_delegate_codex(ToolCtx *ctx, cJSON *args) {
    if (!ctx || !ctx->allow_exec) {
        return strdup("ERROR: delegate_codex requires --allow-exec");
    }
    char *prompt = dup_or_default(cJSON_GetObjectItem(args, "prompt"), NULL);
    char *cwd = dup_or_default(cJSON_GetObjectItem(args, "cwd"), NULL);
    char *model = dup_or_default(cJSON_GetObjectItem(args, "model"), NULL);
    char *mode = dup_or_default(cJSON_GetObjectItem(args, "mode"), "readonly");
    cJSON *timeout_j = cJSON_GetObjectItem(args, "timeout_ms");
    int timeout_ms = cJSON_IsNumber(timeout_j) ? (int)timeout_j->valueint : 120000;
    if (!prompt) {
        free(cwd); free(model); free(mode);
        return strdup("ERROR: missing required arg 'prompt'");
    }
    int write_mode = mode && strcmp(mode, "workspace-write") == 0;
    if (write_mode && !ctx->allow_unsafe_exec) {
        free(prompt); free(cwd); free(model); free(mode);
        return strdup("ERROR: mode='workspace-write' requires --allow-unsafe-exec");
    }
    if (!write_mode && mode && strcmp(mode, "readonly") != 0) {
        free(prompt); free(cwd); free(model); free(mode);
        return strdup("ERROR: mode must be 'readonly' or 'workspace-write'");
    }

    char *argv[16];
    int i = 0;
    argv[i++] = "codex";
    argv[i++] = "exec";
    argv[i++] = "--sandbox";
    argv[i++] = write_mode ? "workspace-write" : "read-only";
    argv[i++] = "--ask-for-approval";
    argv[i++] = "never";
    if (cwd && *cwd) {
        argv[i++] = "-C";
        argv[i++] = cwd;
    }
    if (model && *model) {
        argv[i++] = "-m";
        argv[i++] = model;
    }
    argv[i++] = prompt;
    argv[i] = NULL;

    char *result = run_delegate(argv, cwd, timeout_ms);
    free(prompt); free(cwd); free(model); free(mode);
    return result;
}

static char *tool_delegate_copilot(ToolCtx *ctx, cJSON *args) {
    if (!ctx || !ctx->allow_exec) {
        return strdup("ERROR: delegate_copilot requires --allow-exec");
    }
    char *prompt = dup_or_default(cJSON_GetObjectItem(args, "prompt"), NULL);
    char *cwd = dup_or_default(cJSON_GetObjectItem(args, "cwd"), NULL);
    char *model = dup_or_default(cJSON_GetObjectItem(args, "model"), NULL);
    char *mode = dup_or_default(cJSON_GetObjectItem(args, "mode"), "readonly");
    cJSON *timeout_j = cJSON_GetObjectItem(args, "timeout_ms");
    int timeout_ms = cJSON_IsNumber(timeout_j) ? (int)timeout_j->valueint : 120000;
    if (!prompt) {
        free(cwd); free(model); free(mode);
        return strdup("ERROR: missing required arg 'prompt'");
    }
    int write_mode = mode && strcmp(mode, "workspace-write") == 0;
    if (write_mode && !ctx->allow_unsafe_exec) {
        free(prompt); free(cwd); free(model); free(mode);
        return strdup("ERROR: mode='workspace-write' requires --allow-unsafe-exec");
    }
    if (!write_mode && mode && strcmp(mode, "readonly") != 0) {
        free(prompt); free(cwd); free(model); free(mode);
        return strdup("ERROR: mode must be 'readonly' or 'workspace-write'");
    }

    char *argv[24];
    int i = 0;
    argv[i++] = "copilot";
    argv[i++] = "--no-color";
    argv[i++] = "--stream";
    argv[i++] = "off";
    argv[i++] = "-s";
    if (cwd && *cwd) {
        argv[i++] = "-C";
        argv[i++] = cwd;
    }
    if (model && *model) {
        argv[i++] = "--model";
        argv[i++] = model;
    }
    if (write_mode) {
        argv[i++] = "--allow-all-tools";
        argv[i++] = "--allow-all-paths";
    } else {
        argv[i++] = "--available-tools=read,grep,glob,ls";
        argv[i++] = "--allow-tool=read,grep,glob,ls";
    }
    argv[i++] = "-p";
    argv[i++] = prompt;
    argv[i] = NULL;

    char *result = run_delegate(argv, cwd, timeout_ms);
    free(prompt); free(cwd); free(model); free(mode);
    return result;
}

static void add_delegate_params(cJSON *p) {
    add_prop(p, "prompt", "string", "Task or question to send to the official external CLI.");
    add_prop(p, "cwd", "string", "Working directory for the delegated CLI. Defaults to current directory.");
    add_prop(p, "model", "string", "Optional model id passed through to the delegated CLI.");
    add_prop(p, "mode", "string", "'readonly' (default) or 'workspace-write'. workspace-write requires --allow-unsafe-exec.");
    add_prop(p, "timeout_ms", "integer", "Kill delegated CLI after N ms. Default 120000, cap 300000.");
}

void tools_meta_register(cJSON *arr, int allow_delegates) {
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "auth_status",
            "Report configured model/auth providers without exposing secrets. Detects OpenRouter/OpenAI env, Codex CLI auth file, and GitHub token hints.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "system_info",
            "Return host OS, architecture, working directory, CPU count, and page size.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        add_prop(p, "path", "string", "Path or mount point to inspect. Defaults to current directory.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "disk_usage",
            "Return filesystem capacity and inode counts via statvfs.",
            p));
    }
    {
        const char *req[] = {"name", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "name", "string", "Environment variable name. Sensitive values are redacted.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "env_get",
            "Read a small allowlist of configuration environment variables with secret redaction.",
            p));
    }
    {
        const char *req[] = {"program", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "program", "string", "Executable name to locate on PATH.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "which",
            "Locate executable candidates on PATH without invoking a shell.",
            p));
    }
    {
        const char *req[] = {"path", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "path", "string", "File to checksum.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "file_digest",
            "Compute an FNV-1a 64-bit checksum for fast change detection. Not cryptographic.",
            p));
    }
    {
        const char *req[] = {"path", "pattern", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "path", "string", "Text file to scan.");
        add_prop(p, "pattern", "string", "Literal substring to search for.");
        add_prop(p, "max_results", "integer", "Maximum matching lines to return. Default 50, cap 200.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "grep_text",
            "Search a text file for a literal substring and return matching line numbers.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "list_skills",
            "List local skill packs from SYSCALL_AGENT_SKILLS_DIR, ./skills, and ~/.syscall-agent/skills.",
            p));
    }
    {
        const char *req[] = {"name", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "name", "string", "Skill directory name to read.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "read_skill",
            "Read a local skill pack's SKILL.md by safe skill name.",
            p));
    }
    if (allow_delegates) {
        const char *req[] = {"prompt", NULL};
        cJSON *p = param_obj(req);
        add_delegate_params(p);
        cJSON_AddItemToArray(arr, make_function_tool(
            "delegate_codex",
            "Delegate a task to the official Codex CLI, using its own supported auth. Readonly by default; workspace-write requires --allow-unsafe-exec.",
            p));
    }
    if (allow_delegates) {
        const char *req[] = {"prompt", NULL};
        cJSON *p = param_obj(req);
        add_delegate_params(p);
        cJSON_AddItemToArray(arr, make_function_tool(
            "delegate_copilot",
            "Delegate a task to the official GitHub Copilot CLI, using its own supported auth. Readonly by default; workspace-write requires --allow-unsafe-exec.",
            p));
    }
}

char *tools_meta_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    (void)ctx;
    if (!name) return NULL;
    if (strcmp(name, "auth_status") == 0)  return tool_auth_status(args);
    if (strcmp(name, "system_info") == 0)  return tool_system_info(args);
    if (strcmp(name, "disk_usage") == 0)   return tool_disk_usage(args);
    if (strcmp(name, "env_get") == 0)      return tool_env_get(args);
    if (strcmp(name, "which") == 0)        return tool_which(args);
    if (strcmp(name, "file_digest") == 0)  return tool_file_digest(args);
    if (strcmp(name, "grep_text") == 0)    return tool_grep_text(args);
    if (strcmp(name, "list_skills") == 0)  return tool_list_skills(args);
    if (strcmp(name, "read_skill") == 0)   return tool_read_skill(args);
    if (strcmp(name, "delegate_codex") == 0) return tool_delegate_codex(ctx, args);
    if (strcmp(name, "delegate_copilot") == 0) return tool_delegate_copilot(ctx, args);
    return NULL;
}
