#ifndef LLA_OPENROUTER_H
#define LLA_OPENROUTER_H

#include "../vendor/cJSON.h"

typedef enum {
    OPENROUTER_STREAM_CONTENT_DELTA,
    OPENROUTER_STREAM_REASONING_DELTA,
    OPENROUTER_STREAM_TOOL_CALL_DELTA,
    OPENROUTER_STREAM_ERROR
} OpenRouterStreamEventType;

typedef struct {
    OpenRouterStreamEventType type;
    const char *content;
    int tool_index;
    const char *tool_id;
    const char *tool_name;
    const char *tool_arguments_delta;
} OpenRouterStreamEvent;

typedef void (*OpenRouterStreamHandler)(const OpenRouterStreamEvent *event, void *userdata);

/* Sends a chat completion request to OpenRouter.
 *
 * api_key   — required; bearer token for OpenRouter.
 * model     — model id, e.g. "openai/gpt-4o-mini".
 * messages  — cJSON array (NOT owned; left intact).
 * tools     — cJSON array (NOT owned). May be NULL.
 *
 * Returns the parsed JSON response (caller owns), or NULL on transport error.
 * On HTTP error, the returned JSON has an "error" field; caller should check.
 */
cJSON *openrouter_chat(const char *api_key,
                       const char *model,
                       cJSON *messages,
                       cJSON *tools,
                       int want_reasoning);

cJSON *openrouter_chat_stream(const char *api_key,
                              const char *model,
                              cJSON *messages,
                              cJSON *tools,
                              int want_reasoning,
                              OpenRouterStreamHandler handler,
                              void *userdata);

#endif
