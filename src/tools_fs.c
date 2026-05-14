#include "tools_fs.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_RANGE_BYTES   (256 * 1024)
#define MAX_LIST_ENTRIES   1000

/* ---------- helpers ---------- */

static const char *file_type_name(mode_t m) {
    if (S_ISDIR(m))  return "dir";
    if (S_ISREG(m))  return "file";
    if (S_ISLNK(m))  return "symlink";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    if (S_ISCHR(m))  return "chardev";
    if (S_ISBLK(m))  return "blockdev";
    return "other";
}

static void format_iso8601(time_t t, char out[32]) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static char *dup_or_default(cJSON *node, const char *def) {
    if (cJSON_IsString(node) && node->valuestring) return strdup(node->valuestring);
    return def ? strdup(def) : NULL;
}

/* ---------- stat ---------- */

static char *tool_stat(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    if (!path) return strdup("ERROR: missing required arg 'path'");

    struct stat st;
    if (lstat(path, &st) != 0) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: stat('%s'): %s", path, strerror(errno));
        free(path);
        return b.data;
    }
    char mtime[32], atime[32];
    format_iso8601(st.st_mtime, mtime);
    format_iso8601(st.st_atime, atime);

    Buf out; buf_init(&out);
    buf_printf(&out,
        "STAT %s\n"
        "type: %s\n"
        "size: %lld\n"
        "mode: %04o\n"
        "uid: %u\n"
        "gid: %u\n"
        "mtime: %s\n"
        "atime: %s\n",
        path,
        file_type_name(st.st_mode),
        (long long)st.st_size,
        (unsigned)(st.st_mode & 07777),
        (unsigned)st.st_uid,
        (unsigned)st.st_gid,
        mtime, atime);

    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t n = readlink(path, target, sizeof target - 1);
        if (n > 0) {
            target[n] = '\0';
            buf_printf(&out, "symlink_target: %s\n", target);
        }
    }
    free(path);
    return out.data;
}

/* ---------- list_dir ---------- */

static char *tool_list_dir(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), ".");
    cJSON *hidden_j = cJSON_GetObjectItem(args, "show_hidden");
    cJSON *max_j    = cJSON_GetObjectItem(args, "max_entries");
    int show_hidden = cJSON_IsTrue(hidden_j);
    int max_entries = cJSON_IsNumber(max_j) ? (int)max_j->valueint : 200;
    if (max_entries <= 0) max_entries = 200;
    if (max_entries > MAX_LIST_ENTRIES) max_entries = MAX_LIST_ENTRIES;

    DIR *d = opendir(path);
    if (!d) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: opendir('%s'): %s", path, strerror(errno));
        free(path);
        return b.data;
    }

    Buf out; buf_init(&out);
    buf_printf(&out, "LIST_DIR %s\n---\n", path);

    int total = 0, shown = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            int is_dotdot = (e->d_name[1] == '\0') ||
                            (e->d_name[1] == '.' && e->d_name[2] == '\0');
            if (is_dotdot || !show_hidden) continue;
        }
        total++;
        if (shown >= max_entries) continue;

        char full[4096];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        char mtime[32];
        format_iso8601(st.st_mtime, mtime);
        buf_printf(&out, "%-10s  %10lld  %s  %s%s\n",
            file_type_name(st.st_mode),
            (long long)st.st_size,
            mtime,
            e->d_name,
            S_ISDIR(st.st_mode) ? "/" : "");
        shown++;
    }
    closedir(d);
    if (total > shown) {
        buf_printf(&out, "... (%d more entries; raise max_entries to see them)\n",
            total - shown);
    } else if (total == 0) {
        buf_append_cstr(&out, "(empty)\n");
    }
    free(path);
    return out.data;
}

/* ---------- write_file (atomic via mkstemp+rename) ---------- */

static char *tool_write_file(cJSON *args) {
    char *path    = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    char *content = dup_or_default(cJSON_GetObjectItem(args, "content"), NULL);
    cJSON *mode_j = cJSON_GetObjectItem(args, "mode");
    int mode = cJSON_IsNumber(mode_j) ? (int)mode_j->valueint : 0644;

    if (!path || !content) {
        free(path); free(content);
        return strdup("ERROR: missing required args 'path' and 'content'");
    }

    /* derive temp template in same directory as target so rename() is atomic */
    char *path_copy = strdup(path);
    char *dir = strdup(path);
    const char *slash = strrchr(dir, '/');
    if (slash) {
        ((char *)slash)[0] = '\0';
        if (dir[0] == '\0') { free(dir); dir = strdup("/"); }
    } else {
        free(dir);
        dir = strdup(".");
    }

    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%s/.lla_atomic.XXXXXX", dir);
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: mkstemp in '%s': %s", dir, strerror(errno));
        free(dir); free(path_copy); free(path); free(content);
        return b.data;
    }

    size_t len = strlen(content);
    ssize_t wn = write(fd, content, len);
    if (wn < 0 || (size_t)wn != len) {
        close(fd); unlink(tmpl);
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: write failed: %s", strerror(errno));
        free(dir); free(path_copy); free(path); free(content);
        return b.data;
    }
    fchmod(fd, (mode_t)mode);
    close(fd);

    if (rename(tmpl, path_copy) != 0) {
        unlink(tmpl);
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: rename to '%s': %s", path_copy, strerror(errno));
        free(dir); free(path_copy); free(path); free(content);
        return b.data;
    }

    Buf out; buf_init(&out);
    buf_printf(&out, "OK: wrote %zu bytes to %s (mode %04o, atomic)\n",
        len, path_copy, mode);
    free(dir); free(path_copy); free(path); free(content);
    return out.data;
}

