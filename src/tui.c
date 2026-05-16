#include "tui.h"
#include "openrouter_models.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

const TuiModelChoice TUI_MODELS[] = {
    { "openai/gpt-4o-mini", "GPT-4o mini" },
    { "anthropic/claude-3.5-sonnet", "Claude 3.5 Sonnet" },
    { "google/gemini-2.5-flash", "Gemini 2.5 Flash" },
    { "openai/gpt-4.1-mini", "GPT-4.1 mini" },
    { "deepseek/deepseek-chat-v3-0324", "DeepSeek Chat v3" },
    { "qwen/qwen3-coder", "Qwen3 Coder" },
};

const size_t TUI_MODEL_COUNT = sizeof(TUI_MODELS) / sizeof(TUI_MODELS[0]);

typedef enum {
    ENTRY_SYSTEM,
    ENTRY_USER,
    ENTRY_ASSISTANT,
    ENTRY_TOOL,
    ENTRY_REASONING,
    ENTRY_ERROR
} EntryKind;

typedef struct {
    EntryKind kind;
    char *title;
    char *text;
} TuiEntry;

typedef struct {
    char *text;
    EntryKind kind;
} RenderLine;

typedef struct {
    RenderLine *items;
    size_t len;
    size_t cap;
} RenderLines;

typedef struct {
    AgentConfig *cfg;
    TuiEntry *entries;
    size_t len;
    size_t cap;
    char input[8192];
    size_t input_len;
    int width;
    int height;
    int running;
    int should_exit;
    char status[256];
    struct termios old_termios;
    int raw_enabled;
    OpenRouterModelCatalog model_catalog;
    int model_catalog_loaded;
    int model_picker_open;
    char model_query[256];
    size_t model_query_len;
    size_t *model_matches;
    size_t model_match_len;
    size_t model_match_cap;
    size_t model_selected;
    size_t model_scroll;
    char *owned_model_id;
    AgentConversation *conversation;
} TuiApp;

typedef enum {
    TUI_KEY_NONE,
    TUI_KEY_ESC,
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_PAGE_UP,
    TUI_KEY_PAGE_DOWN,
    TUI_KEY_HOME,
    TUI_KEY_END
} TuiKey;

static volatile sig_atomic_t g_resize_pending = 0;

static void on_sigwinch(int sig) {
    (void)sig;
    g_resize_pending = 1;
}

