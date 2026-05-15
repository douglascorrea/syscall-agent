#include "agent.h"
#include "tools.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int chat_calls = 0;
static int saw_tool_call = 0;
static int saw_tool_result = 0;
static int saw_reasoning = 0;
static int saw_assistant = 0;

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
    return strdup("tool-result");
}

static cJSON *make_response(cJSON *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(root, "choices", choices);
    return root;
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
    chat_calls++;

    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    if (chat_calls == 1) {
        if (want_reasoning) {
            cJSON_AddStringToObject(message, "reasoning", "thinking text");
        }
        cJSON_AddNullToObject(message, "content");
        cJSON *tool_calls = cJSON_CreateArray();
        cJSON *call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "id", "call_1");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", "list_dir");
        cJSON_AddStringToObject(fn, "arguments", "{}");
        cJSON_AddItemToObject(call, "function", fn);
        cJSON_AddItemToArray(tool_calls, call);
        cJSON_AddItemToObject(message, "tool_calls", tool_calls);
    } else {
        cJSON_AddStringToObject(message, "content", "final answer");
    }
    return make_response(message);
}

static void record_event(const AgentEvent *event, void *userdata) {
    (void)userdata;
    if (event->type == AGENT_EVENT_TOOL_CALL &&
        event->content && strcmp(event->content, "{}") == 0) {
        saw_tool_call = 1;
    }
    if (event->type == AGENT_EVENT_TOOL_RESULT &&
        event->content && strstr(event->content, "tool-result")) {
        saw_tool_result = 1;
    }
    if (event->type == AGENT_EVENT_REASONING &&
        event->content && strstr(event->content, "thinking text")) {
        saw_reasoning = 1;
    }
    if (event->type == AGENT_EVENT_ASSISTANT &&
        event->content && strcmp(event->content, "final answer") == 0) {
        saw_assistant = 1;
    }
}

static void expect_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "missing expected event: %s\n", name);
        exit(1);
    }
}

int main(void) {
    AgentConfig cfg = {
        .api_key = "test",
        .model = "test/model",
        .system_path = "/does/not/exist",
        .memory_path = "/does/not/exist",
        .max_steps = 3,
        .verbose = AGENT_VERBOSE_ALL,
        .allow_exec = 0,
        .allow_unsafe_exec = 0,
    };

    int rc = agent_run_with_events(&cfg, "hello", record_event, NULL);
    if (rc != 0) {
        fprintf(stderr, "agent_run_with_events rc=%d\n", rc);
        return 1;
    }
    expect_true("tool call", saw_tool_call);
    expect_true("tool result", saw_tool_result);
    expect_true("reasoning", saw_reasoning);
    expect_true("assistant", saw_assistant);
    return 0;
}
