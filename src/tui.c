#include "tui.h"

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TUI_MIN_COLS 48
#define TUI_MIN_ROWS 12
#define TUI_MAX_ACTIVITY 64
#define TUI_FRAME_MS 75

enum {
    KEY_NONE = 0,
    KEY_ENTER = 1000,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGEUP,
    KEY_PAGEDOWN,
    KEY_CTRL_C,
    KEY_CTRL_D,
};

typedef enum {
    TUI_ROLE_USER = 1,
    TUI_ROLE_ASSISTANT,
    TUI_ROLE_INFO,
} TuiRole;

typedef struct {
    TuiRole role;
    char *text;
} TuiMessage;

typedef struct {
    char **items;
    int count;
    int cap;
} StrList;

typedef struct {
    int active;
    struct termios saved;
} TuiTerminal;

typedef struct {
    pthread_t thread;
    int active;
    char *prompt;
} TuiWorker;

typedef struct {
    const AgentConfig *cfg;
    AgentSession *session;
    pthread_mutex_t mu;

    TuiTerminal term;
    TuiWorker worker;

    TuiMessage *messages;
    int message_count;
    int message_cap;

    char **activity;
    int activity_count;
    int activity_cap;

    Buf input;
    size_t cursor;
    int scroll_lines;

    int rows;
    int cols;
    int busy;
    int should_exit;
    int exit_after_response;
    int last_step;
    int max_steps;
    int request_count;
    char phase[128];
    char last_error[256];
} TuiApp;

static volatile sig_atomic_t g_tui_resized = 0;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int ensure_buf_space(Buf *b, size_t extra) {
    size_t need;
    size_t cap;
    char *next;
    if (!b) return -1;
    need = b->len + extra + 1;
    if (b->cap >= need && b->data) return 0;

    cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    next = realloc(b->data, cap);
    if (!next) return -1;
    b->data = next;
    if (b->cap == 0) b->data[0] = '\0';
    b->cap = cap;
    return 0;
}

static void tui_on_winch(int signo) {
    (void)signo;
    g_tui_resized = 1;
}

static void strlist_init(StrList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void strlist_free(StrList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int strlist_push_owned(StrList *list, char *line) {
    if (list->count == list->cap) {
        int next_cap = list->cap ? list->cap * 2 : 16;
        char **next = realloc(list->items, (size_t)next_cap * sizeof *next);
        if (!next) {
            free(line);
            return -1;
        }
        list->items = next;
        list->cap = next_cap;
    }
    list->items[list->count++] = line;
    return 0;
}

static int strlist_push_copy(StrList *list, const char *line) {
    return strlist_push_owned(list, strdup(line ? line : ""));
}

static char *line_with_prefix(const char *prefix, const char *chunk) {
    Buf b;
    buf_init(&b);
    buf_append_cstr(&b, prefix ? prefix : "");
    buf_append_cstr(&b, chunk ? chunk : "");
    return b.data ? b.data : strdup("");
}

static void wrap_chunked(StrList *out,
                         const char *text,
                         const char *first_prefix,
                         const char *next_prefix,
                         int width) {
    const char *src = text ? text : "";
    const char *first = first_prefix ? first_prefix : "";
    const char *next = next_prefix ? next_prefix : first;
    size_t first_len = strlen(first);
    size_t next_len = strlen(next);

    if (width <= 1) {
        strlist_push_copy(out, "");
        return;
    }

    if (*src == '\0') {
        strlist_push_owned(out, line_with_prefix(first, ""));
        return;
    }

    while (1) {
        const char *line_end = strchr(src, '\n');
        size_t seg_len = line_end ? (size_t)(line_end - src) : strlen(src);
        const char *prefix = first;
        size_t prefix_len = first_len;
        size_t off = 0;

        if (seg_len == 0) {
            strlist_push_owned(out, line_with_prefix(prefix, ""));
        } else {
            while (off < seg_len) {
                int room = width - (int)prefix_len;
                if (room < 1) room = 1;
                size_t take = seg_len - off;
                if ((int)take > room) take = (size_t)room;

                char *chunk = malloc((size_t)room + 1);
                if (!chunk) return;
                memcpy(chunk, src + off, take);
                chunk[take] = '\0';
                {
                    char *line = line_with_prefix(prefix, chunk);
                    free(chunk);
                    if (!line) return;
                    strlist_push_owned(out, line);
                }
                off += take;
                prefix = next;
                prefix_len = next_len;
            }
        }

        if (!line_end) break;
        src = line_end + 1;
        if (*src == '\0') {
            strlist_push_owned(out, line_with_prefix(next, ""));
            break;
        }
    }
}

static void set_text(char *dst, size_t dst_size, const char *fmt, ...) {
    va_list ap;
    if (!dst || dst_size == 0) return;
    va_start(ap, fmt);
    vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);
}

static void update_size(TuiApp *app) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        app->cols = ws.ws_col;
        app->rows = ws.ws_row;
    } else {
        app->cols = 80;
        app->rows = 24;
    }
}

