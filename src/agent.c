#include "agent.h"
#include "codex_provider.h"
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

static int use_codex_provider(const AgentConfig *cfg) {
    return cfg && cfg->provider &&
        (strcmp(cfg->provider, "codex") == 0 ||
         strcmp(cfg->provider, "openai-codex") == 0 ||
         strcmp(cfg->provider, "chatgpt") == 0);
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

struct AgentConversation {
    cJSON *messages;
};

static cJSON *build_initial_messages(const AgentConfig *cfg) {
    cJSON *messages = cJSON_CreateArray();
    if (!messages) return NULL;
    cJSON_AddItemToArray(messages, build_system_message(cfg));
    return messages;
}

AgentConversation *agent_conversation_new(const AgentConfig *cfg) {
    AgentConversation *conv = calloc(1, sizeof *conv);
    if (!conv) return NULL;
    conv->messages = build_initial_messages(cfg);
    if (!conv->messages) {
        free(conv);
        return NULL;
    }
    return conv;
}

void agent_conversation_reset(AgentConversation *conv, const AgentConfig *cfg) {
    if (!conv) return;
    cJSON_Delete(conv->messages);
    conv->messages = build_initial_messages(cfg);
}

void agent_conversation_free(AgentConversation *conv) {
    if (!conv) return;
    cJSON_Delete(conv->messages);
    free(conv);
}

char *agent_conversation_to_json(const AgentConversation *conv) {
    if (!conv || !conv->messages) return NULL;
    return cJSON_PrintUnformatted(conv->messages);
}

AgentConversation *agent_conversation_from_json(const AgentConfig *cfg,
                                                const char *json) {
    cJSON *messages = json ? cJSON_Parse(json) : NULL;
    if (!cJSON_IsArray(messages)) {
        if (messages) cJSON_Delete(messages);
        return agent_conversation_new(cfg);
    }
    AgentConversation *conv = calloc(1, sizeof *conv);
    if (!conv) {
        cJSON_Delete(messages);
        return NULL;
    }
    conv->messages = messages;
    return conv;
}

size_t agent_conversation_estimate_tokens(const AgentConversation *conv) {
    if (!conv || !conv->messages) return 0;
    char *json = cJSON_PrintUnformatted(conv->messages);
    if (!json) return 0;
    size_t chars = strlen(json);
    free(json);
    return chars / 4 + 1;
}

int agent_conversation_message_count(const AgentConversation *conv) {
    if (!conv || !conv->messages) return 0;
    return cJSON_GetArraySize(conv->messages);
}

static int compaction_context_limit(const AgentConfig *cfg) {
    return cfg && cfg->context_limit > 0 ? cfg->context_limit : 128000;
}

static int compaction_percent(const AgentConfig *cfg) {
    return cfg && cfg->compaction_percent >= 50 ? cfg->compaction_percent : 75;
}

static void emit_event(AgentEventHandler handler, void *userdata,
                       AgentEventType type, const char *title,
                       const char *content);

static char *build_compaction_transcript(cJSON *messages, int start, int end) {
    Buf b;
    buf_init(&b);
    for (int i = start; i < end; i++) {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        const char *role_s = cJSON_IsString(role) ? role->valuestring : "message";
        const char *content_s = cJSON_IsString(content) ? content->valuestring : "";
        buf_printf(&b, "%s: %s\n\n", role_s, content_s);
    }
    return b.data ? b.data : strdup("");
}

static char *extract_assistant_content(cJSON *resp) {
    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring && *content->valuestring) {
        return strdup(content->valuestring);
    }
    return NULL;
}

static char *compact_with_model(const AgentConfig *cfg, const char *transcript) {
    const char *prompt = cfg && cfg->compaction_prompt && *cfg->compaction_prompt
        ? cfg->compaction_prompt
        : "Summarize this conversation for later continuation.";
    const char *model = cfg && cfg->compaction_model && *cfg->compaction_model
        ? cfg->compaction_model
        : (cfg ? cfg->model : NULL);
    if (!model || !*model || !cfg) return NULL;

    cJSON *messages = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", prompt);
    cJSON_AddItemToArray(messages, sys);
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", transcript ? transcript : "");
    cJSON_AddItemToArray(messages, user);

    cJSON *resp = NULL;
    if (use_codex_provider(cfg)) {
        resp = codex_responses_chat(model, messages, NULL, 0);
    } else if (cfg->api_key && *cfg->api_key) {
        resp = openrouter_chat(cfg->api_key, model, messages, NULL, 0);
    }
    cJSON_Delete(messages);
    if (!resp) return NULL;
    char *summary = extract_assistant_content(resp);
    cJSON_Delete(resp);
    return summary;
}

