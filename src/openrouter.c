#include "openrouter.h"
#include "http.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *make_request_body(const char *model,
                               cJSON *messages,
                               cJSON *tools,
                               int want_reasoning,
                               int stream) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    if (stream) cJSON_AddBoolToObject(req, "stream", 1);
    if (want_reasoning) {
        cJSON *reasoning = cJSON_CreateObject();
        cJSON_AddNumberToObject(reasoning, "max_tokens", 2000);
        cJSON_AddItemToObject(req, "reasoning", reasoning);
    }
    /* Share the references — we'll Detach before deleting req to avoid double-free. */
    cJSON_AddItemReferenceToObject(req, "messages", messages);
    if (tools) {
        cJSON_AddItemReferenceToObject(req, "tools", tools);
        cJSON_AddStringToObject(req, "tool_choice", "auto");
    }

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

static void make_headers(Buf *auth, const char *api_key, const char **headers) {
    buf_init(auth);
    buf_printf(auth, "Authorization: Bearer %s", api_key);
    headers[0] = auth->data;
    headers[1] = "HTTP-Referer: https://github.com/douglascorrea/syscall-agent";
    headers[2] = "X-Title: syscall-agent";
    headers[3] = NULL;
}

cJSON *openrouter_chat(const char *api_key,
                       const char *model,
                       cJSON *messages,
                       cJSON *tools,
                       int want_reasoning) {
    char *body = make_request_body(model, messages, tools, want_reasoning, 0);
    Buf auth;
    const char *headers[4];
    make_headers(&auth, api_key, headers);

    HttpResponse r = http_post_json(
        "https://openrouter.ai/api/v1/chat/completions",
        body, headers);

    free(body);
    buf_free(&auth);

    if (r.error) {
        fprintf(stderr, "openrouter: transport error: %s\n", r.error);
        http_response_free(&r);
        return NULL;
    }

    cJSON *resp = r.body ? cJSON_Parse(r.body) : NULL;
    if (!resp) {
        fprintf(stderr, "openrouter: invalid JSON response (HTTP %ld): %.500s\n",
            r.status, r.body ? r.body : "(empty)");
    } else if (r.status >= 400) {
        fprintf(stderr, "openrouter: HTTP %ld\n", r.status);
        /* leave error info embedded in resp for caller to inspect */
    }
    http_response_free(&r);
    return resp;
}

typedef struct {
    Buf line;
    Buf content;
    Buf reasoning;
    cJSON *tool_calls;
    cJSON *reasoning_details;
    int error;
    char error_msg[512];
    OpenRouterStreamHandler handler;
    void *userdata;
} StreamState;

static void emit_stream(StreamState *st, const OpenRouterStreamEvent *event) {
    if (st->handler) st->handler(event, st->userdata);
}

static void emit_stream_text(StreamState *st, OpenRouterStreamEventType type, const char *text) {
    if (!text || !*text) return;
    OpenRouterStreamEvent ev = {
        .type = type,
        .content = text,
        .tool_index = -1
    };
    emit_stream(st, &ev);
}

static char *json_string_dup(cJSON *node) {
    if (cJSON_IsString(node) && node->valuestring) return strdup(node->valuestring);
    return strdup("");
}

static void replace_string(cJSON *obj, const char *key, const char *value) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value ? value : "");
}

static void append_string_prop(cJSON *obj, const char *key, const char *chunk) {
    if (!chunk || !*chunk) return;
    cJSON *cur = cJSON_GetObjectItem(obj, key);
    const char *old = cJSON_IsString(cur) && cur->valuestring ? cur->valuestring : "";
    size_t old_len = strlen(old);
    size_t add_len = strlen(chunk);
    char *next = malloc(old_len + add_len + 1);
    if (!next) return;
    memcpy(next, old, old_len);
    memcpy(next + old_len, chunk, add_len + 1);
    replace_string(obj, key, next);
    free(next);
}

