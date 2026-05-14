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

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
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

static void log_step_header(int step, int max, int verbose) {
    if (verbose) {
        fprintf(stderr, "\n=== step %d/%d ===\n", step + 1, max);
    }
}

static void log_tool_call(const char *name, const char *args_str, int verbose) {
    if (verbose) {
        fprintf(stderr, ">> tool: %s %s\n", name,
            args_str ? args_str : "{}");
    } else {
        fprintf(stderr, "  · %s\n", name);
    }
}

static void log_tool_result(const char *result, int verbose) {
    if (!verbose) return;
    size_t n = strlen(result);
    size_t show = n > 400 ? 400 : n;
    fprintf(stderr, "<< result (%zu bytes): %.*s%s\n",
        n, (int)show, result, n > show ? "..." : "");
}

int agent_run(const AgentConfig *cfg, const char *user_prompt) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, build_system_message(cfg));
    cJSON_AddItemToArray(messages, build_user_message(user_prompt));

    ToolCtx ctx = {
        .memory_path       = cfg->memory_path,
        .allow_exec        = cfg->allow_exec,
        .allow_unsafe_exec = cfg->allow_unsafe_exec,
        .bg                = cfg->allow_exec ? bg_table_new() : NULL,
    };
    cJSON *tools = tools_describe(&ctx);

    int rc = 0;

    for (int step = 0; step < cfg->max_steps; step++) {
        log_step_header(step, cfg->max_steps, cfg->verbose);

        cJSON *resp = openrouter_chat(cfg->api_key, cfg->model, messages, tools);
        if (!resp) {
            fprintf(stderr, "agent: no response\n");
            rc = 1;
            goto done;
        }

        cJSON *err = cJSON_GetObjectItem(resp, "error");
        if (err) {
            char *err_str = cJSON_PrintUnformatted(err);
            fprintf(stderr, "agent: API error: %s\n", err_str ? err_str : "(unknown)");
            free(err_str);
            cJSON_Delete(resp);
            rc = 1;
            goto done;
        }

        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
        cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        if (!message) {
            char *raw = cJSON_PrintUnformatted(resp);
            fprintf(stderr, "agent: malformed response: %.500s\n",
                raw ? raw : "(?)");
            free(raw);
            cJSON_Delete(resp);
            rc = 1;
            goto done;
        }

        /* Detach assistant message from response and append to history. */
        cJSON *assistant = cJSON_Duplicate(message, 1);
        cJSON_AddItemToArray(messages, assistant);

        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        cJSON *content    = cJSON_GetObjectItem(message, "content");

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
                log_tool_call(name ? name : "(unknown)", args_str, cfg->verbose);

                long t0 = now_ms();
                char *raw = tools_dispatch(&ctx, name, args_parsed);
                long dt = now_ms() - t0;

                /* Prefix every tool result with a one-line metadata header.
                 * Skips if the tool already emitted its own duration_ms (exec_command/spawn_bg). */
                char *result;
                if (raw && strstr(raw, "duration_ms=")) {
                    result = raw;
                } else {
                    Buf meta; buf_init(&meta);
                    buf_printf(&meta, "[meta] duration_ms=%ld\n", dt);
                    if (raw) buf_append_cstr(&meta, raw);
                    free(raw);
                    result = meta.data;
                }
                log_tool_result(result ? result : "", cfg->verbose);

                cJSON *tool_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_msg, "role", "tool");
                if (id && cJSON_IsString(id)) {
                    cJSON_AddStringToObject(tool_msg, "tool_call_id", id->valuestring);
                }
                if (name) cJSON_AddStringToObject(tool_msg, "name", name);
                cJSON_AddStringToObject(tool_msg, "content", result ? result : "");
                cJSON_AddItemToArray(messages, tool_msg);

                free(result);
                if (args_parsed) cJSON_Delete(args_parsed);
            }
            cJSON_Delete(resp);
            continue; /* go around the loop, let the model respond to tool output */
        }

        /* No tool calls — assistant gave a final text response. */
        if (cJSON_IsString(content) && content->valuestring) {
            printf("%s\n", content->valuestring);
        } else {
            printf("(no content)\n");
        }
        cJSON_Delete(resp);
        goto done;
    }

    fprintf(stderr, "agent: hit max_steps=%d without a final answer\n", cfg->max_steps);
    rc = 2;

done:
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    if (ctx.bg) bg_table_free_kill_all(ctx.bg);
    return rc;
}