static char *lla_strndup(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static const char *skip_spaces(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void trim_span(const char **start, size_t *len) {
    const char *s = *start;
    size_t n = *len;
    while (n > 0 && isspace((unsigned char)s[0])) {
        s++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    *start = s;
    *len = n;
}

static int parse_positive_int(const char *s, int *out) {
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *skip_spaces(end) != '\0' || v <= 0 || v > 100000) return 0;
    *out = (int)v;
    return 1;
}

const char *tui_verbose_name(int mode) {
    switch (mode) {
        case TUI_VERBOSE_NORMAL: return "normal";
        case TUI_VERBOSE_TOOLS: return "tools";
        case TUI_VERBOSE_REASONING: return "reasoning";
        case TUI_VERBOSE_ALL: return "all";
        default: return "custom";
    }
}

int tui_parse_command(const char *line, TuiCommand *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->type = TUI_CMD_NONE;
    out->model_index = -1;

    const char *p = skip_spaces(line ? line : "");
    if (*p != '/') return 0;
    p++;

    const char *cmd = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    size_t cmd_len = (size_t)(p - cmd);
    const char *arg = skip_spaces(p);

    if (cmd_len == 3 && strncmp(cmd, "new", 3) == 0 && *arg == '\0') {
        out->type = TUI_CMD_NEW;
        return 1;
    }
    if (((cmd_len == 4 && strncmp(cmd, "exit", 4) == 0) ||
         (cmd_len == 4 && strncmp(cmd, "quit", 4) == 0)) && *arg == '\0') {
        out->type = TUI_CMD_EXIT;
        return 1;
    }
    if ((cmd_len == 4 && strncmp(cmd, "help", 4) == 0) && *arg == '\0') {
        out->type = TUI_CMD_HELP;
        return 1;
    }
    if ((cmd_len == 6 && strncmp(cmd, "models", 6) == 0) && *arg == '\0') {
        out->type = TUI_CMD_MODELS;
        return 1;
    }
    if ((cmd_len == 5 && strncmp(cmd, "tools", 5) == 0) && *arg == '\0') {
        out->type = TUI_CMD_TOOLS;
        return 1;
    }
    if ((cmd_len == 6 && strncmp(cmd, "skills", 6) == 0) && *arg == '\0') {
        out->type = TUI_CMD_SKILLS;
        return 1;
    }
    if ((cmd_len == 4 && strncmp(cmd, "auth", 4) == 0) && *arg == '\0') {
        out->type = TUI_CMD_AUTH;
        return 1;
    }
    if ((cmd_len == 7 && strncmp(cmd, "sysinfo", 7) == 0) && *arg == '\0') {
        out->type = TUI_CMD_SYSINFO;
        return 1;
    }
    if (cmd_len == 5 && strncmp(cmd, "model", 5) == 0) {
        if (*arg == '\0') {
            out->type = TUI_CMD_MODELS;
            return 1;
        }
        int n = 0;
        if (parse_positive_int(arg, &n)) {
            if (n <= (int)TUI_MODEL_COUNT) {
                out->type = TUI_CMD_MODEL;
                out->model_index = n - 1;
                out->model_id = TUI_MODELS[n - 1].id;
                return 1;
            }
            out->type = TUI_CMD_UNKNOWN;
            out->error = "model index is out of range";
            return 1;
        }
        for (size_t i = 0; i < TUI_MODEL_COUNT; i++) {
            if (strcmp(arg, TUI_MODELS[i].id) == 0 ||
                strcasecmp(arg, TUI_MODELS[i].label) == 0) {
                out->type = TUI_CMD_MODEL;
                out->model_index = (int)i;
                out->model_id = TUI_MODELS[i].id;
                return 1;
            }
        }
        if (strchr(arg, '/')) {
            out->type = TUI_CMD_MODEL;
            out->model_index = -1;
            out->model_id = arg;
            return 1;
        }
        out->type = TUI_CMD_UNKNOWN;
        out->error = "unknown model; use /model to open the OpenRouter picker";
        return 1;
    }
    if (cmd_len == 7 && strncmp(cmd, "verbose", 7) == 0) {
        if (strcmp(arg, "normal") == 0) {
            out->type = TUI_CMD_VERBOSE;
            out->verbose_mode = TUI_VERBOSE_NORMAL;
            return 1;
        }
        if (strcmp(arg, "tools") == 0) {
            out->type = TUI_CMD_VERBOSE;
            out->verbose_mode = TUI_VERBOSE_TOOLS;
            return 1;
        }
        if (strcmp(arg, "reasoning") == 0 || strcmp(arg, "reasioning") == 0) {
            out->type = TUI_CMD_VERBOSE;
            out->verbose_mode = TUI_VERBOSE_REASONING;
            return 1;
        }
        if (strcmp(arg, "all") == 0) {
            out->type = TUI_CMD_VERBOSE;
            out->verbose_mode = TUI_VERBOSE_ALL;
            return 1;
        }
        out->type = TUI_CMD_UNKNOWN;
        out->error = "usage: /verbose normal|tools|reasoning|all";
        return 1;
    }

    out->type = TUI_CMD_UNKNOWN;
    out->error = "unknown command; try /help";
    return 1;
}

static void push_line(char ***lines, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len >= *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        char **items = realloc(*lines, nc * sizeof(char *));
        if (!items) return;
        *lines = items;
        *cap = nc;
    }
    (*lines)[*len] = lla_strndup(s, n);
    if (!(*lines)[*len]) return;
    (*len)++;
}

size_t tui_wrap_plain(const char *text, int width, char ***out_lines) {
    char **lines = NULL;
    size_t len = 0, cap = 0;
    int w = width > 0 ? width : 1;
    const char *p = text ? text : "";

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t para_len = nl ? (size_t)(nl - p) : strlen(p);
        const char *para = p;
        trim_span(&para, &para_len);

        if (para_len == 0) {
            push_line(&lines, &len, &cap, "", 0);
        }

        while (para_len > 0) {
            if ((int)para_len <= w) {
                push_line(&lines, &len, &cap, para, para_len);
                break;
            }

            size_t take = (size_t)w;
            size_t split = 0;
            for (size_t i = 0; i < (size_t)w; i++) {
                if (isspace((unsigned char)para[i])) split = i;
            }
            if (split > 0) take = split;

            const char *seg = para;
            size_t seg_len = take;
            trim_span(&seg, &seg_len);
            push_line(&lines, &len, &cap, seg, seg_len);

            para += take;
            para_len -= take;
            while (para_len > 0 && isspace((unsigned char)*para)) {
                para++;
                para_len--;
            }
        }

        if (!nl) break;
        p = nl + 1;
    }

    if (len == 0) push_line(&lines, &len, &cap, "", 0);
    *out_lines = lines;
    return len;
}

