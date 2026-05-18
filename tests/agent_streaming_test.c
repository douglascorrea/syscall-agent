#include "agent.h"
#include "openrouter.h"
#include "tools.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int saw_assistant_delta = 0;
static int saw_reasoning_delta = 0;
static int saw_tool_delta = 0;
static int saw_tool_result = 0;
static int saw_final_before_delta = 0;
static int saw_any_delta = 0;

char *memory_load(const char *path, size_t *out_len) {
    (void)path;
    if (out_len) *out_len = 0;
    return NULL;
}

typedef struct BgTable BgTable;
BgTable *bg_table_new(void) {
    return NULL;
}

void bg_table_free_kill_all(BgTable *t) {
    (void)t;
}

cJSON *tools_describe(const ToolCtx *ctx) {
    (void)ctx;
    return cJSON_CreateArray();
}

char *tools_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    (void)ctx;
    (void)name;
    (void)args;
    return strdup("stream-tool-result");
}

cJSON *openrouter_chat(const char *api_key,
                       const char *model,
                       cJSON *messages,
                       cJSON *tools,
                       int want_reasoning) {
    (void)api_key;
    (void)model;
    (void)messages;
    (void)tools;
    (void)want_reasoning;
    return NULL;
}

static cJSON *make_stream_response_with_tool_call(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", "hello");

    cJSON *tool_calls = cJSON_CreateArray();
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "id", "call_stream");
    cJSON_AddStringToObject(call, "type", "function");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "list_dir");
    cJSON_AddStringToObject(fn, "arguments", "{}");
    cJSON_AddItemToObject(call, "function", fn);
    cJSON_AddItemToArray(tool_calls, call);
    cJSON_AddItemToObject(message, "tool_calls", tool_calls);

    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(root, "choices", choices);
    return root;
}

cJSON *openrouter_chat_stream(const char *api_key,
                              const char *model,
                              cJSON *messages,
                              cJSON *tools,
                              int want_reasoning,
                              OpenRouterStreamHandler handler,
                              void *userdata,
                              OpenRouterShouldCancelFn should_cancel,
                              void *cancel_userdata) {
    (void)api_key;
    (void)model;
    (void)messages;
    (void)tools;
    (void)should_cancel;
    (void)cancel_userdata;
    if (want_reasoning) {
        OpenRouterStreamEvent ev = {
            .type = OPENROUTER_STREAM_REASONING_DELTA,
            .content = "thinking",
            .tool_index = -1
        };
        handler(&ev, userdata);
    }
    OpenRouterStreamEvent text = {
        .type = OPENROUTER_STREAM_CONTENT_DELTA,
        .content = "hello",
        .tool_index = -1
    };
    handler(&text, userdata);

    OpenRouterStreamEvent tool = {
        .type = OPENROUTER_STREAM_TOOL_CALL_DELTA,
        .tool_index = 0,
        .tool_id = "call_stream",
        .tool_name = "list_dir",
        .tool_arguments_delta = "{}"
    };
    handler(&tool, userdata);
    return make_stream_response_with_tool_call();
}

static void record_event(const AgentEvent *event, void *userdata) {
    (void)userdata;
    if (event->type == AGENT_EVENT_ASSISTANT && !saw_any_delta) saw_final_before_delta = 1;
    if (event->type == AGENT_EVENT_ASSISTANT_DELTA &&
        event->content && strcmp(event->content, "hello") == 0) {
        saw_assistant_delta = 1;
        saw_any_delta = 1;
    }
    if (event->type == AGENT_EVENT_REASONING_DELTA &&
        event->content && strcmp(event->content, "thinking") == 0) {
        saw_reasoning_delta = 1;
        saw_any_delta = 1;
    }
    if (event->type == AGENT_EVENT_TOOL_CALL_DELTA &&
        event->content && strstr(event->content, "list_dir")) {
        saw_tool_delta = 1;
        saw_any_delta = 1;
    }
    if (event->type == AGENT_EVENT_TOOL_RESULT &&
        event->content && strstr(event->content, "stream-tool-result")) {
        saw_tool_result = 1;
    }
}

static void expect_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "missing expected streaming event: %s\n", name);
        exit(1);
    }
}

static void expect_false(const char *name, int value) {
    if (value) {
        fprintf(stderr, "unexpected streaming event: %s\n", name);
        exit(1);
    }
}

int main(void) {
    AgentConfig cfg = {
        .api_key = "test",
        .model = "test/model",
        .system_path = "/does/not/exist",
        .memory_path = "/does/not/exist",
        .max_steps = 1,
        .verbose = AGENT_VERBOSE_ALL,
        .stream = 1,
        .allow_exec = 0,
        .allow_unsafe_exec = 0,
    };

    int rc = agent_run_with_events(&cfg, "hello", record_event, NULL);
    if (rc != 2) {
        fprintf(stderr, "expected max_steps rc=2 after tool call, got %d\n", rc);
        return 1;
    }
    expect_true("assistant delta", saw_assistant_delta);
    expect_true("reasoning delta", saw_reasoning_delta);
    expect_true("tool call delta", saw_tool_delta);
    expect_true("tool result", saw_tool_result);
    expect_false("final before delta", saw_final_before_delta);
    return 0;
}