static cJSON *ensure_tool_call(StreamState *st, int index) {
    if (index < 0) index = 0;
    while (cJSON_GetArraySize(st->tool_calls) <= index) {
        cJSON *call = cJSON_CreateObject();
        cJSON_AddNumberToObject(call, "index", cJSON_GetArraySize(st->tool_calls));
        cJSON_AddStringToObject(call, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", "");
        cJSON_AddStringToObject(fn, "arguments", "");
        cJSON_AddItemToObject(call, "function", fn);
        cJSON_AddItemToArray(st->tool_calls, call);
    }
    return cJSON_GetArrayItem(st->tool_calls, index);
}

static void append_reasoning_detail_text(StreamState *st, cJSON *detail) {
    cJSON *summary = cJSON_GetObjectItem(detail, "summary");
    cJSON *text = cJSON_GetObjectItem(detail, "text");
    cJSON *type = cJSON_GetObjectItem(detail, "type");
    if (cJSON_IsString(summary) && summary->valuestring) {
        emit_stream_text(st, OPENROUTER_STREAM_REASONING_DELTA, summary->valuestring);
    } else if (cJSON_IsString(text) && text->valuestring) {
        emit_stream_text(st, OPENROUTER_STREAM_REASONING_DELTA, text->valuestring);
    } else if (cJSON_IsString(type) && type->valuestring) {
        emit_stream_text(st, OPENROUTER_STREAM_REASONING_DELTA, type->valuestring);
    }
}

static void process_delta(StreamState *st, cJSON *delta) {
    cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (cJSON_IsString(content) && content->valuestring && *content->valuestring) {
        buf_append_cstr(&st->content, content->valuestring);
        emit_stream_text(st, OPENROUTER_STREAM_CONTENT_DELTA, content->valuestring);
    }

    cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning");
    if (!cJSON_IsString(reasoning)) reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
    if (cJSON_IsString(reasoning) && reasoning->valuestring && *reasoning->valuestring) {
        buf_append_cstr(&st->reasoning, reasoning->valuestring);
        emit_stream_text(st, OPENROUTER_STREAM_REASONING_DELTA, reasoning->valuestring);
    }

    cJSON *details = cJSON_GetObjectItem(delta, "reasoning_details");
    if (cJSON_IsArray(details)) {
        cJSON *detail = NULL;
        cJSON_ArrayForEach(detail, details) {
            cJSON_AddItemToArray(st->reasoning_details, cJSON_Duplicate(detail, 1));
            append_reasoning_detail_text(st, detail);
        }
    }

    cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
    if (cJSON_IsArray(tool_calls)) {
        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tool_calls) {
            cJSON *idx_j = cJSON_GetObjectItem(tc, "index");
            int idx = cJSON_IsNumber(idx_j) ? (int)idx_j->valueint : 0;
            cJSON *dst = ensure_tool_call(st, idx);
            cJSON *id = cJSON_GetObjectItem(tc, "id");
            if (cJSON_IsString(id) && id->valuestring && *id->valuestring) {
                replace_string(dst, "id", id->valuestring);
            }
            cJSON *type = cJSON_GetObjectItem(tc, "type");
            if (cJSON_IsString(type) && type->valuestring && *type->valuestring) {
                replace_string(dst, "type", type->valuestring);
            }
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *dst_fn = cJSON_GetObjectItem(dst, "function");
            cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            char *name_chunk = json_string_dup(name);
            char *args_chunk = json_string_dup(args);
            if (name_chunk && *name_chunk) append_string_prop(dst_fn, "name", name_chunk);
            if (args_chunk && *args_chunk) append_string_prop(dst_fn, "arguments", args_chunk);

            cJSON *full_id = cJSON_GetObjectItem(dst, "id");
            cJSON *full_name = cJSON_GetObjectItem(dst_fn, "name");
            OpenRouterStreamEvent ev = {
                .type = OPENROUTER_STREAM_TOOL_CALL_DELTA,
                .content = NULL,
                .tool_index = idx,
                .tool_id = cJSON_IsString(full_id) ? full_id->valuestring : NULL,
                .tool_name = cJSON_IsString(full_name) ? full_name->valuestring : NULL,
                .tool_arguments_delta = args_chunk && *args_chunk ? args_chunk : NULL
            };
            if ((name_chunk && *name_chunk) || (args_chunk && *args_chunk)) {
                emit_stream(st, &ev);
            }
            free(name_chunk);
            free(args_chunk);
        }
    }
}

