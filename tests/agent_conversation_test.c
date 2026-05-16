#include "agent.h"
#include "openrouter.h"
#include "tools.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int chat_calls = 0;
static int first_turn_user_count = 0;
static int second_turn_user_count = 0;
static int second_turn_saw_first_assistant = 0;
static int after_reset_user_count = 0;

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

static cJSON *make_response(const char *content) {
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", content);
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(root, "choices", choices);
    return root;
}

static void inspect_messages(cJSON *messages, int *user_count, int *saw_assistant) {
    int n = cJSON_GetArraySize(messages);
    for (int i = 0; i < n; i++) {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
            (*user_count)++;
        }
        if (cJSON_IsString(role) && strcmp(role->valuestring, "assistant") == 0 &&
            cJSON_IsString(content) && strcmp(content->valuestring, "reply one") == 0) {
            *saw_assistant = 1;
        }
    }
}

cJSON *openrouter_chat(const char *api_key,
                       const char *model,
                       cJSON *messages,
                       cJSON *tools,
                       int want_reasoning) {
    (void)api_key;
    (void)model;
    (void)tools;
    (void)want_reasoning;
    chat_calls++;
    int saw = 0;
    if (chat_calls == 1) {
        inspect_messages(messages, &first_turn_user_count, &saw);
        return make_response("reply one");
    }
    if (chat_calls == 2) {
        inspect_messages(messages, &second_turn_user_count, &second_turn_saw_first_assistant);
        return make_response("reply two");
    }
    inspect_messages(messages, &after_reset_user_count, &saw);
    return make_response("reply reset");
}

cJSON *openrouter_chat_stream(const char *api_key,
                              const char *model,
                              cJSON *messages,
                              cJSON *tools,
                              int want_reasoning,
                              OpenRouterStreamHandler handler,
                              void *userdata) {
    (void)api_key;
    (void)model;
    (void)messages;
    (void)tools;
    (void)want_reasoning;
    (void)handler;
    (void)userdata;
    return NULL;
}

static void ignore_event(const AgentEvent *event, void *userdata) {
    (void)event;
    (void)userdata;
}

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", name, got, want);
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
        .verbose = AGENT_VERBOSE_NORMAL,
        .allow_exec = 0,
        .allow_unsafe_exec = 0,
    };

    AgentConversation *conv = agent_conversation_new(&cfg);
    if (!conv) {
        fprintf(stderr, "agent_conversation_new returned NULL\n");
        return 1;
    }

    expect_int("first run rc",
        agent_conversation_run_with_events(conv, &cfg, "first", ignore_event, NULL), 0);
    expect_int("first turn users", first_turn_user_count, 1);

    expect_int("second run rc",
        agent_conversation_run_with_events(conv, &cfg, "second", ignore_event, NULL), 0);
    expect_int("second turn users", second_turn_user_count, 2);
    expect_int("second turn saw first assistant", second_turn_saw_first_assistant, 1);

    agent_conversation_reset(conv, &cfg);
    expect_int("after reset run rc",
        agent_conversation_run_with_events(conv, &cfg, "after reset", ignore_event, NULL), 0);
    expect_int("after reset users", after_reset_user_count, 1);

    agent_conversation_free(conv);
    return 0;
}
