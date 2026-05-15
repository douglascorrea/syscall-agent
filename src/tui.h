#ifndef LLA_TUI_H
#define LLA_TUI_H

#include "agent.h"

#include <stddef.h>

#define TUI_VERBOSE_NORMAL    AGENT_VERBOSE_NORMAL
#define TUI_VERBOSE_TOOLS     AGENT_VERBOSE_TOOLS
#define TUI_VERBOSE_REASONING AGENT_VERBOSE_REASONING
#define TUI_VERBOSE_ALL       AGENT_VERBOSE_ALL

typedef enum {
    TUI_CMD_NONE,
    TUI_CMD_UNKNOWN,
    TUI_CMD_NEW,
    TUI_CMD_EXIT,
    TUI_CMD_HELP,
    TUI_CMD_MODELS,
    TUI_CMD_MODEL,
    TUI_CMD_VERBOSE
} TuiCommandType;

typedef struct {
    TuiCommandType type;
    int verbose_mode;
    int model_index;
    const char *model_id;
    const char *error;
} TuiCommand;

typedef struct {
    const char *id;
    const char *label;
} TuiModelChoice;

extern const TuiModelChoice TUI_MODELS[];
extern const size_t TUI_MODEL_COUNT;

int tui_parse_command(const char *line, TuiCommand *out);
size_t tui_wrap_plain(const char *text, int width, char ***out_lines);
void tui_free_lines(char **lines, size_t count);
const char *tui_verbose_name(int mode);

int tui_run(AgentConfig *cfg);

#endif
