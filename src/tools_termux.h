#ifndef LLA_TOOLS_TERMUX_H
#define LLA_TOOLS_TERMUX_H

#include "tools.h"
#include "../vendor/cJSON.h"

void  tools_termux_register(cJSON *arr);
char *tools_termux_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