void tui_free_lines(char **lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

static void render_lines_free(RenderLines *rl) {
    for (size_t i = 0; i < rl->len; i++) free(rl->items[i].text);
    free(rl->items);
    rl->items = NULL;
    rl->len = rl->cap = 0;
}

static void render_lines_add(RenderLines *rl, EntryKind kind, const char *text) {
    if (rl->len >= rl->cap) {
        size_t nc = rl->cap ? rl->cap * 2 : 64;
        RenderLine *items = realloc(rl->items, nc * sizeof(RenderLine));
        if (!items) return;
        rl->items = items;
        rl->cap = nc;
    }
    rl->items[rl->len].kind = kind;
    rl->items[rl->len].text = strdup(text ? text : "");
    if (!rl->items[rl->len].text) return;
    rl->len++;
}

static const char *fg_for_kind(EntryKind kind) {
    switch (kind) {
        case ENTRY_SYSTEM: return "\x1b[38;2;102;102;102m";
        case ENTRY_TOOL: return "\x1b[38;2;128;128;128m";
        case ENTRY_REASONING: return "\x1b[38;2;128;128;128m";
        case ENTRY_ERROR: return "\x1b[38;2;204;102;102m";
        default: return "";
    }
}

static const char *bg_for_kind(EntryKind kind) {
    switch (kind) {
        case ENTRY_USER: return "\x1b[48;2;52;53;65m";
        case ENTRY_TOOL: return "\x1b[48;2;40;50;40m";
        case ENTRY_ERROR: return "\x1b[48;2;60;40;40m";
        default: return "";
    }
}

static void print_repeated(const char *s, int count) {
    for (int i = 0; i < count; i++) fputs(s, stdout);
}

static void print_padded(const char *text, int width, EntryKind kind) {
    int w = width > 0 ? width : 1;
    const char *fg = fg_for_kind(kind);
    const char *bg = bg_for_kind(kind);
    fputs(bg, stdout);
    fputs(fg, stdout);

    const char *s = text ? text : "";
    int n = 0;
    while (s[n] && n < w) n++;
    fwrite(s, 1, (size_t)n, stdout);
    for (int i = n; i < w; i++) fputc(' ', stdout);
    fputs("\x1b[0m", stdout);
}

static void get_terminal_size(TuiApp *app) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        app->width = ws.ws_col;
        app->height = ws.ws_row;
    } else {
        app->width = 80;
        app->height = 24;
    }
    if (app->width < 1) app->width = 1;
    if (app->height < 1) app->height = 1;
}

static void add_entry(TuiApp *app, EntryKind kind, const char *title, const char *text) {
    if (app->len >= app->cap) {
        size_t nc = app->cap ? app->cap * 2 : 32;
        TuiEntry *items = realloc(app->entries, nc * sizeof(TuiEntry));
        if (!items) return;
        app->entries = items;
        app->cap = nc;
    }
    app->entries[app->len].kind = kind;
    app->entries[app->len].title = strdup(title ? title : "");
    app->entries[app->len].text = strdup(text ? text : "");
    if (!app->entries[app->len].title || !app->entries[app->len].text) {
        free(app->entries[app->len].title);
        free(app->entries[app->len].text);
        app->entries[app->len].title = NULL;
        app->entries[app->len].text = NULL;
        return;
    }
    app->len++;
}

static void append_entry_delta(TuiApp *app, EntryKind kind, const char *title, const char *delta) {
    const char *safe_title = title ? title : "";
    const char *safe_delta = delta ? delta : "";
    if (app->len > 0) {
        TuiEntry *last = &app->entries[app->len - 1];
        if (last->kind == kind && last->title && strcmp(last->title, safe_title) == 0) {
            size_t old_len = last->text ? strlen(last->text) : 0;
            size_t add_len = strlen(safe_delta);
            char *next = malloc(old_len + add_len + 1);
            if (!next) return;
            if (old_len) memcpy(next, last->text, old_len);
            memcpy(next + old_len, safe_delta, add_len + 1);
            free(last->text);
            last->text = next;
            return;
        }
    }
    add_entry(app, kind, safe_title, safe_delta);
}

static void clear_entries(TuiApp *app) {
    for (size_t i = 0; i < app->len; i++) {
        free(app->entries[i].title);
        free(app->entries[i].text);
    }
    app->len = 0;
}

static void free_entries(TuiApp *app) {
    clear_entries(app);
    free(app->entries);
    app->entries = NULL;
    app->cap = 0;
}

static int model_match_append(TuiApp *app, size_t index) {
    if (app->model_match_len >= app->model_match_cap) {
        size_t nc = app->model_match_cap ? app->model_match_cap * 2 : 128;
        size_t *items = realloc(app->model_matches, nc * sizeof(size_t));
        if (!items) return -1;
        app->model_matches = items;
        app->model_match_cap = nc;
    }
    app->model_matches[app->model_match_len++] = index;
    return 0;
}

static void model_picker_rebuild_matches(TuiApp *app) {
    app->model_match_len = 0;
    for (size_t i = 0; i < app->model_catalog.len; i++) {
        if (openrouter_model_matches(&app->model_catalog.items[i], app->model_query)) {
            if (model_match_append(app, i) != 0) break;
        }
    }
    if (app->model_match_len == 0) {
        app->model_selected = 0;
        app->model_scroll = 0;
        return;
    }
    if (app->model_selected >= app->model_match_len) {
        app->model_selected = app->model_match_len - 1;
    }
    if (app->model_scroll >= app->model_match_len) app->model_scroll = 0;
}

static void model_picker_select_current_model(TuiApp *app) {
    const char *current = app->cfg->model;
    if (!current || !*current) return;
    for (size_t i = 0; i < app->model_match_len; i++) {
        size_t model_index = app->model_matches[i];
        if (strcmp(app->model_catalog.items[model_index].id, current) == 0) {
            app->model_selected = i;
            return;
        }
    }
}

static void model_picker_ensure_visible(TuiApp *app, int visible_rows) {
    if (visible_rows < 1) visible_rows = 1;
    if (app->model_match_len == 0) {
        app->model_scroll = 0;
        return;
    }
    if (app->model_selected >= app->model_match_len) {
        app->model_selected = app->model_match_len - 1;
    }
    if (app->model_selected < app->model_scroll) {
        app->model_scroll = app->model_selected;
    }
    size_t visible = (size_t)visible_rows;
    if (app->model_selected >= app->model_scroll + visible) {
        app->model_scroll = app->model_selected - visible + 1;
    }
}