static int term_enter(TuiTerminal *term) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &term->saved) != 0) return -1;
    raw = term->saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= (tcflag_t)CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    term->active = 1;
    write(STDOUT_FILENO, "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l",
        sizeof "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l" - 1);
    return 0;
}

static void term_leave(TuiTerminal *term) {
    if (!term->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
    write(STDOUT_FILENO, "\x1b[0m\x1b[?25h\x1b[?1049l",
        sizeof "\x1b[0m\x1b[?25h\x1b[?1049l" - 1);
    term->active = 0;
}

static int read_key(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return KEY_NONE;

    if (c == 3) return KEY_CTRL_C;
    if (c == 4) return KEY_CTRL_D;
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    if (c != 27) return (int)c;

    unsigned char seq[3] = {0};
    if (read(STDIN_FILENO, &seq[0], 1) <= 0) return KEY_NONE;

    if (seq[0] == '[') {
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return KEY_NONE;
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) <= 0) return KEY_NONE;
            if (seq[2] == '~') {
                if (seq[1] == '3') return KEY_DELETE;
                if (seq[1] == '5') return KEY_PAGEUP;
                if (seq[1] == '6') return KEY_PAGEDOWN;
            }
            return KEY_NONE;
        }
        if (seq[1] == 'C') return KEY_RIGHT;
        if (seq[1] == 'D') return KEY_LEFT;
        if (seq[1] == 'H' || seq[1] == '1') return KEY_HOME;
        if (seq[1] == 'F' || seq[1] == '4') return KEY_END;
        return KEY_NONE;
    }

    if (seq[0] == 'O') {
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return KEY_NONE;
        if (seq[1] == 'H') return KEY_HOME;
        if (seq[1] == 'F') return KEY_END;
    }
    return KEY_NONE;
}

static void add_message_locked(TuiApp *app, TuiRole role, const char *text) {
    if (app->message_count == app->message_cap) {
        int next_cap = app->message_cap ? app->message_cap * 2 : 32;
        TuiMessage *next = realloc(app->messages, (size_t)next_cap * sizeof *next);
        if (!next) return;
        app->messages = next;
        app->message_cap = next_cap;
    }
    app->messages[app->message_count].role = role;
    app->messages[app->message_count].text = strdup(text ? text : "");
    if (!app->messages[app->message_count].text) return;
    app->message_count++;
}

static void add_activity_locked(TuiApp *app, const char *fmt, ...) {
    Buf b;
    va_list ap;
    buf_init(&b);
    va_start(ap, fmt);
    {
        va_list ap_copy;
        va_copy(ap_copy, ap);
        int n = vsnprintf(NULL, 0, fmt, ap);
        if (n >= 0) {
            if (ensure_buf_space(&b, (size_t)n) != 0) {
                va_end(ap_copy);
                va_end(ap);
                return;
            }
            vsnprintf(b.data + b.len, (size_t)n + 1, fmt, ap_copy);
            b.len += (size_t)n;
        }
        va_end(ap_copy);
    }
    va_end(ap);

    if (!b.data) return;
    if (app->activity_count == app->activity_cap) {
        int next_cap = app->activity_cap ? app->activity_cap * 2 : 32;
        char **next = realloc(app->activity, (size_t)next_cap * sizeof *next);
        if (!next) {
            buf_free(&b);
            return;
        }
        app->activity = next;
        app->activity_cap = next_cap;
    }
    app->activity[app->activity_count++] = b.data;

    if (app->activity_count > TUI_MAX_ACTIVITY) {
        free(app->activity[0]);
        memmove(app->activity, app->activity + 1,
            (size_t)(app->activity_count - 1) * sizeof *app->activity);
        app->activity_count--;
    }
}

