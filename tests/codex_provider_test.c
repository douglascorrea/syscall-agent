#include "codex_provider.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int saw_url = 0;
static int saw_model = 0;
static int saw_input = 0;
static int saw_tool = 0;
static int saw_auth_header = 0;
static int saw_account_header = 0;

HttpResponse http_post_json(const char *url, const char *body, const char *const *headers) {
    HttpResponse r = { .status = 200 };
    saw_url = url && strcmp(url, "https://chatgpt.com/backend-api/codex/responses") == 0;
    saw_model = body && strstr(body, "\"model\":\"codex-mini-latest\"") != NULL;
    saw_input = body && strstr(body, "\"input\"") != NULL &&
        strstr(body, "\"input_text\"") != NULL;
    saw_tool = body && strstr(body, "\"tools\"") != NULL &&
        strstr(body, "\"name\":\"test_tool\"") != NULL;
    for (size_t i = 0; headers && headers[i]; i++) {
        if (strcmp(headers[i], "Authorization: Bearer test-access-token") == 0) saw_auth_header = 1;
        if (strcmp(headers[i], "ChatGPT-Account-ID: acct_123") == 0) saw_account_header = 1;
    }
    r.body = strdup(
        "{"
        "\"output\":["
        "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hello\"}]},"
        "{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"test_tool\",\"arguments\":\"{\\\"ok\\\":true}\"}"
        "]"
        "}");
    r.body_len = strlen(r.body);
    return r;
}

void http_response_free(HttpResponse *r) {
    free(r->body);
    free(r->error);
    r->body = NULL;
    r->error = NULL;
}

static void expect_true(const char *name, int got) {
    if (!got) {
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

static void write_auth(void) {
    mkdir("build/codex-provider-home", 0777);
    FILE *f = fopen("build/codex-provider-home/auth.json", "wb");
    if (!f) {
        perror("auth.json");
        exit(1);
    }
    fputs(
        "{"
        "\"auth_mode\":\"chatgpt\","
        "\"OPENAI_API_KEY\":null,"
        "\"tokens\":{"
        "\"access_token\":\"test-access-token\","
        "\"account_id\":\"acct_123\""
        "}"
        "}",
        f);
    fclose(f);
}

static cJSON *make_messages(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", "system instructions");
    cJSON_AddItemToArray(messages, sys);
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", "hello?");
    cJSON_AddItemToArray(messages, user);
    return messages;
}

static cJSON *make_tools(void) {
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", "test_tool");
    cJSON_AddStringToObject(fn, "description", "test tool");
    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON_AddObjectToObject(params, "properties");
    cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(tools, tool);
    return tools;
}

int main(void) {
    write_auth();
    setenv("CODEX_HOME", "build/codex-provider-home", 1);

    CodexAuthInfo auth;
    char err[256] = {0};
    if (codex_auth_load(&auth, err, sizeof err) != 0) {
        fprintf(stderr, "codex_auth_load failed: %s\n", err);
        return 1;
    }
    expect_str("account", auth.account_id, "acct_123");
    codex_auth_info_free(&auth);

    cJSON *messages = make_messages();
    cJSON *tools = make_tools();
    cJSON *resp = codex_responses_chat("codex-mini-latest", messages, tools, 0);
    expect_true("url", saw_url);
    expect_true("model", saw_model);
    expect_true("input", saw_input);
    expect_true("tool", saw_tool);
    expect_true("auth header", saw_auth_header);
    expect_true("account header", saw_account_header);

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    expect_str("content", cJSON_GetStringValue(cJSON_GetObjectItem(message, "content")), "hello");
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
    expect_str("tool id", cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id")), "call_1");

    cJSON_Delete(resp);
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    return 0;
}
