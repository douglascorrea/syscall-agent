#ifndef LLA_TOOLS_H
#define LLA_TOOLS_H

#include "../vendor/cJSON.h"

typedef struct {
    const char *memory_path;
} ToolCtx;

/* Returns a cJSON array of OpenAI-style tool definitions. Caller frees. */
cJSON *tools_describe(void);

/* Execute a tool. args is a parsed cJSON object (may be NULL).
 * Returns a newly-allocated string (caller must free). Never returns NULL —
 * errors come back as plain text the model can read. */
char *tools_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
