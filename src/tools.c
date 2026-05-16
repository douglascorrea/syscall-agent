#include "tools.h"
#include "tools_fs.h"
#include "tools_proc.h"
#include "tools_watch.h"
#include "tools_net.h"
#include "tools_meta.h"
#include "util.h"
#include "http.h"
#include "memory.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_FILE_READ_BYTES (256 * 1024)
#define MAX_FETCH_BYTES     (512 * 1024)
#define MAX_SEARCH_RESULTS  200

/* ---------- helpers ---------- */

static char *dup_or_default(cJSON *node, const char *def) {
    if (cJSON_IsString(node) && node->valuestring) return strdup(node->valuestring);
    return def ? strdup(def) : NULL;
}

static char *fmt_err(const char *fmt, const char *arg) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, fmt, arg ? arg : "");
    return b.data; /* caller frees */
}

/* ---------- read_file ---------- */

static char *tool_read_file(cJSON *args) {
    char *path = dup_or_default(cJSON_GetObjectItem(args, "path"), NULL);
    if (!path) return strdup("ERROR: missing required arg 'path'");

    FILE *f = fopen(path, "rb");
    if (!f) {
        char *e = fmt_err("ERROR: could not open '%s'", path);
        free(path);
        return e;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    size_t to_read = (size_t)sz;
    int truncated = 0;
    if (to_read > MAX_FILE_READ_BYTES) {
        to_read = MAX_FILE_READ_BYTES;
        truncated = 1;
    }
    char *buf = malloc(to_read + 1);
    if (!buf) {
        fclose(f);
        free(path);
        return strdup("ERROR: out of memory");
    }
    size_t n = fread(buf, 1, to_read, f);
    buf[n] = '\0';
    fclose(f);

    Buf out;
    buf_init(&out);
    buf_printf(&out, "FILE: %s (%zu bytes%s)\n---\n", path, n,
        truncated ? ", truncated" : "");
    buf_append(&out, buf, n);
    free(buf);
    free(path);
    return out.data;
}

/* ---------- search_files ---------- */

static int search_files_recurse(const char *root, const char *pattern,
                                Buf *out, int *count, int depth) {
    if (depth > 12) return 0;
    DIR *d = opendir(root);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' ||
            (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        /* skip common noise */
        if (strcmp(e->d_name, ".git") == 0 ||
            strcmp(e->d_name, "node_modules") == 0 ||
            strcmp(e->d_name, ".venv") == 0 ||
            strcmp(e->d_name, "venv") == 0 ||
            strcmp(e->d_name, "build") == 0 ||
            strcmp(e->d_name, "dist") == 0) continue;

        char path[4096];
        snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            search_files_recurse(path, pattern, out, count, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            int match = (fnmatch(pattern, e->d_name, 0) == 0) ||
                        (fnmatch(pattern, path, FNM_PATHNAME) == 0) ||
                        (strstr(e->d_name, pattern) != NULL);
            if (match) {
                if (*count < MAX_SEARCH_RESULTS) {
                    buf_printf(out, "%s\n", path);
                }
                (*count)++;
            }
        }
        if (*count >= MAX_SEARCH_RESULTS + 1) break;
    }
    closedir(d);
    return 0;
}

static char *tool_search_files(cJSON *args) {
    char *pattern = dup_or_default(cJSON_GetObjectItem(args, "pattern"), NULL);
    char *path    = dup_or_default(cJSON_GetObjectItem(args, "path"), ".");
    if (!pattern) {
        free(path);
        return strdup("ERROR: missing required arg 'pattern' (e.g. '*.c' or 'main')");
    }
    Buf out;
    buf_init(&out);
    buf_printf(&out, "SEARCH: pattern='%s' path='%s'\n---\n", pattern, path);
    int count = 0;
    if (search_files_recurse(path, pattern, &out, &count, 0) != 0) {
        buf_printf(&out, "ERROR: could not open directory '%s': %s\n",
            path, strerror(errno));
    } else if (count == 0) {
        buf_append_cstr(&out, "(no matches)\n");
    } else if (count > MAX_SEARCH_RESULTS) {
        buf_printf(&out, "... (%d total, showing first %d)\n",
            count, MAX_SEARCH_RESULTS);
    }
    free(pattern);
    free(path);
    return out.data;
}

/* ---------- fetch_url ---------- */

static char *tool_fetch_url(cJSON *args) {
    char *url = dup_or_default(cJSON_GetObjectItem(args, "url"), NULL);
    if (!url) return strdup("ERROR: missing required arg 'url'");
    HttpResponse r = http_get(url, NULL);
    Buf out;
    buf_init(&out);
    if (r.error) {
        buf_printf(&out, "ERROR: %s\n", r.error);
        http_response_free(&r);
        free(url);
        return out.data;
    }
    size_t shown = r.body_len;
    int trunc = 0;
    if (shown > MAX_FETCH_BYTES) { shown = MAX_FETCH_BYTES; trunc = 1; }
    buf_printf(&out, "GET %s -> %ld (%zu bytes%s)\n---\n",
        url, r.status, r.body_len, trunc ? ", truncated" : "");
    if (r.body) buf_append(&out, r.body, shown);
    http_response_free(&r);
    free(url);
    return out.data;
}

/* ---------- web_fetch (fetch + strip to text) ---------- */

static char *tool_web_fetch(cJSON *args) {
    char *url = dup_or_default(cJSON_GetObjectItem(args, "url"), NULL);
    if (!url) return strdup("ERROR: missing required arg 'url'");
    HttpResponse r = http_get(url, NULL);
    Buf out;
    buf_init(&out);
    if (r.error) {
        buf_printf(&out, "ERROR: %s\n", r.error);
        http_response_free(&r);
        free(url);
        return out.data;
    }
    Buf text;
    buf_init(&text);
    if (r.body) strip_html(r.body, &text);
    size_t shown = text.len;
    int trunc = 0;
    if (shown > MAX_FETCH_BYTES) { shown = MAX_FETCH_BYTES; trunc = 1; }
    buf_printf(&out, "WEB_FETCH %s -> %ld (%zu bytes text%s)\n---\n",
        url, r.status, text.len, trunc ? ", truncated" : "");
    if (text.data) buf_append(&out, text.data, shown);
    buf_free(&text);
    http_response_free(&r);
    free(url);
    return out.data;
}

/* ---------- search_web (DuckDuckGo HTML) ---------- */

/* Tiny extractor for DDG HTML: pull <a class="result__a" href="...">title</a>
 * and <a class="result__snippet">snippet</a>. Falls back to stripped text on failure. */
static void extract_ddg_results(const char *html, Buf *out, int max_results) {
    const char *p = html;
    int n = 0;
    while (n < max_results) {
        const char *a = strstr(p, "class=\"result__a\"");
        if (!a) break;
        const char *href = strstr(a, "href=\"");
        if (!href) break;
        href += 6;
        const char *href_end = strchr(href, '"');
        if (!href_end) break;
        const char *gt = strchr(href_end, '>');
        if (!gt) break;
        const char *close = strstr(gt, "</a>");
        if (!close) break;
        Buf title;
        buf_init(&title);
        {
            Buf raw;
            buf_init(&raw);
            buf_append(&raw, gt + 1, (size_t)(close - gt - 1));
            strip_html(raw.data ? raw.data : "", &title);
            buf_free(&raw);
        }

        Buf snippet;
        buf_init(&snippet);
        const char *snip = strstr(close, "class=\"result__snippet\"");
        const char *next = strstr(close, "class=\"result__a\"");
        if (snip && (!next || snip < next)) {
            const char *sgt = strchr(snip, '>');
            const char *sclose = sgt ? strstr(sgt, "</a>") : NULL;
            if (sgt && sclose) {
                Buf raw;
                buf_init(&raw);
                buf_append(&raw, sgt + 1, (size_t)(sclose - sgt - 1));
                strip_html(raw.data ? raw.data : "", &snippet);
                buf_free(&raw);
            }
        }

        Buf href_buf;
        buf_init(&href_buf);
        buf_append(&href_buf, href, (size_t)(href_end - href));

        buf_printf(out, "%d. %s\n   %s\n   %s\n\n",
            n + 1,
            title.data ? title.data : "(no title)",
            href_buf.data ? href_buf.data : "",
            snippet.data ? snippet.data : "");

        buf_free(&title);
        buf_free(&snippet);
        buf_free(&href_buf);

        p = close + 4;
        n++;
    }
    if (n == 0) {
        buf_append_cstr(out, "(no results parsed — DDG HTML may have changed)\n");
    }
}

/* Brave Search API client. Returns 1 if used (success or failure), 0 if no key. */
static int search_web_brave(const char *query, Buf *out) {
    const char *key = getenv("BRAVE_SEARCH_API_KEY");
    if (!key || !*key) return 0;

    char *enc = url_encode(query);
    Buf url;
    buf_init(&url);
    buf_printf(&url, "https://api.search.brave.com/res/v1/web/search?q=%s&count=10", enc);
    free(enc);

    Buf auth;
    buf_init(&auth);
    buf_printf(&auth, "X-Subscription-Token: %s", key);

    const char *headers[] = {
        auth.data,
        "Accept: application/json",
        NULL
    };
    HttpResponse r = http_get(url.data, headers);
    buf_free(&url);
    buf_free(&auth);

    if (r.error) {
        buf_printf(out, "ERROR (brave): %s\n", r.error);
        http_response_free(&r);
        return 1;
    }
    buf_printf(out, "SEARCH_WEB[brave] query=\"%s\" (HTTP %ld)\n---\n", query, r.status);
    if (r.status >= 400) {
        if (r.body) buf_append(out, r.body, r.body_len < 800 ? r.body_len : 800);
        http_response_free(&r);
        return 1;
    }
    cJSON *root = r.body ? cJSON_Parse(r.body) : NULL;
    if (!root) {
        buf_append_cstr(out, "(could not parse JSON)\n");
        http_response_free(&r);
        return 1;
    }
    cJSON *web = cJSON_GetObjectItem(root, "web");
    cJSON *results = web ? cJSON_GetObjectItem(web, "results") : NULL;
    int n = cJSON_IsArray(results) ? cJSON_GetArraySize(results) : 0;
    if (n == 0) buf_append_cstr(out, "(no results)\n");
    for (int i = 0; i < n && i < 10; i++) {
        cJSON *it = cJSON_GetArrayItem(results, i);
        const char *t = NULL, *u = NULL, *d = NULL;
        cJSON *tj = cJSON_GetObjectItem(it, "title");
        cJSON *uj = cJSON_GetObjectItem(it, "url");
        cJSON *dj = cJSON_GetObjectItem(it, "description");
        if (cJSON_IsString(tj)) t = tj->valuestring;
        if (cJSON_IsString(uj)) u = uj->valuestring;
        if (cJSON_IsString(dj)) d = dj->valuestring;
        Buf desc;
        buf_init(&desc);
        if (d) strip_html(d, &desc);
        buf_printf(out, "%d. %s\n   %s\n   %s\n\n",
            i + 1, t ? t : "(no title)", u ? u : "",
            desc.data ? desc.data : "");
        buf_free(&desc);
    }
    cJSON_Delete(root);
    http_response_free(&r);
    return 1;
}

static char *tool_search_web(cJSON *args) {
    char *query = dup_or_default(cJSON_GetObjectItem(args, "query"), NULL);
    if (!query) return strdup("ERROR: missing required arg 'query'");

    Buf out;
    buf_init(&out);

    /* Prefer Brave Search if an API key is configured. */
    if (search_web_brave(query, &out)) {
        free(query);
        return out.data;
    }

    /* Fallback: DuckDuckGo HTML endpoint. Can rate-limit per IP. */
    char *enc = url_encode(query);
    Buf body;
    buf_init(&body);
    buf_printf(&body, "q=%s", enc);
    free(enc);

    const char *headers[] = {
        "User-Agent: Mozilla/5.0",
        "Accept: text/html",
        NULL
    };
    HttpResponse r = http_post_form(
        "https://html.duckduckgo.com/html/", body.data, headers);
    buf_free(&body);

    if (r.error) {
        buf_printf(&out, "ERROR: %s\n", r.error);
        http_response_free(&r);
        free(query);
        return out.data;
    }
    buf_printf(&out, "SEARCH_WEB[ddg] query=\"%s\" (HTTP %ld)\n---\n", query, r.status);
    if (r.status == 202) {
        buf_append_cstr(&out,
            "(DuckDuckGo returned an anti-bot interstitial — likely rate-limited. "
            "Try web_fetch on a known URL instead, or set BRAVE_SEARCH_API_KEY.)\n");
    } else if (r.body) {
        extract_ddg_results(r.body, &out, 10);
    }
    http_response_free(&r);
    free(query);
    return out.data;
}

/* ---------- save_memory ---------- */

static char *tool_save_memory(ToolCtx *ctx, cJSON *args) {
    char *content = dup_or_default(cJSON_GetObjectItem(args, "content"), NULL);
    char *topic   = dup_or_default(cJSON_GetObjectItem(args, "topic"), "note");
    if (!content) {
        free(topic);
        return strdup("ERROR: missing required arg 'content'");
    }
    int rc = memory_append(ctx->memory_path, topic, content);
    Buf out;
    buf_init(&out);
    if (rc == 0) {
        buf_printf(&out, "OK: appended memory to %s under '%s'\n",
            ctx->memory_path, topic);
    } else {
        buf_printf(&out, "ERROR: could not write to %s\n", ctx->memory_path);
    }
    free(content);
    free(topic);
    return out.data;
}

/* ---------- registry ---------- */

static cJSON *make_function_tool(const char *name, const char *desc,
                                 cJSON *params) {
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

static void add_string_prop(cJSON *params, const char *name, const char *desc) {
    cJSON *props = cJSON_GetObjectItem(params, "properties");
    cJSON *p = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
}

static char *tool_list_tools(const ToolCtx *ctx) {
    cJSON *tools = tools_describe(ctx);
    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "LIST_TOOLS\n---\n");
    int n = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *fn = tool ? cJSON_GetObjectItem(tool, "function") : NULL;
        cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
        cJSON *desc = fn ? cJSON_GetObjectItem(fn, "description") : NULL;
        if (cJSON_IsString(name) && name->valuestring) {
            buf_printf(&out, "%s\t%s\n",
                name->valuestring,
                cJSON_IsString(desc) && desc->valuestring ? desc->valuestring : "");
        }
    }
    cJSON_Delete(tools);
    return out.data;
}

