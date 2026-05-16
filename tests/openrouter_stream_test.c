#include "openrouter.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int saw_stream_true = 0;
static int saw_content_delta = 0;
static int saw_reasoning_delta = 0;
static int saw_reasoning_detail_delta = 0;
static int saw_tool_delta = 0;

HttpResponse http_post_json(const char *url, const char *body, const char *const *headers) {
    (void)url;
    (void)body;
    (void)headers;
    HttpResponse r = {0};
    r.error = strdup("non-streaming HTTP should not be used in this test");
    return r;
}

HttpResponse http_post_json_stream(const char *url,
                                   const char *body,
                                   const char *const *headers,
                                   HttpStreamWriteFn write_fn,
                                   void *userdata) {
    (void)headers;
    HttpResponse r = { .status = 200 };
    if (!url || strcmp(url, "https://openrouter.ai/api/v1/chat/completions") != 0) {
        r.status = 500;
        return r;
    }
    saw_stream_true = body && strstr(body, "\"stream\":true") != NULL;

    const char *chunk1 =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"think \",\"content\":\"hi \","
        "\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"list_dir\",\"arguments\":\"{\"}}]}}]}\n\n";
    const char *chunk2 =
        "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\","
        "\"text\":\"detail\"}],\"content\":\"there\",\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    write_fn((char *)chunk1, 1, strlen(chunk1), userdata);
    write_fn((char *)chunk2, 1, strlen(chunk2), userdata);
    return r;
}

void http_response_free(HttpResponse *r) {
    free(r->body);
    free(r->error);
    r->body = NULL;
    r->error = NULL;
}

static void record_stream(const OpenRouterStreamEvent *event, void *userdata) {
    (void)userdata;
    if (event->type == OPENROUTER_STREAM_CONTENT_DELTA &&
        event->content && strcmp(event->content, "hi ") == 0) {
        saw_content_delta = 1;
    }
    if (event->type == OPENROUTER_STREAM_REASONING_DELTA &&
        event->content && strcmp(event->content, "think ") == 0) {
        saw_reasoning_delta = 1;
    }
    if (event->type == OPENROUTER_STREAM_REASONING_DELTA &&
        event->content && strcmp(event->content, "detail") == 0) {
        saw_reasoning_detail_delta = 1;
    }
    if (event->type == OPENROUTER_STREAM_TOOL_CALL_DELTA &&
        event->tool_name && strcmp(event->tool_name, "list_dir") == 0 &&
        event->tool_arguments_delta && strcmp(event->tool_arguments_delta, "{") == 0) {
        saw_tool_delta = 1;
    }
}

static void expect_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "missing expected value: %s\n", name);
        exit(1);
    }
}

static void expect_str(const char *name, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", name, got ? got : "(null)", want);
        exit(1);
    }
}

int main(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON *tools = cJSON_CreateArray();
    cJSON *resp = openrouter_chat_stream(
        "test-key", "test/model", messages, tools, 1, record_stream, NULL);
    if (!resp) {
        fprintf(stderr, "openrouter_chat_stream returned NULL\n");
        return 1;
    }

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    expect_str("content", cJSON_IsString(content) ? content->valuestring : NULL, "hi there");

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    cJSON *tool0 = cJSON_GetArrayItem(tool_calls, 0);
    cJSON *fn = cJSON_GetObjectItem(tool0, "function");
    cJSON *name = cJSON_GetObjectItem(fn, "name");
    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
    expect_str("tool name", cJSON_IsString(name) ? name->valuestring : NULL, "list_dir");
    expect_str("tool args", cJSON_IsString(args) ? args->valuestring : NULL, "{}");

    expect_true("stream true", saw_stream_true);
    expect_true("content delta", saw_content_delta);
    expect_true("reasoning delta", saw_reasoning_delta);
    expect_true("reasoning detail delta", saw_reasoning_detail_delta);
    expect_true("tool delta", saw_tool_delta);

    cJSON_Delete(resp);
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    return 0;
}
