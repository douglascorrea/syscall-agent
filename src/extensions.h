#ifndef CEZAR_EXTENSIONS_H
#define CEZAR_EXTENSIONS_H

#include "tools.h"

void extensions_register(cJSON *arr, const ToolCtx *ctx);
char *extensions_dispatch(ToolCtx *ctx, const char *name, cJSON *args);
char *extensions_list_text(void);

#endif
