#include "codex_provider.h"
#include "http.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t err_cap, const char *msg) {
    if (!err || err_cap == 0) return;
    snprintf(err, err_cap, "%s", msg ? msg : "Codex auth error");
}

static char *codex_auth_path(void) {
    const char *codex_home = getenv("CODEX_HOME");
    Buf b;
    buf_init(&b);
    if (codex_home && *codex_home) {
        buf_printf(&b, "%s/auth.json", codex_home);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        buf_printf(&b, "%s/.codex/auth.json", home);
    }
    return b.data;
}

int codex_auth_load(CodexAuthInfo *out, char *err, size_t err_cap) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);

    char *path = codex_auth_path();
    if (!path) {
        set_err(err, err_cap, "CODEX_HOME/HOME is not available");
        return -1;
    }
    size_t len = 0;
    char *text = read_text_file(path, &len);
    if (!text) {
        free(path);
        set_err(err, err_cap, "Codex auth file is missing; run /login codex or codex --login");
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        free(path);
        set_err(err, err_cap, "Codex auth file is not valid JSON");
        return -1;
    }

    cJSON *mode = cJSON_GetObjectItem(root, "auth_mode");
    cJSON *api_key = cJSON_GetObjectItem(root, "OPENAI_API_KEY");
    if (!cJSON_IsString(mode) || strcmp(mode->valuestring, "chatgpt") != 0 ||
        (cJSON_IsString(api_key) && api_key->valuestring && *api_key->valuestring)) {
        cJSON_Delete(root);
        free(path);
        set_err(err, err_cap, "Codex auth is not in ChatGPT subscription mode; run codex --login");
        return -1;
    }

    cJSON *tokens = cJSON_GetObjectItem(root, "tokens");
    cJSON *access = tokens ? cJSON_GetObjectItem(tokens, "access_token") : NULL;
    cJSON *account = tokens ? cJSON_GetObjectItem(tokens, "account_id") : NULL;
    if (!cJSON_IsString(access) || !access->valuestring || !*access->valuestring) {
        cJSON_Delete(root);
        free(path);
        set_err(err, err_cap, "Codex ChatGPT auth is missing an access token; run codex --login");
        return -1;
    }
    if (!cJSON_IsString(account) || !account->valuestring || !*account->valuestring) {
        cJSON_Delete(root);
        free(path);
        set_err(err, err_cap, "Codex ChatGPT auth is missing an account id; run codex --login");
        return -1;
    }

    out->access_token = strdup(access->valuestring);
    out->account_id = strdup(account->valuestring);
    out->auth_path = path;
    cJSON_Delete(root);
    if (!out->access_token || !out->account_id || !out->auth_path) {
        codex_auth_info_free(out);
        set_err(err, err_cap, "out of memory while loading Codex auth");
        return -1;
    }
    return 0;
}

void codex_auth_info_free(CodexAuthInfo *auth) {
    if (!auth) return;
    free(auth->access_token);
    free(auth->account_id);
    free(auth->auth_path);
    auth->access_token = NULL;
    auth->account_id = NULL;
    auth->auth_path = NULL;
}

static const char *json_string(cJSON *node) {
    return cJSON_IsString(node) && node->valuestring ? node->valuestring : "";
}

static void append_message_text(Buf *out, cJSON *content) {
    if (cJSON_IsString(content) && content->valuestring) {
        buf_append_cstr(out, content->valuestring);
        return;
    }
    if (cJSON_IsArray(content)) {
        int n = cJSON_GetArraySize(content);
        for (int i = 0; i < n; i++) {
            cJSON *part = cJSON_GetArrayItem(content, i);
            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                if (out->len) buf_append_cstr(out, "\n");
                buf_append_cstr(out, text->valuestring);
            }
        }
    }
}

static cJSON *message_item(const char *role, const char *type, const char *text) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "role", role);
    cJSON *content = cJSON_AddArrayToObject(item, "content");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", type);
    cJSON_AddStringToObject(part, "text", text ? text : "");
    cJSON_AddItemToArray(content, part);
    return item;
}

