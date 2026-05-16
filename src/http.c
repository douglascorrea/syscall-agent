#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_http_url(const char *url) {
    return url &&
           (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t add = size * nmemb;
    HttpResponse *r = (HttpResponse *)userdata;
    char *nb = realloc(r->body, r->body_len + add + 1);
    if (!nb) return 0;
    r->body = nb;
    memcpy(r->body + r->body_len, ptr, add);
    r->body_len += add;
    r->body[r->body_len] = '\0';
    return add;
}

void http_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup(void) {
    curl_global_cleanup();
}

static struct curl_slist *build_headers(const char *const *headers) {
    struct curl_slist *list = NULL;
    if (!headers) return NULL;
    for (size_t i = 0; headers[i]; i++) {
        list = curl_slist_append(list, headers[i]);
    }
    return list;
}

static void set_common_options(CURL *c) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    /* DuckDuckGo and other sites flag bot-shaped or unusual user-agents.
     * A bare "Mozilla/5.0" is consistently accepted. */
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
}

static HttpResponse perform(CURL *c) {
    HttpResponse r = {0};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    set_common_options(c);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        r.error = strdup(curl_easy_strerror(rc));
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}

HttpResponse http_get(const char *url, const char *const *headers) {
    HttpResponse r = {0};
    if (!is_http_url(url)) {
        r.error = strdup("unsupported URL scheme; only http:// and https:// are allowed");
        return r;
    }
    CURL *c = curl_easy_init();
    if (!c) { r.error = strdup("curl init failed"); return r; }
    struct curl_slist *list = build_headers(headers);
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    r = perform(c);
    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

HttpResponse http_post_json(const char *url, const char *body, const char *const *headers) {
    HttpResponse r = {0};
    if (!is_http_url(url)) {
        r.error = strdup("unsupported URL scheme; only http:// and https:// are allowed");
        return r;
    }
    CURL *c = curl_easy_init();
    if (!c) { r.error = strdup("curl init failed"); return r; }
    struct curl_slist *list = curl_slist_append(NULL, "Content-Type: application/json");
    if (headers) {
        for (size_t i = 0; headers[i]; i++) list = curl_slist_append(list, headers[i]);
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    r = perform(c);
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

HttpResponse http_post_form(const char *url, const char *body, const char *const *headers) {
    HttpResponse r = {0};
    if (!is_http_url(url)) {
        r.error = strdup("unsupported URL scheme; only http:// and https:// are allowed");
        return r;
    }
    CURL *c = curl_easy_init();
    if (!c) { r.error = strdup("curl init failed"); return r; }
    struct curl_slist *list = curl_slist_append(NULL,
        "Content-Type: application/x-www-form-urlencoded");
    if (headers) {
        for (size_t i = 0; headers[i]; i++) list = curl_slist_append(list, headers[i]);
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    r = perform(c);
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

HttpResponse http_post_json_stream(const char *url,
                                   const char *body,
                                   const char *const *headers,
                                   HttpStreamWriteFn write_fn,
                                   void *userdata) {
    HttpResponse r = {0};
    if (!is_http_url(url)) {
        r.error = strdup("unsupported URL scheme; only http:// and https:// are allowed");
        return r;
    }
    CURL *c = curl_easy_init();
    if (!c) { r.error = strdup("curl init failed"); return r; }
    struct curl_slist *list = curl_slist_append(NULL, "Content-Type: application/json");
    if (headers) {
        for (size_t i = 0; headers[i]; i++) list = curl_slist_append(list, headers[i]);
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_fn);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, userdata);
    set_common_options(c);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        r.error = strdup(curl_easy_strerror(rc));
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

void http_response_free(HttpResponse *r) {
    free(r->body);
    free(r->error);
    r->body = NULL;
    r->error = NULL;
    r->body_len = 0;
}