static void set_current_model(TuiApp *app, const char *model_id) {
    if (!model_id || !*model_id) return;
    char *copy = strdup(model_id);
    if (!copy) {
        add_entry(app, ENTRY_ERROR, "model", "out of memory while setting model");
        return;
    }
    free(app->owned_model_id);
    app->owned_model_id = copy;
    app->cfg->model = app->owned_model_id;

    char msg[512];
    snprintf(msg, sizeof msg, "model set to %s", app->cfg->model);
    add_entry(app, ENTRY_SYSTEM, "model", msg);
    snprintf(app->status, sizeof app->status, "%s", msg);
}

static int load_openrouter_models(TuiApp *app, int force) {
    if (app->model_catalog_loaded && !force) return 0;
    snprintf(app->status, sizeof app->status, "loading OpenRouter models...");

    char err[256];
    if (openrouter_models_fetch(app->cfg->api_key, &app->model_catalog, err, sizeof err) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", err[0] ? err : "could not load OpenRouter models");
        add_entry(app, ENTRY_ERROR, "models", msg);
        snprintf(app->status, sizeof app->status, "model catalog load failed");
        return -1;
    }
    app->model_catalog_loaded = 1;
    app->model_query[0] = '\0';
    app->model_query_len = 0;
    app->model_selected = 0;
    app->model_scroll = 0;
    model_picker_rebuild_matches(app);
    model_picker_select_current_model(app);
    snprintf(app->status, sizeof app->status, "loaded %zu OpenRouter models", app->model_catalog.len);
    return 0;
}

static void open_model_picker(TuiApp *app) {
    if (load_openrouter_models(app, 0) != 0) return;
    app->model_picker_open = 1;
    app->model_query[0] = '\0';
    app->model_query_len = 0;
    app->model_selected = 0;
    app->model_scroll = 0;
    model_picker_rebuild_matches(app);
    model_picker_select_current_model(app);
    snprintf(app->status, sizeof app->status, "model picker open");
}

static void close_model_picker(TuiApp *app, const char *status) {
    app->model_picker_open = 0;
    app->model_query[0] = '\0';
    app->model_query_len = 0;
    app->model_selected = 0;
    app->model_scroll = 0;
    snprintf(app->status, sizeof app->status, "%s", status ? status : "ready");
}

static void model_picker_move(TuiApp *app, long delta) {
    if (app->model_match_len == 0) return;
    long next = (long)app->model_selected + delta;
    if (next < 0) next = 0;
    if (next >= (long)app->model_match_len) next = (long)app->model_match_len - 1;
    app->model_selected = (size_t)next;
}

static void model_picker_select(TuiApp *app) {
    if (app->model_match_len == 0) {
        snprintf(app->status, sizeof app->status, "no matching model");
        return;
    }
    size_t model_index = app->model_matches[app->model_selected];
    const char *id = app->model_catalog.items[model_index].id;
    set_current_model(app, id);
    app->model_picker_open = 0;
}

static void add_wrapped(RenderLines *rl, EntryKind kind, const char *prefix,
                        const char *text, int width) {
    int content_width = width > 4 ? width - 4 : 1;
    char **lines = NULL;
    size_t count = tui_wrap_plain(text, content_width, &lines);
    for (size_t i = 0; i < count; i++) {
        Buf b;
        buf_init(&b);
        buf_append_cstr(&b, prefix ? prefix : "");
        buf_append_cstr(&b, lines[i]);
        render_lines_add(rl, kind, b.data ? b.data : "");
        buf_free(&b);
    }
    tui_free_lines(lines, count);
}

static void build_render_lines(TuiApp *app, RenderLines *rl) {
    for (size_t i = 0; i < app->len; i++) {
        TuiEntry *e = &app->entries[i];
        if (e->kind == ENTRY_USER) {
            render_lines_add(rl, ENTRY_USER, "");
            add_wrapped(rl, ENTRY_USER, "  ", e->text, app->width);
            render_lines_add(rl, ENTRY_USER, "");
        } else {
            Buf label;
            buf_init(&label);
            if (e->title && *e->title) {
                buf_printf(&label, "%s", e->title);
                render_lines_add(rl, e->kind, label.data ? label.data : "");
            }
            buf_free(&label);
            add_wrapped(rl, e->kind, "  ", e->text, app->width);
        }
        render_lines_add(rl, ENTRY_SYSTEM, "");
    }
}

