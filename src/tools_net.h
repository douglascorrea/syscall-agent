#ifndef CEZAR_TOOLS_NET_H
#define CEZAR_TOOLS_NET_H

#include "tools.h"
#include "../vendor/cJSON.h"

void  tools_net_register(cJSON *arr);
char *tools_net_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
