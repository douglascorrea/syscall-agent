#ifndef LLA_TOOLS_H
#define LLA_TOOLS_H

#include "../vendor/cJSON.h"

/* Forward-declared in tools_proc.h. */
typedef struct BgTable BgTable;

typedef struct {
    const char *memory_path;
    int allow_exec;
    int allow_unsafe_exec;
    BgTable *bg;
} ToolCtx;

/* Returns a cJSON array of OpenAI-style tool definitions. Caller frees. */
cJSON *tools_describe(const ToolCtx *ctx);

/* Execute a tool. args is a parsed cJSON object (may be NULL).
 * Returns a newly-allocated string (caller must free). Never returns NULL —
 * errors come back as plain text the model can read. */
char *tools_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
