#include "agent.h"
#include "openrouter.h"
#include "tools.h"
#include "tools_proc.h"
#include "memory.h"
#include "util.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct AgentSession {
    AgentConfig cfg;
    cJSON *messages;
    ToolCtx ctx;
    cJSON *tools;
};

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void emit_event(const AgentHooks *hooks,
                       AgentEventType type,
                       int step_index,
                       int max_steps,
                       const char *message,
                       const char *tool_name,
                       const char *tool_args,
                       long duration_ms) {
    if (!hooks || !hooks->on_event) return;
    AgentEvent event = {
        .type = type,
        .step_index = step_index,
        .max_steps = max_steps,
        .duration_ms = duration_ms,
        .message = message,
        .tool_name = tool_name,
        .tool_args = tool_args,
    };
    hooks->on_event(&event, hooks->userdata);
}

static cJSON *build_system_message(const AgentConfig *cfg) {
    size_t slen = 0, mlen = 0;
    char *sys = read_text_file(cfg->system_path, &slen);
    char *mem = memory_load(cfg->memory_path, &mlen);

    Buf b;
    buf_init(&b);
    if (sys && slen) {
        buf_append(&b, sys, slen);
    } else {
        buf_append_cstr(&b,
            "You are a helpful local AI agent. Use tools when useful. "
            "Be concise. When something is worth remembering across sessions, "
            "call save_memory.");
    }
    if (mem && mlen) {
        buf_append_cstr(&b,
            "\n\n---\n# Persistent Memory (from MEMORY.md)\n\n");
        buf_append(&b, mem, mlen);
    }
    free(sys);
    free(mem);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "system");
    cJSON_AddStringToObject(msg, "content", b.data ? b.data : "");
    buf_free(&b);
    return msg;
}

static cJSON *build_user_message(const char *prompt) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    return msg;
}

static void cli_on_event(const AgentEvent *event, void *userdata) {
    const AgentConfig *cfg = (const AgentConfig *)userdata;
    if (!event || !cfg) return;

    switch (event->type) {
    case AGENT_EVENT_STEP_START:
        if (cfg->verbose) {
            fprintf(stderr, "\n=== step %d/%d ===\n",
                event->step_index + 1, event->max_steps);
        }
        break;
    case AGENT_EVENT_TOOL_CALL_START:
        if (cfg->verbose) {
            fprintf(stderr, ">> tool: %s %s\n",
                event->tool_name ? event->tool_name : "(unknown)",
                event->tool_args ? event->tool_args : "{}");
        } else {
            fprintf(stderr, "  · %s\n",
                event->tool_name ? event->tool_name : "(unknown)");
        }
        break;
    case AGENT_EVENT_TOOL_CALL_RESULT:
        if (cfg->verbose && event->message) {
            size_t n = strlen(event->message);
            size_t show = n > 400 ? 400 : n;
            fprintf(stderr, "<< result (%zu bytes): %.*s%s\n",
                n, (int)show, event->message, n > show ? "..." : "");
        }
        break;
    case AGENT_EVENT_ERROR:
        fprintf(stderr, "agent: %s\n",
            event->message ? event->message : "unknown error");
        break;
    default:
        break;
    }
}

AgentSession *agent_session_new(const AgentConfig *cfg) {
    if (!cfg) return NULL;

    AgentSession *session = calloc(1, sizeof *session);
    if (!session) return NULL;
    session->cfg = *cfg;

    session->messages = cJSON_CreateArray();
    if (!session->messages) {
        free(session);
        return NULL;
    }
    cJSON_AddItemToArray(session->messages, build_system_message(cfg));

    session->ctx.memory_path = cfg->memory_path;
    session->ctx.allow_exec = cfg->allow_exec;
    session->ctx.allow_unsafe_exec = cfg->allow_unsafe_exec;
    session->ctx.bg = cfg->allow_exec ? bg_table_new() : NULL;

    session->tools = tools_describe(&session->ctx);
    if (!session->tools) {
        agent_session_free(session);
        return NULL;
    }

    return session;
}

