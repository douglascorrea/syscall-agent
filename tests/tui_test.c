#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int agent_run_with_events(const AgentConfig *cfg,
                          const char *user_prompt,
                          AgentEventHandler handler,
                          void *userdata) {
    (void)cfg;
    (void)user_prompt;
    (void)handler;
    (void)userdata;
    return 0;
}

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", name, got, want);
        exit(1);
    }
}

static void expect_str(const char *name, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", name, got ? got : "(null)", want);
        exit(1);
    }
}

static void test_verbose_command_accepts_required_modes(void) {
    TuiCommand cmd;

    expect_int("normal parse",
        tui_parse_command("/verbose normal", &cmd), 1);
    expect_int("normal type", cmd.type, TUI_CMD_VERBOSE);
    expect_int("normal mode", cmd.verbose_mode, TUI_VERBOSE_NORMAL);

    expect_int("tools parse",
        tui_parse_command("/verbose tools", &cmd), 1);
    expect_int("tools mode", cmd.verbose_mode, TUI_VERBOSE_TOOLS);

    expect_int("reasoning parse",
        tui_parse_command("/verbose reasoning", &cmd), 1);
    expect_int("reasoning mode", cmd.verbose_mode, TUI_VERBOSE_REASONING);

    expect_int("misspelled reasoning parse",
        tui_parse_command("/verbose reasioning", &cmd), 1);
    expect_int("misspelled reasoning mode", cmd.verbose_mode, TUI_VERBOSE_REASONING);

    expect_int("all parse",
        tui_parse_command("/verbose all", &cmd), 1);
    expect_int("all mode", cmd.verbose_mode, TUI_VERBOSE_ALL);
}

static void test_model_command_selects_predefined_models(void) {
    TuiCommand cmd;

    expect_int("model by index parse",
        tui_parse_command("/model 2", &cmd), 1);
    expect_int("model by index type", cmd.type, TUI_CMD_MODEL);
    expect_int("model index", cmd.model_index, 1);
    expect_str("model id", cmd.model_id, "anthropic/claude-3.5-sonnet");

    expect_int("model by id parse",
        tui_parse_command("/model google/gemini-2.5-flash", &cmd), 1);
    expect_int("model by id index", cmd.model_index, 2);
    expect_str("model by id", cmd.model_id, "google/gemini-2.5-flash");

    expect_int("model arbitrary openrouter id parse",
        tui_parse_command("/model meta-llama/llama-4.1", &cmd), 1);
    expect_int("model arbitrary type", cmd.type, TUI_CMD_MODEL);
    expect_int("model arbitrary index", cmd.model_index, -1);
    expect_str("model arbitrary id", cmd.model_id, "meta-llama/llama-4.1");
}

static void test_new_and_exit_commands(void) {
    TuiCommand cmd;

    expect_int("new parse", tui_parse_command("/new", &cmd), 1);
    expect_int("new type", cmd.type, TUI_CMD_NEW);

    expect_int("exit parse", tui_parse_command("/exit", &cmd), 1);
    expect_int("exit type", cmd.type, TUI_CMD_EXIT);
}

static void test_meta_commands(void) {
    TuiCommand cmd;

    expect_int("tools parse", tui_parse_command("/tools", &cmd), 1);
    expect_int("tools type", cmd.type, TUI_CMD_TOOLS);

    expect_int("skills parse", tui_parse_command("/skills", &cmd), 1);
    expect_int("skills type", cmd.type, TUI_CMD_SKILLS);

    expect_int("auth parse", tui_parse_command("/auth", &cmd), 1);
    expect_int("auth type", cmd.type, TUI_CMD_AUTH);

    expect_int("sysinfo parse", tui_parse_command("/sysinfo", &cmd), 1);
    expect_int("sysinfo type", cmd.type, TUI_CMD_SYSINFO);
}

static void test_wrapping_fits_narrow_widths(void) {
    char **lines = NULL;
    size_t count = tui_wrap_plain("abcdefgh ijkl", 5, &lines);
    expect_int("line count", (int)count, 3);
    expect_str("line 0", lines[0], "abcde");
    expect_str("line 1", lines[1], "fgh");
    expect_str("line 2", lines[2], "ijkl");
    tui_free_lines(lines, count);

    lines = NULL;
    count = tui_wrap_plain("one two three", 9, &lines);
    expect_int("word line count", (int)count, 2);
    expect_str("word line 0", lines[0], "one two");
    expect_str("word line 1", lines[1], "three");
    tui_free_lines(lines, count);
}

int main(void) {
    test_verbose_command_accepts_required_modes();
    test_model_command_selects_predefined_models();
    test_new_and_exit_commands();
    test_meta_commands();
    test_wrapping_fits_narrow_widths();
    return 0;
}
