#ifndef CEZAR_TOOLS_FS_H
#define CEZAR_TOOLS_FS_H

#include "tools.h"
#include "../vendor/cJSON.h"

void  tools_fs_register(cJSON *arr);

/* Returns a newly-allocated result string on hit, NULL if it did not handle
 * the tool name. Caller frees on hit. */
char *tools_fs_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
