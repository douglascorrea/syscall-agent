#ifndef CEZAR_TOOLS_META_H
#define CEZAR_TOOLS_META_H

#include "tools.h"

void tools_meta_register(cJSON *arr, int allow_delegates);
char *tools_meta_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
