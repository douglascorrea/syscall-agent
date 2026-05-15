#include "tools_watch.h"
#include "os_compat.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define MAX_WATCH_MS  120000

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

static char *tool_watch_path(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    cJSON *t_j = cJSON_GetObjectItem(args, "timeout_ms");
    int timeout_ms = cJSON_IsNumber(t_j) ? (int)t_j->valueint : 30000;
    if (timeout_ms <= 0) timeout_ms = 1000;
    if (timeout_ms > MAX_WATCH_MS) timeout_ms = MAX_WATCH_MS;
    if (!path) return strdup("ERROR: missing required arg 'path'");

    WatchEvent ev = {0};
    int rc = os_watch_path(path, timeout_ms, &ev);
    Buf out; buf_init(&out);
    if (rc < 0) {
        buf_printf(&out, "ERROR: could not watch '%s'\n", path);
    } else if (rc == 1) {
        buf_printf(&out, "WATCH_PATH path=%s timeout_ms=%d -> TIMEOUT\n",
            path, timeout_ms);
    } else {
        buf_printf(&out, "WATCH_PATH path=%s -> %s\n", ev.path, ev.kind);
    }
    free(path);
    return out.data;
}

void tools_watch_register(cJSON *arr) {
    const char *req[] = {"path", NULL};
    cJSON *p = param_obj(req);
    add_prop(p, "path",       "string",  "File or directory to watch.");
    add_prop(p, "timeout_ms", "integer", "Max wait time in ms (default 30000, hard cap 120000).");
    cJSON_AddItemToArray(arr, make_function_tool(
        "watch_path",
        "Block until the given path (or its parent if it's a directory) is modified, created, deleted, or renamed. Returns the first event or 'timeout'. Backed by kqueue (macOS) / inotify (Linux).",
        p));
}

char *tools_watch_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    (void)ctx;
    if (!name) return NULL;
    if (strcmp(name, "watch_path") == 0) return tool_watch_path(args);
    return NULL;
}
