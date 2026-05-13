#ifndef LLA_HTTP_H
#define LLA_HTTP_H

#include <stddef.h>

typedef struct {
    long status;
    char *body;
    size_t body_len;
    char *error;
} HttpResponse;

void http_global_init(void);
void http_global_cleanup(void);

/* Headers passed as NULL-terminated array of "Key: Value" strings (may be NULL). */
HttpResponse http_get(const char *url, const char *const *headers);
HttpResponse http_post_json(const char *url, const char *body, const char *const *headers);
HttpResponse http_post_form(const char *url, const char *body, const char *const *headers);

void http_response_free(HttpResponse *r);

#endif