static void process_sse_payload(StreamState *st, const char *payload) {
    while (*payload == ' ') payload++;
    if (strcmp(payload, "[DONE]") == 0) return;

    cJSON *root = cJSON_Parse(payload);
    if (!root) return;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        snprintf(st->error_msg, sizeof st->error_msg, "%s",
            cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "stream error");
        st->error = 1;
        OpenRouterStreamEvent ev = {
            .type = OPENROUTER_STREAM_ERROR,
            .content = st->error_msg,
            .tool_index = -1
        };
        emit_stream(st, &ev);
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice0 ? cJSON_GetObjectItem(choice0, "delta") : NULL;
    if (delta) process_delta(st, delta);
    cJSON_Delete(root);
}

static void process_sse_line(StreamState *st, const char *line) {
    if (strncmp(line, "data:", 5) == 0) {
        process_sse_payload(st, line + 5);
    }
}

static size_t stream_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    StreamState *st = userdata;
    size_t n = size * nmemb;
    for (size_t i = 0; i < n; i++) {
        char ch = ptr[i];
        if (ch == '\n') {
            size_t len = st->line.len;
            if (len > 0 && st->line.data[len - 1] == '\r') {
                st->line.data[len - 1] = '\0';
                st->line.len--;
            }
            if (st->line.len > 0) process_sse_line(st, st->line.data);
            st->line.len = 0;
            if (st->line.data) st->line.data[0] = '\0';
        } else {
            buf_append(&st->line, &ch, 1);
        }
    }
    return n;
}

static cJSON *stream_build_response(StreamState *st) {
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", st->content.data ? st->content.data : "");
    if (st->reasoning.len > 0) {
        cJSON_AddStringToObject(message, "reasoning", st->reasoning.data);
    }
    if (cJSON_GetArraySize(st->reasoning_details) > 0) {
        cJSON_AddItemToObject(message, "reasoning_details", st->reasoning_details);
        st->reasoning_details = cJSON_CreateArray();
    }
    if (cJSON_GetArraySize(st->tool_calls) > 0) {
        cJSON_AddItemToObject(message, "tool_calls", st->tool_calls);
        st->tool_calls = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(root, "choices", choices);
    if (st->error) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "message", st->error_msg);
        cJSON_AddItemToObject(root, "error", err);
    }
    return root;
}

cJSON *openrouter_chat_stream(const char *api_key,
                              const char *model,
                              cJSON *messages,
                              cJSON *tools,
                              int want_reasoning,
                              OpenRouterStreamHandler handler,
                              void *userdata) {
    char *body = make_request_body(model, messages, tools, want_reasoning, 1);
    Buf auth;
    const char *headers[4];
    make_headers(&auth, api_key, headers);

    StreamState st = {0};
    buf_init(&st.line);
    buf_init(&st.content);
    buf_init(&st.reasoning);
    st.tool_calls = cJSON_CreateArray();
    st.reasoning_details = cJSON_CreateArray();
    st.handler = handler;
    st.userdata = userdata;

    HttpResponse r = http_post_json_stream(
        "https://openrouter.ai/api/v1/chat/completions",
        body, headers, stream_write_cb, &st);

    free(body);
    buf_free(&auth);

    if (r.error) {
        fprintf(stderr, "openrouter: stream transport error: %s\n", r.error);
        http_response_free(&r);
        cJSON_Delete(st.tool_calls);
        cJSON_Delete(st.reasoning_details);
        buf_free(&st.line);
        buf_free(&st.content);
        buf_free(&st.reasoning);
        return NULL;
    }
    if (r.status >= 400) {
        fprintf(stderr, "openrouter: stream HTTP %ld\n", r.status);
        st.error = 1;
        snprintf(st.error_msg, sizeof st.error_msg, "stream HTTP %ld", r.status);
    }
    http_response_free(&r);

    cJSON *resp = stream_build_response(&st);
    cJSON_Delete(st.tool_calls);
    cJSON_Delete(st.reasoning_details);
    buf_free(&st.line);
    buf_free(&st.content);
    buf_free(&st.reasoning);
    return resp;
}
