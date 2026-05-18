#ifndef CEZAR_HTTP_H
#define CEZAR_HTTP_H

#include <stddef.h>

typedef struct {
    long status;
    char *body;
    size_t body_len;
    char *error;
} HttpResponse;

typedef size_t (*HttpStreamWriteFn)(char *ptr, size_t size, size_t nmemb, void *userdata);

void http_global_init(void);
void http_global_cleanup(void);

/* Headers passed as NULL-terminated array of "Key: Value" strings (may be NULL). */
HttpResponse http_get(const char *url, const char *const *headers);
HttpResponse http_post_json(const char *url, const char *body, const char *const *headers);
HttpResponse http_post_form(const char *url, const char *body, const char *const *headers);
HttpResponse http_post_json_stream(const char *url,
                                   const char *body,
                                   const char *const *headers,
                                   HttpStreamWriteFn write_fn,
                                   void *userdata);

void http_response_free(HttpResponse *r);

#endif