static void render_model_picker(TuiApp *app, int body_height) {
    int printed = 0;
    int list_rows = body_height - 4;
    if (list_rows < 1) list_rows = 1;
    model_picker_ensure_visible(app, list_rows);

    char line[2048];
    snprintf(line, sizeof line, " OpenRouter model picker  models:%zu  matches:%zu",
        app->model_catalog.len, app->model_match_len);
    print_padded(line, app->width, ENTRY_SYSTEM);
    printf("\r\n");
    printed++;

    snprintf(line, sizeof line, " search: %s", app->model_query);
    print_padded(line, app->width, ENTRY_ASSISTANT);
    printf("\r\n");
    printed++;

    if (app->model_match_len == 0) {
        print_padded(" no matching models", app->width, ENTRY_ERROR);
        printf("\r\n");
        printed++;
        for (int i = 1; i < list_rows; i++) {
            print_padded("", app->width, ENTRY_ASSISTANT);
            printf("\r\n");
            printed++;
        }
    } else {
        for (int row = 0; row < list_rows; row++) {
            size_t match_pos = app->model_scroll + (size_t)row;
            if (match_pos < app->model_match_len) {
                size_t model_index = app->model_matches[match_pos];
                OpenRouterModel *m = &app->model_catalog.items[model_index];
                const char *mark = match_pos == app->model_selected ? ">" : " ";
                if (m->context_length > 0) {
                    snprintf(line, sizeof line, "%s %-42s %-28s ctx:%d in:%s out:%s",
                        mark,
                        m->id ? m->id : "",
                        m->name ? m->name : "",
                        m->context_length,
                        m->prompt_price && *m->prompt_price ? m->prompt_price : "0",
                        m->completion_price && *m->completion_price ? m->completion_price : "0");
                } else {
                    snprintf(line, sizeof line, "%s %-42s %-28s in:%s out:%s",
                        mark,
                        m->id ? m->id : "",
                        m->name ? m->name : "",
                        m->prompt_price && *m->prompt_price ? m->prompt_price : "0",
                        m->completion_price && *m->completion_price ? m->completion_price : "0");
                }
                print_padded(line, app->width,
                    match_pos == app->model_selected ? ENTRY_USER : ENTRY_ASSISTANT);
            } else {
                print_padded("", app->width, ENTRY_ASSISTANT);
            }
            printf("\r\n");
            printed++;
        }
    }

    snprintf(line, sizeof line,
        " Up/Down move  PgUp/PgDn jump  type to search  Enter select  Esc close  Ctrl-R refresh");
    print_padded(line, app->width, ENTRY_SYSTEM);
    printf("\r\n");
    printed++;

    while (printed < body_height) {
        print_padded("", app->width, ENTRY_ASSISTANT);
        printf("\r\n");
        printed++;
    }
}

static void render_app(TuiApp *app) {
    get_terminal_size(app);
    g_resize_pending = 0;

    printf("\x1b[H\x1b[2J");

    if (app->height <= 3) {
        char tiny[512];
        snprintf(tiny, sizeof tiny, "lla %s %s",
            app->cfg->model ? app->cfg->model : "no-model",
            app->running ? "working" : "");
        print_padded(tiny, app->width, ENTRY_SYSTEM);
        if (app->height > 1) {
            printf("\r\n");
            print_padded(app->input, app->width, ENTRY_USER);
        }
        fflush(stdout);
        return;
    }

    printf("\x1b[38;2;0;215;255m");
    print_repeated("─", app->width);
    printf("\x1b[0m\r\n");

    char header[1024];
    snprintf(header, sizeof header, " syscall-agent  %s  verbose:%s%s",
        app->cfg->model ? app->cfg->model : "no-model",
        tui_verbose_name(app->cfg->verbose),
        app->running ? "  working" : "");
    print_padded(header, app->width, ENTRY_ASSISTANT);
    printf("\r\n");

    int footer_lines = 3;
    int body_height = app->height - 2 - footer_lines;
    if (body_height < 1) body_height = 1;

    if (app->model_picker_open) {
        render_model_picker(app, body_height);
    } else {
        RenderLines rl = {0};
        build_render_lines(app, &rl);
        size_t start = rl.len > (size_t)body_height ? rl.len - (size_t)body_height : 0;
        int printed = 0;
        for (size_t i = start; i < rl.len && printed < body_height; i++, printed++) {
            print_padded(rl.items[i].text, app->width, rl.items[i].kind);
            printf("\r\n");
        }
        while (printed < body_height) {
            print_padded("", app->width, ENTRY_ASSISTANT);
            printf("\r\n");
            printed++;
        }
        render_lines_free(&rl);
    }

    printf("\x1b[38;2;95;135;255m");
    print_repeated("─", app->width);
    printf("\x1b[0m\r\n");

    char footer[1024];
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    snprintf(footer, sizeof footer, " %s  %s",
        app->status[0] ? app->status : cwd,
        app->running ? "running" : "ready");
    print_padded(footer, app->width, ENTRY_SYSTEM);
    printf("\r\n");

    char input_line[9000];
    if (app->model_picker_open) {
        snprintf(input_line, sizeof input_line, "model search> %s", app->model_query);
    } else {
        snprintf(input_line, sizeof input_line, "› %s", app->input);
    }
    print_padded(input_line, app->width, ENTRY_ASSISTANT);
    fflush(stdout);
}

static int enable_raw(TuiApp *app) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "ERROR: --tui requires an interactive terminal\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &app->old_termios) != 0) {
        perror("tcgetattr");
        return -1;
    }
    struct termios raw = app->old_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        perror("tcsetattr");
        return -1;
    }
    app->raw_enabled = 1;
    printf("\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J");
    fflush(stdout);
    return 0;
}