static cJSON *tool_output_item(cJSON *msg) {
    cJSON *item = cJSON_CreateObject();
    cJSON *id = cJSON_GetObjectItem(msg, "tool_call_id");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", json_string(id));
    cJSON_AddStringToObject(item, "output", json_string(cJSON_GetObjectItem(msg, "content")));
    return item;
}

static void append_assistant_tool_calls(cJSON *input, cJSON *tool_calls) {
    int n = cJSON_IsArray(tool_calls) ? cJSON_GetArraySize(tool_calls) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "function_call");
        cJSON_AddStringToObject(item, "call_id", json_string(cJSON_GetObjectItem(tc, "id")));
        cJSON_AddStringToObject(item, "name", json_string(cJSON_GetObjectItem(fn, "name")));
        cJSON_AddStringToObject(item, "arguments", json_string(cJSON_GetObjectItem(fn, "arguments")));
        cJSON_AddItemToArray(input, item);
    }
}

static void add_input_from_messages(cJSON *req, cJSON *messages) {
    cJSON *input = cJSON_AddArrayToObject(req, "input");
    int n = cJSON_IsArray(messages) ? cJSON_GetArraySize(messages) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        const char *role = json_string(cJSON_GetObjectItem(msg, "role"));
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (strcmp(role, "system") == 0) {
            continue;
        }
        if (strcmp(role, "tool") == 0) {
            cJSON_AddItemToArray(input, tool_output_item(msg));
            continue;
        }
        Buf text;
        buf_init(&text);
        append_message_text(&text, content);
        if (strcmp(role, "assistant") == 0) {
            if (text.len > 0) {
                cJSON_AddItemToArray(input, message_item("assistant", "output_text", text.data));
            }
            append_assistant_tool_calls(input, cJSON_GetObjectItem(msg, "tool_calls"));
        } else {
            cJSON_AddItemToArray(input, message_item("user", "input_text", text.data ? text.data : ""));
        }
        buf_free(&text);
    }
}

static char *system_instructions(cJSON *messages) {
    int n = cJSON_IsArray(messages) ? cJSON_GetArraySize(messages) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        if (strcmp(json_string(cJSON_GetObjectItem(msg, "role")), "system") != 0) continue;
        Buf text;
        buf_init(&text);
        append_message_text(&text, cJSON_GetObjectItem(msg, "content"));
        return text.data ? text.data : strdup("");
    }
    return strdup("");
}

static cJSON *responses_tools_from_chat_tools(cJSON *tools) {
    cJSON *out = cJSON_CreateArray();
    int n = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        cJSON *fn = cJSON_GetObjectItem(tool, "function");
        cJSON *name = cJSON_GetObjectItem(fn, "name");
        if (!cJSON_IsString(name) || !name->valuestring) continue;
        cJSON *rt = cJSON_CreateObject();
        cJSON_AddStringToObject(rt, "type", "function");
        cJSON_AddStringToObject(rt, "name", name->valuestring);
        cJSON *desc = cJSON_GetObjectItem(fn, "description");
        cJSON_AddStringToObject(rt, "description", json_string(desc));
        cJSON *params = cJSON_GetObjectItem(fn, "parameters");
        cJSON_AddItemToObject(rt, "parameters",
            cJSON_IsObject(params) ? cJSON_Duplicate(params, 1) : cJSON_CreateObject());
        cJSON_AddItemToArray(out, rt);
    }
    return out;
}