static void tui_on_agent_event(const AgentEvent *event, void *userdata) {
    TuiApp *app = (TuiApp *)userdata;
    pthread_mutex_lock(&app->mu);
    switch (event->type) {
    case AGENT_EVENT_STEP_START:
        app->last_step = event->step_index + 1;
        app->max_steps = event->max_steps;
        set_text(app->phase, sizeof app->phase, "Step %d/%d",
            event->step_index + 1, event->max_steps);
        add_activity_locked(app, "Step %d/%d started",
            event->step_index + 1, event->max_steps);
        break;
    case AGENT_EVENT_MODEL_REQUEST_START:
        set_text(app->phase, sizeof app->phase, "Waiting for model");
        break;
    case AGENT_EVENT_TOOL_CALL_START:
        set_text(app->phase, sizeof app->phase, "Running %s",
            event->tool_name ? event->tool_name : "tool");
        add_activity_locked(app, "Tool: %s",
            event->tool_name ? event->tool_name : "(unknown)");
        break;
    case AGENT_EVENT_TOOL_CALL_RESULT:
        set_text(app->phase, sizeof app->phase, "Tool completed");
        add_activity_locked(app, "Done: %s (%ld ms)",
            event->tool_name ? event->tool_name : "(unknown)",
            event->duration_ms);
        break;
    case AGENT_EVENT_FINAL_RESPONSE:
        set_text(app->phase, sizeof app->phase, "Response ready");
        add_activity_locked(app, "Response ready");
        break;
    case AGENT_EVENT_ERROR:
        set_text(app->phase, sizeof app->phase, "Error");
        set_text(app->last_error, sizeof app->last_error, "%s",
            event->message ? event->message : "unknown error");
        add_activity_locked(app, "Error: %.120s",
            event->message ? event->message : "unknown error");
        break;
    }
    pthread_mutex_unlock(&app->mu);
}

static void free_messages(TuiApp *app) {
    for (int i = 0; i < app->message_count; i++) free(app->messages[i].text);
    free(app->messages);
    app->messages = NULL;
    app->message_count = 0;
    app->message_cap = 0;
}

static void free_activity(TuiApp *app) {
    for (int i = 0; i < app->activity_count; i++) free(app->activity[i]);
    free(app->activity);
    app->activity = NULL;
    app->activity_count = 0;
    app->activity_cap = 0;
}

static void trim_visible(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
}

static char *pad_or_trim(const char *text, int width) {
    char *out;
    size_t len = text ? strlen(text) : 0;
    if (width < 0) width = 0;
    out = malloc((size_t)width + 1);
    if (!out) return NULL;

    if ((int)len > width) {
        memcpy(out, text, (size_t)width);
    } else if (len > 0) {
        memcpy(out, text, len);
        memset(out + len, ' ', (size_t)width - len);
    } else {
        memset(out, ' ', (size_t)width);
    }
    out[width] = '\0';
    return out;
}

static void box_top(StrList *rows, const char *title, int width) {
    char *line = malloc((size_t)width + 1);
    int title_len = title ? (int)strlen(title) : 0;
    if (!line) return;
    if (width < 2) {
        line[0] = '\0';
        strlist_push_owned(rows, line);
        return;
    }
    memset(line, '-', (size_t)width);
    line[0] = '+';
    line[width - 1] = '+';
    if (title_len > 0 && width > 4) {
        int start = 2;
        int room = width - 4;
        if (title_len > room) title_len = room;
        line[1] = ' ';
        memcpy(line + start, title, (size_t)title_len);
        if (start + title_len < width - 1) line[start + title_len] = ' ';
    }
    line[width] = '\0';
    strlist_push_owned(rows, line);
}

