#include "openrouter.h"
#include "http.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cJSON *openrouter_chat(const char *api_key,
                       const char *model,
                       cJSON *messages,
                       cJSON *tools,
                       char **err_out) {
    if (err_out) *err_out = NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    /* Share the references — we'll Detach before deleting req to avoid double-free. */
    cJSON_AddItemReferenceToObject(req, "messages", messages);
    if (tools) {
        cJSON_AddItemReferenceToObject(req, "tools", tools);
        cJSON_AddStringToObject(req, "tool_choice", "auto");
    }

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    Buf auth;
    buf_init(&auth);
    buf_printf(&auth, "Authorization: Bearer %s", api_key);

    const char *headers[] = {
        auth.data,
        "HTTP-Referer: https://github.com/local/low_level_agent",
        "X-Title: low_level_agent",
        NULL
    };

    HttpResponse r = http_post_json(
        "https://openrouter.ai/api/v1/chat/completions",
        body, headers);

    free(body);
    buf_free(&auth);

    if (r.error) {
        if (err_out) {
            Buf err;
            buf_init(&err);
            buf_printf(&err, "transport error: %s", r.error);
            *err_out = err.data;
        }
        http_response_free(&r);
        return NULL;
    }

    cJSON *resp = r.body ? cJSON_Parse(r.body) : NULL;
    if (!resp) {
        if (err_out) {
            Buf err;
            buf_init(&err);
            buf_printf(&err, "invalid JSON response (HTTP %ld): %.500s",
                r.status, r.body ? r.body : "(empty)");
            *err_out = err.data;
        }
    }
    http_response_free(&r);
    return resp;
}