static char *make_request_body(const char *model, cJSON *messages, cJSON *tools, int want_reasoning) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model && *model ? model : "codex-mini-latest");
    cJSON_AddBoolToObject(req, "store", 0);
    cJSON_AddBoolToObject(req, "parallel_tool_calls", 0);
    char *instructions = system_instructions(messages);
    if (instructions && *instructions) cJSON_AddStringToObject(req, "instructions", instructions);
    free(instructions);
    add_input_from_messages(req, messages);
    if (tools) cJSON_AddItemToObject(req, "tools", responses_tools_from_chat_tools(tools));
    if (want_reasoning) {
        cJSON *reasoning = cJSON_AddObjectToObject(req, "reasoning");
        cJSON_AddStringToObject(reasoning, "summary", "auto");
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

static void append_response_message_text(Buf *content, cJSON *message_item) {
    cJSON *parts = cJSON_GetObjectItem(message_item, "content");
    int n = cJSON_IsArray(parts) ? cJSON_GetArraySize(parts) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *part = cJSON_GetArrayItem(parts, i);
        cJSON *type = cJSON_GetObjectItem(part, "type");
        if (!cJSON_IsString(type)) continue;
        if (strcmp(type->valuestring, "output_text") == 0 ||
            strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                buf_append_cstr(content, text->valuestring);
            }
        }
    }
}

static void add_tool_call(cJSON *tool_calls, cJSON *item) {
    cJSON *call = cJSON_CreateObject();
    cJSON_AddNumberToObject(call, "index", cJSON_GetArraySize(tool_calls));
    cJSON_AddStringToObject(call, "id", json_string(cJSON_GetObjectItem(item, "call_id")));
    cJSON_AddStringToObject(call, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(call, "function");
    cJSON_AddStringToObject(fn, "name", json_string(cJSON_GetObjectItem(item, "name")));
    cJSON_AddStringToObject(fn, "arguments", json_string(cJSON_GetObjectItem(item, "arguments")));
    cJSON_AddItemToArray(tool_calls, call);
}

static cJSON *chat_response_from_responses(cJSON *root) {
    Buf content;
    buf_init(&content);
    cJSON *tool_calls = cJSON_CreateArray();
    cJSON *output = cJSON_GetObjectItem(root, "output");
    int n = cJSON_IsArray(output) ? cJSON_GetArraySize(output) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(output, i);
        const char *type = json_string(cJSON_GetObjectItem(item, "type"));
        if (strcmp(type, "message") == 0) {
            append_response_message_text(&content, item);
        } else if (strcmp(type, "function_call") == 0) {
            add_tool_call(tool_calls, item);
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *choices = cJSON_AddArrayToObject(resp, "choices");
    cJSON *choice = cJSON_CreateObject();
    cJSON *msg = cJSON_AddObjectToObject(choice, "message");
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", content.data ? content.data : "");
    if (cJSON_GetArraySize(tool_calls) > 0) {
        cJSON_AddItemToObject(msg, "tool_calls", tool_calls);
    } else {
        cJSON_Delete(tool_calls);
    }
    cJSON_AddItemToArray(choices, choice);
    buf_free(&content);
    return resp;
}

cJSON *codex_responses_chat(const char *model,
                            cJSON *messages,
                            cJSON *tools,
                            int want_reasoning) {
    CodexAuthInfo auth;
    char err[256] = {0};
    if (codex_auth_load(&auth, err, sizeof err) != 0) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", err[0] ? err : "Codex auth unavailable");
        return root;
    }

    char *body = make_request_body(model, messages, tools, want_reasoning);
    Buf bearer, account;
    buf_init(&bearer);
    buf_init(&account);
    buf_printf(&bearer, "Authorization: Bearer %s", auth.access_token);
    buf_printf(&account, "ChatGPT-Account-ID: %s", auth.account_id);
    const char *headers[] = {
        bearer.data,
        account.data,
        "version: cezar",
        NULL
    };
    HttpResponse r = http_post_json(
        "https://chatgpt.com/backend-api/codex/responses",
        body,
        headers);
    free(body);
    buf_free(&bearer);
    buf_free(&account);
    codex_auth_info_free(&auth);

    if (r.error) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", r.error);
        http_response_free(&r);
        return root;
    }
    cJSON *raw = r.body ? cJSON_Parse(r.body) : NULL;
    if (!raw) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", "Codex response was not valid JSON");
        http_response_free(&r);
        return root;
    }
    if (r.status >= 400 || cJSON_GetObjectItem(raw, "error")) {
        http_response_free(&r);
        return raw;
    }
    cJSON *chat = chat_response_from_responses(raw);
    cJSON_Delete(raw);
    http_response_free(&r);
    return chat;
}