static void disable_raw(TuiApp *app) {
    if (app->raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &app->old_termios);
        app->raw_enabled = 0;
    }
    printf("\x1b[?25h\x1b[?1049l");
    fflush(stdout);
}

static void add_welcome(TuiApp *app) {
    add_entry(app, ENTRY_SYSTEM, "pi-style tui",
        "Type a prompt and press Enter. Commands: /model, /verbose normal|tools|reasoning|all, /new, /exit.");
}

static char *tools_text(const TuiApp *app) {
    Buf b;
    buf_init(&b);
    buf_append_cstr(&b,
        "Always available:\n"
        "  list_tools, read_file, search_files, search_web, fetch_url, web_fetch,\n"
        "  save_memory, stat, list_dir, write_file, read_file_range,\n"
        "  auth_status, system_info, disk_usage, env_get, which, file_digest,\n"
        "  grep_text, list_skills, read_skill, dns_lookup, tcp_check, watch_path,\n"
        "  list_processes,\n"
        "  termux_info, termux_api_status, termux_storage_status,\n"
        "  termux_battery_status, termux_wifi_info, termux_clipboard_get,\n"
        "  termux_clipboard_set, termux_notification, termux_vibrate,\n"
        "  termux_wake_lock\n");
    if (app->cfg->allow_exec) {
        buf_append_cstr(&b,
            "\nExec tools enabled:\n"
            "  exec_command, spawn_bg, bg_read, bg_kill, bg_list,\n"
            "  delegate_codex, delegate_copilot\n");
    } else {
        buf_append_cstr(&b,
            "\nExec tools disabled. Start with --allow-exec to expose exec_command,\n"
            "spawn_bg, bg_read, bg_kill, and bg_list.\n");
    }
    return b.data;
}

static void append_skills_from_root(Buf *b, const char *root, int *any) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s/SKILL.md", root, e->d_name);
        if (file_exists(full)) {
            buf_printf(b, "%s  %s\n", e->d_name, full);
            *any = 1;
        }
    }
    closedir(d);
}

static char *skills_text(void) {
    Buf b;
    buf_init(&b);
    int any = 0;
    const char *custom = getenv("SYSCALL_AGENT_SKILLS_DIR");
    if (custom && *custom) append_skills_from_root(&b, custom, &any);
    append_skills_from_root(&b, "skills", &any);
    const char *home = getenv("HOME");
    if (home && *home) {
        char root[PATH_MAX];
        snprintf(root, sizeof root, "%s/.syscall-agent/skills", home);
        append_skills_from_root(&b, root, &any);
    }
    if (!any) {
        buf_append_cstr(&b,
            "(no local skills found)\n"
            "Checked SYSCALL_AGENT_SKILLS_DIR, ./skills, and ~/.syscall-agent/skills.\n");
    }
    return b.data;
}

static char *auth_text(void) {
    Buf b;
    buf_init(&b);
    const char *provider = getenv_or("SYSCALL_AGENT_AUTH_PROVIDER", "openrouter");
    const char *or_key = getenv("OPENROUTER_API_KEY");
    const char *oa_key = getenv("OPENAI_API_KEY");
    const char *gh = getenv("GH_TOKEN");
    if (!gh || !*gh) gh = getenv("GITHUB_TOKEN");
    char codex[PATH_MAX];
    const char *codex_home = getenv("CODEX_HOME");
    if (codex_home && *codex_home) {
        snprintf(codex, sizeof codex, "%s/auth.json", codex_home);
    } else {
        const char *home = getenv("HOME");
        snprintf(codex, sizeof codex, "%s/.codex/auth.json", home ? home : "");
    }
    buf_printf(&b,
        "provider: %s\n"
        "OPENROUTER_API_KEY: %s\n"
        "OPENAI_API_KEY: %s\n"
        "Codex OAuth file: %s (%s)\n"
        "GitHub token hint: %s\n\n"
        "Codex OAuth and Copilot subscription credentials are detected for visibility only;\n"
        "syscall-agent does not print or repurpose those tokens.\n",
        provider,
        (or_key && *or_key) ? "set" : "missing",
        (oa_key && *oa_key) ? "set" : "missing",
        codex,
        file_exists(codex) ? "present" : "missing",
        (gh && *gh) ? "set" : "missing");
    return b.data;
}

