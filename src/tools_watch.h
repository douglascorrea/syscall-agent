#ifndef LLA_TOOLS_WATCH_H
#define LLA_TOOLS_WATCH_H

#include "tools.h"
#include "../vendor/cJSON.h"

void  tools_watch_register(cJSON *arr);
char *tools_watch_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
