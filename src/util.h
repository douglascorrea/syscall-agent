#ifndef CEZAR_UTIL_H
#define CEZAR_UTIL_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_append(Buf *b, const char *s, size_t n);
void buf_append_cstr(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);

char *read_text_file(const char *path, size_t *out_len);
int   write_text_file(const char *path, const char *data, size_t len);
int   append_text_file(const char *path, const char *data, size_t len);
int   file_exists(const char *path);

char *url_encode(const char *s);
void  strip_html(const char *html, Buf *out);

const char *getenv_or(const char *name, const char *fallback);

#endif