static char *sysinfo_text(void) {
    Buf b;
    buf_init(&b);
    struct utsname u;
    if (uname(&u) == 0) {
        buf_printf(&b, "%s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    } else {
        buf_append_cstr(&b, "uname unavailable\n");
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) buf_printf(&b, "cwd: %s\n", cwd);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    long page = sysconf(_SC_PAGESIZE);
    if (cpus > 0) buf_printf(&b, "cpus_online: %ld\n", cpus);
    if (page > 0) buf_printf(&b, "page_size: %ld\n", page);
    return b.data;
}

static void handle_command(TuiApp *app, const char *line) {
    TuiCommand cmd;
    if (!tui_parse_command(line, &cmd)) return;

    if (cmd.type == TUI_CMD_NEW) {
        clear_entries(app);
        agent_conversation_reset(app->conversation, app->cfg);
        add_welcome(app);
        snprintf(app->status, sizeof app->status, "new session");
    } else if (cmd.type == TUI_CMD_EXIT) {
        app->should_exit = 1;
    } else if (cmd.type == TUI_CMD_HELP) {
        add_entry(app, ENTRY_SYSTEM, "commands",
            "/model opens the live OpenRouter model picker.\n"
            "/models opens the same picker. Type to search, Enter selects, Esc closes.\n"
            "/model N selects a built-in quick choice; /model provider/model-id sets any OpenRouter id directly.\n"
            "/verbose normal hides tool and reasoning detail.\n"
            "/verbose tools shows tool calls and tool output.\n"
            "/verbose reasoning shows model reasoning fields when returned.\n"
            "/verbose all shows tools and reasoning.\n"
            "/tools lists available tool families.\n"
            "/skills lists local skill packs.\n"
            "/auth shows configured auth surfaces without secrets.\n"
            "/sysinfo shows host and working-directory information.\n"
            "/new clears the visible transcript. /exit leaves the TUI.");
    } else if (cmd.type == TUI_CMD_MODELS) {
        open_model_picker(app);
    } else if (cmd.type == TUI_CMD_TOOLS) {
        char *t = tools_text(app);
        add_entry(app, ENTRY_SYSTEM, "tools", t ? t : "");
        free(t);
    } else if (cmd.type == TUI_CMD_SKILLS) {
        char *s = skills_text();
        add_entry(app, ENTRY_SYSTEM, "skills", s ? s : "");
        free(s);
    } else if (cmd.type == TUI_CMD_AUTH) {
        char *a = auth_text();
        add_entry(app, ENTRY_SYSTEM, "auth", a ? a : "");
        free(a);
    } else if (cmd.type == TUI_CMD_SYSINFO) {
        char *s = sysinfo_text();
        add_entry(app, ENTRY_SYSTEM, "system", s ? s : "");
        free(s);
    } else if (cmd.type == TUI_CMD_MODEL) {
        set_current_model(app, cmd.model_id);
    } else if (cmd.type == TUI_CMD_VERBOSE) {
        app->cfg->verbose = cmd.verbose_mode;
        char msg[128];
        snprintf(msg, sizeof msg, "verbose set to %s", tui_verbose_name(app->cfg->verbose));
        add_entry(app, ENTRY_SYSTEM, "verbose", msg);
        snprintf(app->status, sizeof app->status, "%s", msg);
    } else {
        add_entry(app, ENTRY_ERROR, "command", cmd.error ? cmd.error : "unknown command");
    }
}

static void on_agent_event(const AgentEvent *event, void *userdata) {
    TuiApp *app = userdata;
    if (!event) return;

    switch (event->type) {
        case AGENT_EVENT_STATUS:
            snprintf(app->status, sizeof app->status, "%s", event->content ? event->content : "");
            break;
        case AGENT_EVENT_TOOL_CALL:
            add_entry(app, ENTRY_TOOL, event->title ? event->title : "tool", event->content ? event->content : "");
            break;
        case AGENT_EVENT_TOOL_CALL_DELTA:
            append_entry_delta(app, ENTRY_TOOL, event->title ? event->title : "tool call", event->content ? event->content : "");
            break;
        case AGENT_EVENT_TOOL_RESULT:
            add_entry(app, ENTRY_TOOL, event->title ? event->title : "tool result", event->content ? event->content : "");
            break;
        case AGENT_EVENT_REASONING:
            add_entry(app, ENTRY_REASONING, "reasoning", event->content ? event->content : "");
            break;
        case AGENT_EVENT_REASONING_DELTA:
            append_entry_delta(app, ENTRY_REASONING, "reasoning", event->content ? event->content : "");
            break;
        case AGENT_EVENT_ASSISTANT:
            add_entry(app, ENTRY_ASSISTANT, "assistant", event->content ? event->content : "");
            break;
        case AGENT_EVENT_ASSISTANT_DELTA:
            append_entry_delta(app, ENTRY_ASSISTANT, "assistant", event->content ? event->content : "");
            break;
        case AGENT_EVENT_ERROR:
            add_entry(app, ENTRY_ERROR, event->title ? event->title : "error", event->content ? event->content : "");
            break;
    }
    render_app(app);
}

static void submit_line(TuiApp *app) {
    app->input[app->input_len] = '\0';
    char *line = strdup(app->input);
    if (!line) return;
    app->input_len = 0;
    app->input[0] = '\0';

    const char *trimmed = skip_spaces(line);
    if (*trimmed == '\0') {
        free(line);
        return;
    }

    if (*trimmed == '/') {
        handle_command(app, trimmed);
        free(line);
        return;
    }

    add_entry(app, ENTRY_USER, "user", trimmed);
    app->running = 1;
    snprintf(app->status, sizeof app->status, "working");
    render_app(app);
    int rc = agent_conversation_run_with_events(
        app->conversation, app->cfg, trimmed, on_agent_event, app);
    app->running = 0;
    if (rc == 0) {
        snprintf(app->status, sizeof app->status, "ready");
    } else {
        snprintf(app->status, sizeof app->status, "agent exited with rc=%d", rc);
    }
    free(line);
}

static int read_byte_timeout(unsigned char *out, int timeout_ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0 || !(pfd.revents & POLLIN)) return 0;
    return read(STDIN_FILENO, out, 1) == 1;
}