/* ---------- read_file_range (mmap-backed) ---------- */

static char *tool_read_file_range(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    cJSON *off_j = cJSON_GetObjectItem(args, "offset");
    cJSON *len_j = cJSON_GetObjectItem(args, "length");
    if (!path) return strdup("ERROR: missing required arg 'path'");

    long long offset = cJSON_IsNumber(off_j) ? (long long)off_j->valuedouble : 0;
    long long length = cJSON_IsNumber(len_j) ? (long long)len_j->valuedouble : MAX_RANGE_BYTES;
    if (offset < 0) offset = 0;
    if (length <= 0 || length > MAX_RANGE_BYTES) length = MAX_RANGE_BYTES;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: open('%s'): %s", path, strerror(errno));
        free(path);
        return b.data;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: fstat('%s'): %s", path, strerror(errno));
        free(path);
        return b.data;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        free(path);
        return strdup("ERROR: not a regular file");
    }
    long long file_size = (long long)st.st_size;
    if (offset >= file_size) {
        close(fd);
        Buf b; buf_init(&b);
        buf_printf(&b,
            "READ_FILE_RANGE %s offset=%lld length=%lld file_size=%lld\n---\n(offset past end)\n",
            path, offset, length, file_size);
        free(path);
        return b.data;
    }
    long long avail = file_size - offset;
    if (length > avail) length = avail;

    /* mmap requires page-aligned offsets, so align down and adjust pointer */
    long page = sysconf(_SC_PAGESIZE);
    long long aligned_off = offset - (offset % page);
    size_t map_len = (size_t)(length + (offset - aligned_off));

    void *p = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, (off_t)aligned_off);
    close(fd);
    if (p == MAP_FAILED) {
        Buf b; buf_init(&b);
        buf_printf(&b, "ERROR: mmap: %s", strerror(errno));
        free(path);
        return b.data;
    }
    const char *slice = (const char *)p + (offset - aligned_off);

    Buf out; buf_init(&out);
    buf_printf(&out,
        "READ_FILE_RANGE %s offset=%lld length=%lld file_size=%lld\n---\n",
        path, offset, length, file_size);
    buf_append(&out, slice, (size_t)length);
    munmap(p, map_len);
    free(path);
    return out.data;
}

/* ---------- registration ---------- */

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

void tools_fs_register(cJSON *arr) {
    {
        const char *req[] = {"path", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "path", "string", "Path to inspect.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "stat",
            "Get file metadata (type, size, mode, uid/gid, mtime, atime, symlink target) without reading content. Use to decide if a file is worth reading.",
            p));
    }
    {
        cJSON *p = param_obj(NULL);
        add_prop(p, "path",        "string",  "Directory to list. Default '.'");
        add_prop(p, "show_hidden", "boolean", "Include dotfiles (default false).");
        add_prop(p, "max_entries", "integer", "Max entries to return (default 200, hard cap 1000).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "list_dir",
            "List a directory with each entry's type, size, mtime, name. Cheap; prefer this over recursive search when you just want one directory.",
            p));
    }
    {
        const char *req[] = {"path", "content", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "path",    "string",  "Destination path.");
        add_prop(p, "content", "string",  "File contents.");
        add_prop(p, "mode",    "integer", "POSIX mode bits (e.g. 0644).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "write_file",
            "Atomically write a file (mkstemp + rename). Replaces any existing file with no torn writes for concurrent readers.",
            p));
    }
    {
        const char *req[] = {"path", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "path",   "string",  "File to read from.");
        add_prop(p, "offset", "integer", "Byte offset to start at (default 0).");
        add_prop(p, "length", "integer", "Bytes to read (default 256KB, hard max 256KB).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "read_file_range",
            "Read a byte range from a file (mmap-backed). Use for huge files / logs where read_file would be wasteful.",
            p));
    }
}

char *tools_fs_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    (void)ctx;
    if (!name) return NULL;
    if (strcmp(name, "stat") == 0)            return tool_stat(args);
    if (strcmp(name, "list_dir") == 0)        return tool_list_dir(args);
    if (strcmp(name, "write_file") == 0)      return tool_write_file(args);
    if (strcmp(name, "read_file_range") == 0) return tool_read_file_range(args);
    return NULL;
}
