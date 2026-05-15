#ifndef LLA_TOOLS_PROC_H
#define LLA_TOOLS_PROC_H

#include "tools.h"
#include "../vendor/cJSON.h"

/* Background process table (forward-declared; defined in tools_proc.c). */
typedef struct BgTable BgTable;

BgTable *bg_table_new(void);
void     bg_table_free_kill_all(BgTable *t);

void  tools_proc_register(cJSON *arr, int allow_exec);
char *tools_proc_dispatch(ToolCtx *ctx, const char *name, cJSON *args);

#endif