static TuiKey read_escape_key(void) {
    unsigned char a = 0, b = 0, c = 0;
    if (!read_byte_timeout(&a, 30)) return TUI_KEY_ESC;
    if (a != '[') return TUI_KEY_ESC;
    if (!read_byte_timeout(&b, 30)) return TUI_KEY_ESC;

    if (b == 'A') return TUI_KEY_UP;
    if (b == 'B') return TUI_KEY_DOWN;
    if (b == 'H') return TUI_KEY_HOME;
    if (b == 'F') return TUI_KEY_END;
    if (b == '5' || b == '6') {
        if (read_byte_timeout(&c, 30) && c == '~') {
            return b == '5' ? TUI_KEY_PAGE_UP : TUI_KEY_PAGE_DOWN;
        }
    }
    return TUI_KEY_NONE;
}

static int picker_visible_rows(const TuiApp *app) {
    int footer_lines = 3;
    int body_height = app->height - 2 - footer_lines;
    int rows = body_height - 4;
    return rows > 1 ? rows : 1;
}

static void handle_model_picker_key(TuiApp *app, TuiKey key) {
    int visible = picker_visible_rows(app);
    switch (key) {
        case TUI_KEY_ESC:
            close_model_picker(app, "ready");
            break;
        case TUI_KEY_UP:
            model_picker_move(app, -1);
            break;
        case TUI_KEY_DOWN:
            model_picker_move(app, 1);
            break;
        case TUI_KEY_PAGE_UP:
            model_picker_move(app, -visible);
            break;
        case TUI_KEY_PAGE_DOWN:
            model_picker_move(app, visible);
            break;
        case TUI_KEY_HOME:
            app->model_selected = 0;
            break;
        case TUI_KEY_END:
            if (app->model_match_len > 0) app->model_selected = app->model_match_len - 1;
            break;
        default:
            break;
    }
}

static void handle_model_picker_byte(TuiApp *app, unsigned char ch) {
    if (ch == 3) {
        close_model_picker(app, "ready");
        return;
    }
    if (ch == 4) {
        app->should_exit = 1;
        return;
    }
    if (ch == '\r' || ch == '\n') {
        model_picker_select(app);
        return;
    }
    if (ch == 18) {
        if (load_openrouter_models(app, 1) == 0) {
            app->model_picker_open = 1;
            snprintf(app->status, sizeof app->status, "model catalog refreshed");
        }
        return;
    }
    if (ch == 27) {
        handle_model_picker_key(app, read_escape_key());
        return;
    }
    if (ch == 127 || ch == 8) {
        if (app->model_query_len > 0) {
            app->model_query[--app->model_query_len] = '\0';
            app->model_selected = 0;
            app->model_scroll = 0;
            model_picker_rebuild_matches(app);
        }
        return;
    }
    if (ch == 21) {
        app->model_query_len = 0;
        app->model_query[0] = '\0';
        app->model_selected = 0;
        app->model_scroll = 0;
        model_picker_rebuild_matches(app);
        return;
    }
    if (isprint(ch) && app->model_query_len + 1 < sizeof app->model_query) {
        app->model_query[app->model_query_len++] = (char)ch;
        app->model_query[app->model_query_len] = '\0';
        app->model_selected = 0;
        app->model_scroll = 0;
        model_picker_rebuild_matches(app);
    }
}

int tui_run(AgentConfig *cfg) {
    TuiApp app;
    memset(&app, 0, sizeof app);
    app.cfg = cfg;
    openrouter_model_catalog_init(&app.model_catalog);
    snprintf(app.status, sizeof app.status, "ready");
    app.conversation = agent_conversation_new(cfg);
    if (!app.conversation) {
        fprintf(stderr, "ERROR: could not initialize TUI conversation\n");
        return 2;
    }

    if (enable_raw(&app) != 0) {
        agent_conversation_free(app.conversation);
        return 2;
    }
    signal(SIGWINCH, on_sigwinch);
    add_welcome(&app);
    render_app(&app);

    while (!app.should_exit) {
        if (g_resize_pending) render_app(&app);
        unsigned char ch = 0;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;

        if (app.model_picker_open) {
            handle_model_picker_byte(&app, ch);
        } else if (ch == 3 || ch == 4) {
            app.should_exit = 1;
        } else if (ch == '\r' || ch == '\n') {
            submit_line(&app);
        } else if (ch == 127 || ch == 8) {
            if (app.input_len > 0) app.input[--app.input_len] = '\0';
        } else if (ch == 27) {
            (void)read_escape_key();
        } else if (isprint(ch) && app.input_len + 1 < sizeof app.input) {
            app.input[app.input_len++] = (char)ch;
            app.input[app.input_len] = '\0';
        }
        render_app(&app);
    }

    disable_raw(&app);
    openrouter_model_catalog_free(&app.model_catalog);
    free(app.model_matches);
    free(app.owned_model_id);
    agent_conversation_free(app.conversation);
    free_entries(&app);
    return 0;
}