static void build_box_rows(StrList *rows,
                           const char *title,
                           int width,
                           int height,
                           StrList *content,
                           int scroll_from_bottom) {
    int inner_w = width - 2;
    int inner_h = height - 2;
    int start = 0;

    strlist_init(rows);
    if (width < 4 || height < 3) {
        for (int i = 0; i < height; i++) strlist_push_copy(rows, "");
        return;
    }

    box_top(rows, title, width);

    if (content->count > inner_h) {
        start = content->count - inner_h - scroll_from_bottom;
        if (start < 0) start = 0;
    }
    if (content->count <= inner_h) start = 0;

    for (int row = 0; row < inner_h; row++) {
        int idx = start + row;
        const char *src = (idx >= 0 && idx < content->count) ? content->items[idx] : "";
        char *body = pad_or_trim(src, inner_w);
        Buf b;
        buf_init(&b);
        buf_append_cstr(&b, "|");
        buf_append_cstr(&b, body ? body : "");
        buf_append_cstr(&b, "|");
        free(body);
        strlist_push_owned(rows, b.data ? b.data : strdup(""));
    }

    {
        char *bottom = malloc((size_t)width + 1);
        if (!bottom) return;
        memset(bottom, '-', (size_t)width);
        bottom[0] = '+';
        bottom[width - 1] = '+';
        bottom[width] = '\0';
        strlist_push_owned(rows, bottom);
    }
}

static void build_transcript_lines(TuiApp *app, StrList *lines, int width) {
    strlist_init(lines);
    if (app->message_count == 0) {
        strlist_push_copy(lines, "No messages yet. Type a prompt and press Enter.");
        return;
    }
    for (int i = 0; i < app->message_count; i++) {
        const char *first = "Info: ";
        const char *next = "      ";
        if (app->messages[i].role == TUI_ROLE_USER) {
            first = "You:  ";
            next = "      ";
        } else if (app->messages[i].role == TUI_ROLE_ASSISTANT) {
            first = "Agent:";
            next = "      ";
        }
        wrap_chunked(lines, app->messages[i].text, first, next, width);
        if (i != app->message_count - 1) strlist_push_copy(lines, "");
    }
}

static void build_activity_lines(TuiApp *app, StrList *lines, int width) {
    strlist_init(lines);
    {
        char header[160];
        set_text(header, sizeof header, "Phase: %s",
            app->phase[0] ? app->phase : (app->busy ? "Working" : "Idle"));
        wrap_chunked(lines, header, "", "", width);
    }
    {
        char status[160];
        set_text(status, sizeof status, "Turns: %d | Step: %d/%d",
            app->request_count,
            app->last_step,
            app->max_steps > 0 ? app->max_steps : app->cfg->max_steps);
        wrap_chunked(lines, status, "", "", width);
    }
    strlist_push_copy(lines, "");
    if (app->activity_count == 0) {
        strlist_push_copy(lines, "No activity yet.");
        return;
    }
    for (int i = 0; i < app->activity_count; i++) {
        wrap_chunked(lines, app->activity[i], "* ", "  ", width);
    }
}

static void build_footer(TuiApp *app, Buf *out) {
    char footer[512];
    char line[1024];
    set_text(footer, sizeof footer,
        " %s | model %s | exec %s | Enter send | PgUp/PgDn scroll | Ctrl-C quit%s ",
        app->busy ? "busy" : "idle",
        app->cfg->model,
        app->cfg->allow_exec ? "on" : "off",
        app->exit_after_response ? " after response" : "");
    snprintf(line, sizeof line, "\x1b[7m%-*.*s\x1b[0m",
        app->cols, app->cols, footer);
    buf_append_cstr(out, line);
    buf_append_cstr(out, "\n");
}

static void append_rows(Buf *out, StrList *rows) {
    for (int i = 0; i < rows->count; i++) {
        buf_append_cstr(out, rows->items[i] ? rows->items[i] : "");
        buf_append_cstr(out, "\n");
    }
}

static size_t input_window_start(const char *input, size_t cursor, int width) {
    size_t len = input ? strlen(input) : 0;
    if (width <= 0 || cursor <= (size_t)width) return 0;
    if (cursor > len) cursor = len;
    return cursor - (size_t)width;
}