static char *compact_locally(const char *transcript) {
    const char *safe = transcript ? transcript : "";
    size_t n = strlen(safe);
    size_t keep = n > 8000 ? 8000 : n;
    Buf b;
    buf_init(&b);
    buf_append_cstr(&b, "Local compaction summary of earlier conversation:\n");
    if (n > keep) buf_append_cstr(&b, "[truncated]\n");
    buf_append(&b, safe, keep);
    return b.data;
}

static int maybe_compact_conversation(AgentConversation *conv,
                                      const AgentConfig *cfg,
                                      AgentEventHandler handler,
                                      void *userdata) {
    if (!conv || !conv->messages) return 0;
    int count = cJSON_GetArraySize(conv->messages);
    if (count <= 4) return 0;
    int limit = compaction_context_limit(cfg);
    int pct = compaction_percent(cfg);
    size_t threshold = ((size_t)limit * (size_t)pct) / 100;
    if (threshold == 0 || agent_conversation_estimate_tokens(conv) < threshold) return 0;

    int keep_start = count - 4;
    if (keep_start < 1) return 0;
    char *transcript = build_compaction_transcript(conv->messages, 1, keep_start);
    char *summary = compact_with_model(cfg, transcript);
    if (!summary) summary = compact_locally(transcript);
    free(transcript);
    if (!summary) return -1;

    cJSON *next = cJSON_CreateArray();
    cJSON *system = cJSON_GetArrayItem(conv->messages, 0);
    if (system) cJSON_AddItemToArray(next, cJSON_Duplicate(system, 1));
    cJSON *summary_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(summary_msg, "role", "system");
    Buf content;
    buf_init(&content);
    buf_append_cstr(&content, "Compacted conversation summary:\n");
    buf_append_cstr(&content, summary);
    cJSON_AddStringToObject(summary_msg, "content", content.data ? content.data : summary);
    buf_free(&content);
    cJSON_AddItemToArray(next, summary_msg);
    for (int i = keep_start; i < count; i++) {
        cJSON *msg = cJSON_GetArrayItem(conv->messages, i);
        cJSON_AddItemToArray(next, cJSON_Duplicate(msg, 1));
    }
    cJSON_Delete(conv->messages);
    conv->messages = next;
    free(summary);
    emit_event(handler, userdata, AGENT_EVENT_STATUS, "agent", "conversation compacted");
    return 1;
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

static void emit_event(AgentEventHandler handler, void *userdata,
                       AgentEventType type, const char *title,
                       const char *content) {
    if (!handler) return;
    AgentEvent ev = {
        .type = type,
        .title = title,
        .content = content
    };
    handler(&ev, userdata);
}

static int should_cancel(const AgentConfig *cfg) {
    return cfg && cfg->should_cancel && cfg->should_cancel(cfg->cancel_userdata);
}

typedef struct {
    AgentEventHandler handler;
    void *userdata;
    const AgentConfig *cfg;
} AgentStreamForwarder;

static void on_openrouter_stream(const OpenRouterStreamEvent *event, void *userdata) {
    AgentStreamForwarder *f = userdata;
    if (!event || !f) return;
    switch (event->type) {
        case OPENROUTER_STREAM_CONTENT_DELTA:
            emit_event(f->handler, f->userdata, AGENT_EVENT_ASSISTANT_DELTA,
                "assistant", event->content ? event->content : "");
            break;
        case OPENROUTER_STREAM_REASONING_DELTA:
            if ((f->cfg->verbose & AGENT_VERBOSE_REASONING) != 0) {
                emit_event(f->handler, f->userdata, AGENT_EVENT_REASONING_DELTA,
                    "reasoning", event->content ? event->content : "");
            }
            break;
        case OPENROUTER_STREAM_TOOL_CALL_DELTA:
            if ((f->cfg->verbose & AGENT_VERBOSE_TOOLS) != 0) {
                Buf b;
                buf_init(&b);
                if (event->tool_name && *event->tool_name) {
                    buf_printf(&b, "%s", event->tool_name);
                } else {
                    buf_printf(&b, "tool[%d]", event->tool_index);
                }
                if (event->tool_arguments_delta && *event->tool_arguments_delta) {
                    buf_printf(&b, " %s", event->tool_arguments_delta);
                }
                emit_event(f->handler, f->userdata, AGENT_EVENT_TOOL_CALL_DELTA,
                    "tool call", b.data ? b.data : "");
                buf_free(&b);
            }
            break;
        case OPENROUTER_STREAM_ERROR:
            emit_event(f->handler, f->userdata, AGENT_EVENT_ERROR,
                "stream", event->content ? event->content : "stream error");
            break;
    }
}

static char *extract_reasoning_text(cJSON *message) {
    cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning");
    if (cJSON_IsString(reasoning) && reasoning->valuestring && *reasoning->valuestring) {
        return strdup(reasoning->valuestring);
    }

    cJSON *reasoning_content = cJSON_GetObjectItem(message, "reasoning_content");
    if (cJSON_IsString(reasoning_content) &&
        reasoning_content->valuestring &&
        *reasoning_content->valuestring) {
        return strdup(reasoning_content->valuestring);
    }

    cJSON *details = cJSON_GetObjectItem(message, "reasoning_details");
    if (cJSON_IsArray(details) && cJSON_GetArraySize(details) > 0) {
        Buf out;
        buf_init(&out);
        int n = cJSON_GetArraySize(details);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(details, i);
            cJSON *summary = cJSON_GetObjectItem(item, "summary");
            cJSON *text = cJSON_GetObjectItem(item, "text");
            cJSON *type = cJSON_GetObjectItem(item, "type");
            if (cJSON_IsString(summary) && summary->valuestring) {
                buf_append_cstr(&out, summary->valuestring);
                buf_append_cstr(&out, "\n");
            } else if (cJSON_IsString(text) && text->valuestring) {
                buf_append_cstr(&out, text->valuestring);
                buf_append_cstr(&out, "\n");
            } else if (cJSON_IsString(type) && type->valuestring) {
                buf_printf(&out, "[%s]\n", type->valuestring);
            }
        }
        if (out.len > 0) return out.data;
        buf_free(&out);

        return cJSON_PrintUnformatted(details);
    }

    return NULL;
}

