#ifndef LLA_OPENROUTER_H
#define LLA_OPENROUTER_H

#include "../vendor/cJSON.h"

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

#endif