static void build_input_rows(TuiApp *app, StrList *rows, int width, int *cursor_col_out) {
    char *line = NULL;
    size_t start = 0;
    int inner_w = width - 2;
    int prefix_len = 2;
    strlist_init(rows);

    if (width < 4) {
        strlist_push_copy(rows, "");
        strlist_push_copy(rows, "");
        strlist_push_copy(rows, "");
        *cursor_col_out = 1;
        return;
    }

    box_top(rows, app->busy ? " Input (waiting) " : " Input ", width);

    if (app->busy) {
        line = pad_or_trim("[waiting for response]", inner_w);
        *cursor_col_out = 1;
    } else {
        int visible_w = inner_w - prefix_len;
        size_t len = app->input.data ? app->input.len : 0;
        start = input_window_start(app->input.data ? app->input.data : "", app->cursor, visible_w);
        if (visible_w < 1) visible_w = 1;
        {
            char *display = malloc((size_t)visible_w + 1);
            size_t take = 0;
            if (display) {
                if (start < len) {
                    take = len - start;
                    if ((int)take > visible_w) take = (size_t)visible_w;
                    memcpy(display, app->input.data + start, take);
                }
                display[take] = '\0';
                if (take < (size_t)visible_w) {
                    memset(display + take, ' ', (size_t)visible_w - take);
                    display[visible_w] = '\0';
                }
                {
                    Buf b;
                    buf_init(&b);
                    buf_append_cstr(&b, "> ");
                    buf_append_cstr(&b, display);
                    line = pad_or_trim(b.data ? b.data : "", inner_w);
                    buf_free(&b);
                }
                free(display);
            }
        }
        *cursor_col_out = 1 + prefix_len + (int)(app->cursor - start);
        if (*cursor_col_out > inner_w) *cursor_col_out = inner_w;
        if (*cursor_col_out < 1 + prefix_len) *cursor_col_out = 1 + prefix_len;
    }

    {
        Buf b;
        buf_init(&b);
        buf_append_cstr(&b, "|");
        buf_append_cstr(&b, line ? line : "");
        buf_append_cstr(&b, "|");
        strlist_push_owned(rows, b.data ? b.data : strdup(""));
    }
    free(line);

    {
        char *bottom = malloc((size_t)width + 1);
        if (!bottom) return;
        memset(bottom, '-', (size_t)width);
        bottom[0] = '+';
        bottom[width - 1] = '+';
        bottom[width] = '\0';
        strlist_push_owned(rows, bottom);
    }
}

static void render_small(TuiApp *app, Buf *out) {
    char line[512];
    buf_append_cstr(out, "low_level_agent tui\n");
    snprintf(line, sizeof line, "state: %s | model: %s\n",
        app->busy ? "busy" : "idle", app->cfg->model);
    buf_append_cstr(out, line);
    buf_append_cstr(out, "window too small for full layout\n");
    if (app->message_count > 0) {
        TuiMessage *last = &app->messages[app->message_count - 1];
        const char *label = last->role == TUI_ROLE_USER ? "you" :
            (last->role == TUI_ROLE_ASSISTANT ? "agent" : "info");
        snprintf(line, sizeof line, "%s: %.200s\n", label, last->text ? last->text : "");
        buf_append_cstr(out, line);
    }
    snprintf(line, sizeof line, "> %.200s\n", app->input.data ? app->input.data : "");
    buf_append_cstr(out, line);
    build_footer(app, out);
}