cJSON *tools_describe(const ToolCtx *ctx) {
    cJSON *arr = cJSON_CreateArray();

    {   /* list_tools */
        cJSON *p = param_obj(NULL);
        cJSON_AddItemToArray(arr, make_function_tool(
            "list_tools",
            "List every tool currently visible to the model with one-line descriptions.",
            p));
    }
    {   /* read_file */
        const char *req[] = {"path", NULL};
        cJSON *p = param_obj(req);
        add_string_prop(p, "path", "Path to a local file. Relative or absolute.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "read_file",
            "Read the contents of a local file. Returns up to 256KB of text.",
            p));
    }
    {   /* search_files */
        const char *req[] = {"pattern", NULL};
        cJSON *p = param_obj(req);
        add_string_prop(p, "pattern", "Glob (e.g. '*.c') or substring matched against filenames.");
        add_string_prop(p, "path",    "Root directory to search. Defaults to '.'");
        cJSON_AddItemToArray(arr, make_function_tool(
            "search_files",
            "Recursively search for files by name pattern. Returns matching paths.",
            p));
    }
    {   /* search_web */
        const char *req[] = {"query", NULL};
        cJSON *p = param_obj(req);
        add_string_prop(p, "query", "Web search query string.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "search_web",
            "Search the web (via DuckDuckGo) and return the top results with titles, URLs, snippets.",
            p));
    }
    {   /* fetch_url */
        const char *req[] = {"url", NULL};
        cJSON *p = param_obj(req);
        add_string_prop(p, "url", "HTTP/HTTPS URL to fetch.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "fetch_url",
            "HTTP GET a URL and return the raw response body (up to 512KB).",
            p));
    }
    {   /* web_fetch */
        const char *req[] = {"url", NULL};
        cJSON *p = param_obj(req);
        add_string_prop(p, "url", "HTTP/HTTPS URL of an HTML page to fetch.");
        cJSON_AddItemToArray(arr, make_function_tool(
            "web_fetch",
            "Fetch a web page and return its text content (HTML stripped). Prefer this for readable web content.",
            p));
    }
    {   /* save_memory */
        const char *req[] = {"content", NULL};
        cJSON *p = param_obj(req);
        add_string_prop(p, "content", "The memory text to persist. Be concise and self-contained.");
        add_string_prop(p, "topic",   "Short label categorizing this memory (optional).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "save_memory",
            "Persist a note to MEMORY.md. Call ONLY when something is worth remembering across future runs (user preferences, durable facts, important context). Do NOT save ephemeral details.",
            p));
    }

    tools_fs_register(arr);
    tools_meta_register(arr, ctx ? ctx->allow_exec : 0);
    tools_proc_register(arr, ctx ? ctx->allow_exec : 0);
    tools_watch_register(arr);
    tools_net_register(arr);

    return arr;
}

char *tools_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    if (!name) return strdup("ERROR: missing tool name");
    if (strcmp(name, "list_tools") == 0)   return tool_list_tools(ctx);
    if (strcmp(name, "read_file") == 0)    return tool_read_file(args);
    if (strcmp(name, "search_files") == 0) return tool_search_files(args);
    if (strcmp(name, "search_web") == 0)   return tool_search_web(args);
    if (strcmp(name, "fetch_url") == 0)    return tool_fetch_url(args);
    if (strcmp(name, "web_fetch") == 0)    return tool_web_fetch(args);
    if (strcmp(name, "save_memory") == 0)  return tool_save_memory(ctx, args);

    char *r;
    if ((r = tools_fs_dispatch(ctx, name, args)))    return r;
    if ((r = tools_meta_dispatch(ctx, name, args)))  return r;
    if ((r = tools_proc_dispatch(ctx, name, args)))  return r;
    if ((r = tools_watch_dispatch(ctx, name, args))) return r;
    if ((r = tools_net_dispatch(ctx, name, args)))   return r;

    Buf b;
    buf_init(&b);
    buf_printf(&b, "ERROR: unknown tool '%s'", name);
    return b.data;
}
