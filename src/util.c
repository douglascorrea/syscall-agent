#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

void buf_init(Buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + need + 1) nc *= 2;
    b->data = realloc(b->data, nc);
    b->cap = nc;
}

void buf_append(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_cstr(Buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_grow(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    b->len += (size_t)n;
    va_end(ap2);
}

char *read_text_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

int write_text_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int append_text_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

char *url_encode(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_unreserved(c)) {
            *p++ = (char)c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}

/* very small HTML→text: strip tags, decode a few entities, collapse whitespace,
 * skip <script>/<style> blocks entirely. */
void strip_html(const char *html, Buf *out) {
    const char *p = html;
    int in_tag = 0;
    int last_was_space = 1;
    while (*p) {
        if (!in_tag) {
            if (*p == '<') {
                /* skip script/style blocks */
                if (strncasecmp(p, "<script", 7) == 0 || strncasecmp(p, "<style", 6) == 0) {
                    const char *tag_end = (strncasecmp(p, "<script", 7) == 0) ? "</script" : "</style";
                    const char *q = strcasestr(p + 1, tag_end);
                    if (!q) break;
                    p = q;
                    while (*p && *p != '>') p++;
                    if (*p) p++;
                    continue;
                }
                in_tag = 1;
                p++;
                continue;
            }
            if (*p == '&') {
                if (strncmp(p, "&amp;", 5) == 0)  { buf_append_cstr(out, "&");  p += 5; last_was_space = 0; continue; }
                if (strncmp(p, "&lt;", 4) == 0)   { buf_append_cstr(out, "<");  p += 4; last_was_space = 0; continue; }
                if (strncmp(p, "&gt;", 4) == 0)   { buf_append_cstr(out, ">");  p += 4; last_was_space = 0; continue; }
                if (strncmp(p, "&quot;", 6) == 0) { buf_append_cstr(out, "\""); p += 6; last_was_space = 0; continue; }
                if (strncmp(p, "&#39;", 5) == 0)  { buf_append_cstr(out, "'");  p += 5; last_was_space = 0; continue; }
                if (strncmp(p, "&nbsp;", 6) == 0) { buf_append_cstr(out, " ");  p += 6; last_was_space = 1; continue; }
            }
            if (isspace((unsigned char)*p)) {
                if (!last_was_space) {
                    buf_append_cstr(out, " ");
                    last_was_space = 1;
                }
                p++;
                continue;
            }
            buf_append(out, p, 1);
            last_was_space = 0;
            p++;
        } else {
            if (*p == '>') in_tag = 0;
            p++;
        }
    }
}

const char *getenv_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}