static void render_ui(TuiApp *app) {
    Buf out;
    int footer_h = 1;
    int input_h = 3;
    int activity_h = (app->cols >= 100) ? (app->rows - input_h - footer_h) : 7;
    int top_h;
    int cursor_row = 1;
    int cursor_col = 1;
    StrList transcript_lines, activity_lines, transcript_box, activity_box, input_box;

    buf_init(&out);
    buf_append_cstr(&out, "\x1b[H");

    pthread_mutex_lock(&app->mu);
    if (app->cols < TUI_MIN_COLS || app->rows < TUI_MIN_ROWS) {
        render_small(app, &out);
        if (!app->busy) {
            buf_append_cstr(&out, "\x1b[?25h");
        }
        pthread_mutex_unlock(&app->mu);
        if (out.data) write(STDOUT_FILENO, out.data, out.len);
        buf_free(&out);
        return;
    }

    if (app->cols >= 100) {
        int activity_w = app->cols / 3;
        int transcript_w = app->cols - activity_w - 1;
        top_h = app->rows - input_h - footer_h;

        build_transcript_lines(app, &transcript_lines, transcript_w - 2);
        build_activity_lines(app, &activity_lines, activity_w - 2);
        build_box_rows(&transcript_box, " Transcript ", transcript_w, top_h,
            &transcript_lines, app->scroll_lines);
        build_box_rows(&activity_box, " Activity ", activity_w, top_h,
            &activity_lines, 0);
        build_input_rows(app, &input_box, app->cols, &cursor_col);
        cursor_row = top_h + 2;

        for (int i = 0; i < top_h; i++) {
            buf_append_cstr(&out, transcript_box.items[i]);
            buf_append_cstr(&out, " ");
            buf_append_cstr(&out, activity_box.items[i]);
            buf_append_cstr(&out, "\n");
        }
        append_rows(&out, &input_box);
        build_footer(app, &out);

        strlist_free(&transcript_lines);
        strlist_free(&activity_lines);
        strlist_free(&transcript_box);
        strlist_free(&activity_box);
        strlist_free(&input_box);
    } else {
        int transcript_h = app->rows - activity_h - input_h - footer_h;
        if (transcript_h < 5) transcript_h = 5;

        build_transcript_lines(app, &transcript_lines, app->cols - 2);
        build_activity_lines(app, &activity_lines, app->cols - 2);
        build_box_rows(&transcript_box, " Transcript ", app->cols, transcript_h,
            &transcript_lines, app->scroll_lines);
        build_box_rows(&activity_box, " Activity ", app->cols, activity_h,
            &activity_lines, 0);
        build_input_rows(app, &input_box, app->cols, &cursor_col);
        cursor_row = transcript_h + activity_h + 2;

        append_rows(&out, &transcript_box);
        append_rows(&out, &activity_box);
        append_rows(&out, &input_box);
        build_footer(app, &out);

        strlist_free(&transcript_lines);
        strlist_free(&activity_lines);
        strlist_free(&transcript_box);
        strlist_free(&activity_box);
        strlist_free(&input_box);
    }

    if (app->busy) {
        buf_append_cstr(&out, "\x1b[?25l");
    } else {
        char cursor_buf[64];
        snprintf(cursor_buf, sizeof cursor_buf, "\x1b[%d;%dH\x1b[?25h",
            cursor_row, cursor_col + 1);
        buf_append_cstr(&out, cursor_buf);
    }
    pthread_mutex_unlock(&app->mu);

    if (out.data) write(STDOUT_FILENO, out.data, out.len);
    buf_free(&out);
}

static void input_insert_locked(TuiApp *app, int ch) {
    size_t len = app->input.len;
    if (app->cursor > len) app->cursor = len;
    if (ensure_buf_space(&app->input, 1) != 0) return;
    memmove(app->input.data + app->cursor + 1,
        app->input.data + app->cursor,
        len - app->cursor + 1);
    app->input.data[app->cursor] = (char)ch;
    app->input.len++;
    app->cursor++;
}

static void input_backspace_locked(TuiApp *app) {
    if (app->cursor == 0 || app->input.len == 0) return;
    memmove(app->input.data + app->cursor - 1,
        app->input.data + app->cursor,
        app->input.len - app->cursor + 1);
    app->input.len--;
    app->cursor--;
}

static void input_delete_locked(TuiApp *app) {
    if (app->cursor >= app->input.len || app->input.len == 0) return;
    memmove(app->input.data + app->cursor,
        app->input.data + app->cursor + 1,
        app->input.len - app->cursor);
    app->input.len--;
}

typedef struct {
    TuiApp *app;
    char *prompt;
} WorkerArgs;