static int agent_run_messages_with_events(const AgentConfig *cfg,
                                          cJSON *messages,
                                          AgentEventHandler handler,
                                          void *userdata) {
    ToolCtx ctx = {
        .memory_path       = cfg->memory_path,
        .allow_exec        = cfg->allow_exec,
        .allow_unsafe_exec = cfg->allow_unsafe_exec,
        .bg                = cfg->allow_exec ? bg_table_new() : NULL,
    };
    cJSON *tools = tools_describe(&ctx);

    int rc = 0;

    for (int step = 0; step < cfg->max_steps; step++) {
        if (should_cancel(cfg)) {
            emit_event(handler, userdata, AGENT_EVENT_ERROR, "agent", "cancelled");
            rc = 130;
            goto done;
        }
        if (!handler) log_step_header(step, cfg->max_steps, cfg->verbose);
        if (handler) {
            char status[64];
            snprintf(status, sizeof status, "step %d/%d", step + 1, cfg->max_steps);
            emit_event(handler, userdata, AGENT_EVENT_STATUS, "agent", status);
        }

        int use_stream = cfg->stream && handler;
        AgentStreamForwarder forwarder = {
            .handler = handler,
            .userdata = userdata,
            .cfg = cfg
        };
        cJSON *resp = use_codex_provider(cfg)
            ? codex_responses_chat(
                cfg->model,
                messages,
                tools,
                (cfg->verbose & AGENT_VERBOSE_REASONING) != 0)
            : use_stream
            ? openrouter_chat_stream(
                cfg->api_key,
                cfg->model,
                messages,
                tools,
                (cfg->verbose & AGENT_VERBOSE_REASONING) != 0,
                on_openrouter_stream,
                &forwarder,
                cfg->should_cancel,
                cfg->cancel_userdata)
            : openrouter_chat(
                cfg->api_key,
                cfg->model,
                messages,
                tools,
                (cfg->verbose & AGENT_VERBOSE_REASONING) != 0);
        if (!resp) {
            if (should_cancel(cfg)) {
                emit_event(handler, userdata, AGENT_EVENT_ERROR, "agent", "cancelled");
                rc = 130;
                goto done;
            }
            fprintf(stderr, "agent: no response\n");
            emit_event(handler, userdata, AGENT_EVENT_ERROR, "agent", "no response");
            rc = 1;
            goto done;
        }

        cJSON *err = cJSON_GetObjectItem(resp, "error");
        if (err) {
            char *err_str = cJSON_PrintUnformatted(err);
            if (!handler) {
                fprintf(stderr, "agent: API error: %s\n", err_str ? err_str : "(unknown)");
            }
            emit_event(handler, userdata, AGENT_EVENT_ERROR, "API error",
                err_str ? err_str : "(unknown)");
            free(err_str);
            cJSON_Delete(resp);
            rc = 1;
            goto done;
        }

        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
        if (should_cancel(cfg)) {
            cJSON_Delete(resp);
            emit_event(handler, userdata, AGENT_EVENT_ERROR, "agent", "cancelled");
            rc = 130;
            goto done;
        }
        cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        if (!message) {
            char *raw = cJSON_PrintUnformatted(resp);
            if (!handler) {
                fprintf(stderr, "agent: malformed response: %.500s\n",
                    raw ? raw : "(?)");
            }
            emit_event(handler, userdata, AGENT_EVENT_ERROR, "malformed response",
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

        if ((cfg->verbose & AGENT_VERBOSE_REASONING) != 0) {
            char *reasoning = extract_reasoning_text(message);
            if (reasoning && *reasoning) {
                emit_event(handler, userdata, AGENT_EVENT_REASONING, "reasoning", reasoning);
            }
            free(reasoning);
        }

        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            int n = cJSON_GetArraySize(tool_calls);
            for (int i = 0; i < n; i++) {
                if (should_cancel(cfg)) {
                    cJSON_Delete(resp);
                    emit_event(handler, userdata, AGENT_EVENT_ERROR, "agent", "cancelled");
                    rc = 130;
                    goto done;
                }
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                cJSON *id = cJSON_GetObjectItem(tc, "id");
                cJSON *fn = cJSON_GetObjectItem(tc, "function");
                cJSON *name_j = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                cJSON *args_j = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
                const char *name = cJSON_IsString(name_j) ? name_j->valuestring : NULL;
                const char *args_str = cJSON_IsString(args_j) ? args_j->valuestring : NULL;

                cJSON *args_parsed = args_str ? cJSON_Parse(args_str) : NULL;
                if (!handler) {
                    log_tool_call(name ? name : "(unknown)", args_str, cfg->verbose);
                } else if ((cfg->verbose & AGENT_VERBOSE_TOOLS) != 0) {
                    emit_event(handler, userdata, AGENT_EVENT_TOOL_CALL,
                        name ? name : "(unknown)", args_str ? args_str : "{}");
                }

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
                if (!handler) {
                    log_tool_result(result ? result : "", cfg->verbose);
                } else if ((cfg->verbose & AGENT_VERBOSE_TOOLS) != 0) {
                    emit_event(handler, userdata, AGENT_EVENT_TOOL_RESULT,
                        name ? name : "tool result", result ? result : "");
                }

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
        if (use_stream && cJSON_IsString(content) && content->valuestring && *content->valuestring) {
            /* Content deltas were already emitted as they arrived. */
        } else if (cJSON_IsString(content) && content->valuestring) {
            if (handler) {
                emit_event(handler, userdata, AGENT_EVENT_ASSISTANT,
                    "assistant", content->valuestring);
            } else {
                printf("%s\n", content->valuestring);
            }
        } else {
            if (handler) {
                emit_event(handler, userdata, AGENT_EVENT_ASSISTANT,
                    "assistant", "(no content)");
            } else {
                printf("(no content)\n");
            }
        }
        cJSON_Delete(resp);
        goto done;
    }

    if (!handler) {
        fprintf(stderr, "agent: hit max_steps=%d without a final answer\n", cfg->max_steps);
    }
    emit_event(handler, userdata, AGENT_EVENT_ERROR, "agent", "hit max_steps without a final answer");
    rc = 2;

done:
    cJSON_Delete(tools);
    if (ctx.bg) bg_table_free_kill_all(ctx.bg);
    return rc;
}

int agent_conversation_run_with_events(AgentConversation *conv,
                                       const AgentConfig *cfg,
                                       const char *user_prompt,
                                       AgentEventHandler handler,
                                       void *userdata) {
    if (!conv || !conv->messages) {
        emit_event(handler, userdata, AGENT_EVENT_ERROR,
            "agent", "conversation is not initialized");
        return 1;
    }
    cJSON_AddItemToArray(conv->messages, build_user_message(user_prompt));
    if (maybe_compact_conversation(conv, cfg, handler, userdata) < 0) {
        emit_event(handler, userdata, AGENT_EVENT_ERROR,
            "agent", "conversation compaction failed");
        return 1;
    }
    return agent_run_messages_with_events(cfg, conv->messages, handler, userdata);
}

int agent_run_with_events(const AgentConfig *cfg,
                          const char *user_prompt,
                          AgentEventHandler handler,
                          void *userdata) {
    AgentConversation *conv = agent_conversation_new(cfg);
    if (!conv) {
        emit_event(handler, userdata, AGENT_EVENT_ERROR,
            "agent", "could not initialize conversation");
        return 1;
    }
    int rc = agent_conversation_run_with_events(conv, cfg, user_prompt, handler, userdata);
    agent_conversation_free(conv);
    return rc;
}

int agent_run(const AgentConfig *cfg, const char *user_prompt) {
    return agent_run_with_events(cfg, user_prompt, NULL, NULL);
}