int agent_session_run(AgentSession *session,
                      const char *user_prompt,
                      const AgentHooks *hooks,
                      char **out_final) {
    if (out_final) *out_final = NULL;
    if (!session || !user_prompt) return 1;

    cJSON_AddItemToArray(session->messages, build_user_message(user_prompt));

    for (int step = 0; step < session->cfg.max_steps; step++) {
        emit_event(hooks, AGENT_EVENT_STEP_START, step, session->cfg.max_steps,
            NULL, NULL, NULL, 0);
        emit_event(hooks, AGENT_EVENT_MODEL_REQUEST_START, step, session->cfg.max_steps,
            "Requesting model response", NULL, NULL, 0);

        char *api_err = NULL;
        cJSON *resp = openrouter_chat(session->cfg.api_key,
            session->cfg.model,
            session->messages,
            session->tools,
            &api_err);
        if (!resp) {
            emit_event(hooks, AGENT_EVENT_ERROR, step, session->cfg.max_steps,
                api_err ? api_err : "no response", NULL, NULL, 0);
            free(api_err);
            return 1;
        }
        free(api_err);

        cJSON *err = cJSON_GetObjectItem(resp, "error");
        if (err) {
            char *err_str = cJSON_PrintUnformatted(err);
            emit_event(hooks, AGENT_EVENT_ERROR, step, session->cfg.max_steps,
                err_str ? err_str : "(unknown API error)", NULL, NULL, 0);
            free(err_str);
            cJSON_Delete(resp);
            return 1;
        }

        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
        cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        if (!message) {
            char *raw = cJSON_PrintUnformatted(resp);
            Buf err_msg;
            buf_init(&err_msg);
            buf_printf(&err_msg, "malformed response: %.500s",
                raw ? raw : "(?)");
            emit_event(hooks, AGENT_EVENT_ERROR, step, session->cfg.max_steps,
                err_msg.data ? err_msg.data : "malformed response",
                NULL, NULL, 0);
            free(raw);
            buf_free(&err_msg);
            cJSON_Delete(resp);
            return 1;
        }

        cJSON *assistant = cJSON_Duplicate(message, 1);
        cJSON_AddItemToArray(session->messages, assistant);

        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        cJSON *content = cJSON_GetObjectItem(message, "content");

        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            int n = cJSON_GetArraySize(tool_calls);
            for (int i = 0; i < n; i++) {
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                cJSON *id = cJSON_GetObjectItem(tc, "id");
                cJSON *fn = cJSON_GetObjectItem(tc, "function");
                cJSON *name_j = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                cJSON *args_j = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
                const char *name = cJSON_IsString(name_j) ? name_j->valuestring : NULL;
                const char *args_str = cJSON_IsString(args_j) ? args_j->valuestring : NULL;

                cJSON *args_parsed = args_str ? cJSON_Parse(args_str) : NULL;
                emit_event(hooks, AGENT_EVENT_TOOL_CALL_START, step, session->cfg.max_steps,
                    NULL, name, args_str, 0);

                long t0 = now_ms();
                char *raw = tools_dispatch(&session->ctx, name, args_parsed);
                long dt = now_ms() - t0;

                char *result;
                if (raw && strstr(raw, "duration_ms=")) {
                    result = raw;
                } else {
                    Buf meta;
                    buf_init(&meta);
                    buf_printf(&meta, "[meta] duration_ms=%ld\n", dt);
                    if (raw) buf_append_cstr(&meta, raw);
                    free(raw);
                    result = meta.data;
                }

                emit_event(hooks, AGENT_EVENT_TOOL_CALL_RESULT, step, session->cfg.max_steps,
                    result, name, args_str, dt);

                cJSON *tool_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_msg, "role", "tool");
                if (id && cJSON_IsString(id)) {
                    cJSON_AddStringToObject(tool_msg, "tool_call_id", id->valuestring);
                }
                if (name) cJSON_AddStringToObject(tool_msg, "name", name);
                cJSON_AddStringToObject(tool_msg, "content", result ? result : "");
                cJSON_AddItemToArray(session->messages, tool_msg);

                free(result);
                if (args_parsed) cJSON_Delete(args_parsed);
            }
            cJSON_Delete(resp);
            continue;
        }

        if (cJSON_IsString(content) && content->valuestring) {
            if (out_final) *out_final = strdup(content->valuestring);
            emit_event(hooks, AGENT_EVENT_FINAL_RESPONSE, step, session->cfg.max_steps,
                content->valuestring, NULL, NULL, 0);
        } else {
            if (out_final) *out_final = strdup("(no content)");
            emit_event(hooks, AGENT_EVENT_FINAL_RESPONSE, step, session->cfg.max_steps,
                "(no content)", NULL, NULL, 0);
        }
        cJSON_Delete(resp);
        return 0;
    }

    {
        Buf err_msg;
        buf_init(&err_msg);
        buf_printf(&err_msg, "hit max_steps=%d without a final answer",
            session->cfg.max_steps);
        emit_event(hooks, AGENT_EVENT_ERROR, session->cfg.max_steps - 1,
            session->cfg.max_steps,
            err_msg.data ? err_msg.data : "hit max steps",
            NULL, NULL, 0);
        buf_free(&err_msg);
    }
    return 2;
}

void agent_session_free(AgentSession *session) {
    if (!session) return;
    cJSON_Delete(session->messages);
    cJSON_Delete(session->tools);
    if (session->ctx.bg) bg_table_free_kill_all(session->ctx.bg);
    free(session);
}

int agent_run(const AgentConfig *cfg, const char *user_prompt) {
    AgentSession *session = agent_session_new(cfg);
    if (!session) {
        fprintf(stderr, "agent: could not initialize session\n");
        return 1;
    }

    AgentHooks hooks = {
        .on_event = cli_on_event,
        .userdata = (void *)cfg,
    };
    char *final = NULL;
    int rc = agent_session_run(session, user_prompt, &hooks, &final);
    if (rc == 0) {
        printf("%s\n", final ? final : "(no content)");
    }

    free(final);
    agent_session_free(session);
    return rc;
}
