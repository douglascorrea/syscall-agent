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
static int compaction_calls = 0;

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
    (void)tools;
    (void)want_reasoning;
    if (model && strcmp(model, "compact/model") == 0) {
        compaction_calls++;
        inspect_messages(messages, &(int){0}, &(int){0});
        return make_response("summary compacted");
    }
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
                              void *userdata,
                              OpenRouterShouldCancelFn should_cancel,
                              void *cancel_userdata) {
    (void)api_key;
    (void)model;
    (void)messages;
    (void)tools;
    (void)want_reasoning;
    (void)handler;
    (void)userdata;
    (void)should_cancel;
    (void)cancel_userdata;
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

static void expect_contains(const char *name, const char *got, const char *want) {
    if (!got || !strstr(got, want)) {
        fprintf(stderr, "%s: got '%s' expected substring '%s'\n",
            name, got ? got : "(null)", want);
        exit(1);
    }
}

static int always_cancel(void *userdata) {
    (void)userdata;
    return 1;
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

    AgentConfig cancel_cfg = cfg;
    cancel_cfg.should_cancel = always_cancel;
    int calls_before_cancel = chat_calls;
    expect_int("cancelled run rc",
        agent_conversation_run_with_events(conv, &cancel_cfg, "cancelled", ignore_event, NULL), 130);
    expect_int("cancelled run made no model call", chat_calls, calls_before_cancel);

    agent_conversation_free(conv);

    AgentConfig compact_cfg = cfg;
    compact_cfg.context_limit = 16;
    compact_cfg.compaction_percent = 50;
    compact_cfg.compaction_model = "compact/model";
    compact_cfg.compaction_prompt = "compact this transcript";
    AgentConversation *compact_conv = agent_conversation_new(&compact_cfg);
    expect_int("compact run one",
        agent_conversation_run_with_events(compact_conv, &compact_cfg,
            "first long message that should count toward context", ignore_event, NULL), 0);
    expect_int("compact run two",
        agent_conversation_run_with_events(compact_conv, &compact_cfg,
            "second long message that should count toward context", ignore_event, NULL), 0);
    expect_int("compact run three",
        agent_conversation_run_with_events(compact_conv, &compact_cfg,
            "third long message that triggers compaction", ignore_event, NULL), 0);
    if (compaction_calls < 1) {
        fprintf(stderr, "expected at least one compaction call\n");
        return 1;
    }
    char *compact_json = agent_conversation_to_json(compact_conv);
    expect_contains("compacted conversation summary", compact_json, "summary compacted");
    free(compact_json);
    agent_conversation_free(compact_conv);
    return 0;
}
