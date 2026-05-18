#ifndef CEZAR_TUI_H
#define CEZAR_TUI_H

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
    TUI_CMD_VERBOSE,
    TUI_CMD_TOOLS,
    TUI_CMD_SKILLS,
    TUI_CMD_AUTH,
    TUI_CMD_SYSINFO,
    TUI_CMD_LOGIN,
    TUI_CMD_SESSIONS,
    TUI_CMD_RESUME,
    TUI_CMD_RENAME,
    TUI_CMD_EXTENSIONS,
    TUI_CMD_STEER,
    TUI_CMD_SETTINGS,
    TUI_CMD_COMPACT
} TuiCommandType;

typedef struct {
    TuiCommandType type;
    int verbose_mode;
    int model_index;
    int percent;
    const char *model_id;
    const char *arg;
    const char *error;
} TuiCommand;

typedef struct {
    const char *id;
    const char *label;
} TuiModelChoice;

extern const TuiModelChoice TUI_MODELS[];
extern const size_t TUI_MODEL_COUNT;

int tui_parse_command(const char *line, TuiCommand *out);
int tui_history_apply(const char *const *items,
                      size_t len,
                      int direction,
                      size_t *cursor,
                      char *input,
                      size_t input_cap);
size_t tui_build_input_lines(const char *input, int width, char ***out_lines);
size_t tui_wrap_plain(const char *text, int width, char ***out_lines);
void tui_free_lines(char **lines, size_t count);
const char *tui_verbose_name(int mode);

int tui_run(AgentConfig *cfg);

#endif