static void *worker_main(void *opaque) {
    WorkerArgs *args = (WorkerArgs *)opaque;
    TuiApp *app = args->app;
    AgentHooks hooks = {
        .on_event = tui_on_agent_event,
        .userdata = app,
    };
    char *reply = NULL;
    int rc = agent_session_run(app->session, args->prompt, &hooks, &reply);

    pthread_mutex_lock(&app->mu);
    if (rc == 0) {
        add_message_locked(app, TUI_ROLE_ASSISTANT, reply ? reply : "(no content)");
        set_text(app->phase, sizeof app->phase, "Idle");
        app->last_error[0] = '\0';
        add_activity_locked(app, "Turn complete");
    } else {
        const char *msg = app->last_error[0] ? app->last_error : "Request failed";
        add_message_locked(app, TUI_ROLE_INFO, msg);
        set_text(app->phase, sizeof app->phase, "Idle");
        add_activity_locked(app, "Turn failed");
    }
    app->busy = 0;
    pthread_mutex_unlock(&app->mu);

    free(reply);
    free(args->prompt);
    free(args);
    return NULL;
}

static int submit_prompt(TuiApp *app, const char *prompt_text) {
    WorkerArgs *args;
    pthread_t tid;

    args = calloc(1, sizeof *args);
    if (!args) return -1;
    args->app = app;
    args->prompt = strdup(prompt_text ? prompt_text : "");
    if (!args->prompt) {
        free(args);
        return -1;
    }

    if (pthread_create(&tid, NULL, worker_main, args) != 0) {
        free(args->prompt);
        free(args);
        return -1;
    }

    app->worker.thread = tid;
    app->worker.active = 1;
    return 0;
}

static void maybe_join_worker(TuiApp *app) {
    int should_join = 0;
    pthread_t tid;

    pthread_mutex_lock(&app->mu);
    if (app->worker.active && !app->busy) {
        should_join = 1;
        tid = app->worker.thread;
        app->worker.active = 0;
    }
    pthread_mutex_unlock(&app->mu);

    if (should_join) pthread_join(tid, NULL);
}

static void begin_turn(TuiApp *app, const char *prompt) {
    pthread_mutex_lock(&app->mu);
    if (app->busy) {
        pthread_mutex_unlock(&app->mu);
        return;
    }
    add_message_locked(app, TUI_ROLE_USER, prompt);
    set_text(app->phase, sizeof app->phase, "Starting request");
    app->last_error[0] = '\0';
    app->last_step = 0;
    app->max_steps = app->cfg->max_steps;
    app->busy = 1;
    app->request_count++;
    app->scroll_lines = 0;
    add_activity_locked(app, "Submitted prompt");
    pthread_mutex_unlock(&app->mu);

    if (submit_prompt(app, prompt) != 0) {
        pthread_mutex_lock(&app->mu);
        app->busy = 0;
        set_text(app->phase, sizeof app->phase, "Idle");
        set_text(app->last_error, sizeof app->last_error, "%s",
            "could not start worker thread");
        add_message_locked(app, TUI_ROLE_INFO, app->last_error);
        add_activity_locked(app, "Error: could not start worker thread");
        pthread_mutex_unlock(&app->mu);
    }
}

static void handle_keypress(TuiApp *app, int key) {
    char *prompt = NULL;

    pthread_mutex_lock(&app->mu);
    if (key == KEY_PAGEUP) {
        app->scroll_lines += 3;
        pthread_mutex_unlock(&app->mu);
        return;
    }
    if (key == KEY_PAGEDOWN) {
        app->scroll_lines -= 3;
        if (app->scroll_lines < 0) app->scroll_lines = 0;
        pthread_mutex_unlock(&app->mu);
        return;
    }
    if (key == KEY_CTRL_C || key == KEY_CTRL_D) {
        if (app->busy) {
            app->exit_after_response = 1;
            add_activity_locked(app, "Will exit after the current response");
        } else {
            app->should_exit = 1;
        }
        pthread_mutex_unlock(&app->mu);
        return;
    }
    if (app->busy) {
        pthread_mutex_unlock(&app->mu);
        return;
    }

    if (key == KEY_LEFT) {
        if (app->cursor > 0) app->cursor--;
    } else if (key == KEY_RIGHT) {
        if (app->cursor < app->input.len) app->cursor++;
    } else if (key == KEY_HOME) {
        app->cursor = 0;
    } else if (key == KEY_END) {
        app->cursor = app->input.len;
    } else if (key == KEY_BACKSPACE) {
        input_backspace_locked(app);
    } else if (key == KEY_DELETE) {
        input_delete_locked(app);
    } else if (key == KEY_ENTER) {
        if (app->input.len > 0) {
            prompt = strdup(app->input.data ? app->input.data : "");
            app->input.len = 0;
            if (app->input.data) app->input.data[0] = '\0';
            app->cursor = 0;
        }
    } else if (key >= 32 && key != 127) {
        input_insert_locked(app, key);
    }
    pthread_mutex_unlock(&app->mu);

    if (prompt) {
        trim_visible(prompt);
        if (*prompt) begin_turn(app, prompt);
        free(prompt);
    }
}

static char *read_prompt_stdin(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n <= 0) {
        free(line);
        return NULL;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    return line;
}

static void app_init(TuiApp *app, const AgentConfig *cfg) {
    memset(app, 0, sizeof *app);
    app->cfg = cfg;
    pthread_mutex_init(&app->mu, NULL);
    buf_init(&app->input);
    set_text(app->phase, sizeof app->phase, "Idle");
}

static void app_destroy(TuiApp *app) {
    maybe_join_worker(app);
    term_leave(&app->term);
    agent_session_free(app->session);
    free_messages(app);
    free_activity(app);
    buf_free(&app->input);
    pthread_mutex_destroy(&app->mu);
}

int tui_run(const AgentConfig *cfg, const char *initial_prompt) {
    TuiApp app;
    struct sigaction sa_old;
    struct sigaction sa_new;
    long next_frame = 0;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        char *stdin_prompt = NULL;
        if (initial_prompt) return agent_run(cfg, initial_prompt);
        stdin_prompt = read_prompt_stdin();
        if (!stdin_prompt) {
            fprintf(stderr, "ERROR: no prompt provided (give one as an arg or via stdin)\n");
            return 2;
        }
        {
            int rc = agent_run(cfg, stdin_prompt);
            free(stdin_prompt);
            return rc;
        }
    }

    app_init(&app, cfg);
    app.session = agent_session_new(cfg);
    if (!app.session) {
        fprintf(stderr, "ERROR: could not initialize TUI session\n");
        app_destroy(&app);
        return 1;
    }
    update_size(&app);

    if (term_enter(&app.term) != 0) {
        fprintf(stderr, "ERROR: could not enter raw terminal mode\n");
        app_destroy(&app);
        return 1;
    }

    memset(&sa_new, 0, sizeof sa_new);
    sa_new.sa_handler = tui_on_winch;
    sigemptyset(&sa_new.sa_mask);
    sigaction(SIGWINCH, &sa_new, &sa_old);

    pthread_mutex_lock(&app.mu);
    add_message_locked(&app, TUI_ROLE_INFO,
        "Minimal TUI ready. Enter sends, PgUp/PgDn scroll transcript, Ctrl-C exits.");
    add_activity_locked(&app, "TUI started");
    pthread_mutex_unlock(&app.mu);

    if (initial_prompt && *initial_prompt) begin_turn(&app, initial_prompt);

    while (!app.should_exit) {
        fd_set rfds;
        struct timeval tv;
        int rc;
        long now;

        if (g_tui_resized) {
            g_tui_resized = 0;
            pthread_mutex_lock(&app.mu);
            update_size(&app);
            pthread_mutex_unlock(&app.mu);
        }

        now = now_ms();
        if (now >= next_frame) {
            render_ui(&app);
            next_frame = now + TUI_FRAME_MS;
        }

        maybe_join_worker(&app);
        pthread_mutex_lock(&app.mu);
        if (!app.busy && app.exit_after_response) app.should_exit = 1;
        pthread_mutex_unlock(&app.mu);
        if (app.should_exit) break;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = TUI_FRAME_MS * 1000;
        rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            int key;
            while ((key = read_key()) != KEY_NONE) handle_keypress(&app, key);
        } else if (rc < 0 && errno != EINTR) {
            pthread_mutex_lock(&app.mu);
            add_activity_locked(&app, "Input error: %s", strerror(errno));
            pthread_mutex_unlock(&app.mu);
        }
    }

    maybe_join_worker(&app);
    render_ui(&app);
    sigaction(SIGWINCH, &sa_old, NULL);
    app_destroy(&app);
    return 0;
}
