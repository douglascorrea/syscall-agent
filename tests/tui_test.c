#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AgentConversation *agent_conversation_new(const AgentConfig *cfg) {
    (void)cfg;
    return (AgentConversation *)1;
}

void agent_conversation_reset(AgentConversation *conv, const AgentConfig *cfg) {
    (void)conv;
    (void)cfg;
}

void agent_conversation_free(AgentConversation *conv) {
    (void)conv;
}

char *agent_conversation_to_json(const AgentConversation *conv) {
    (void)conv;
    return strdup("[]");
}

AgentConversation *agent_conversation_from_json(const AgentConfig *cfg,
                                                const char *json) {
    (void)cfg;
    (void)json;
    return (AgentConversation *)1;
}

size_t agent_conversation_estimate_tokens(const AgentConversation *conv) {
    (void)conv;
    return 42;
}

int agent_conversation_message_count(const AgentConversation *conv) {
    (void)conv;
    return 3;
}

int agent_conversation_run_with_events(AgentConversation *conv,
                                       const AgentConfig *cfg,
                                       const char *user_prompt,
                                       AgentEventHandler handler,
                                       void *userdata) {
    (void)conv;
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

    expect_int("extensions parse", tui_parse_command("/extensions", &cmd), 1);
    expect_int("extensions type", cmd.type, TUI_CMD_EXTENSIONS);
}

static void test_auth_and_session_commands(void) {
    TuiCommand cmd;

    expect_int("login codex parse", tui_parse_command("/login codex", &cmd), 1);
    expect_int("login codex type", cmd.type, TUI_CMD_LOGIN);
    expect_str("login codex provider", cmd.arg, "codex");

    expect_int("login copilot host parse",
        tui_parse_command("/login copilot https://example.ghe.com", &cmd), 1);
    expect_int("login copilot type", cmd.type, TUI_CMD_LOGIN);
    expect_str("login copilot provider", cmd.arg, "copilot https://example.ghe.com");

    expect_int("resume parse",
        tui_parse_command("/resume 123e4567-e89b-12d3-a456-426614174000", &cmd), 1);
    expect_int("resume type", cmd.type, TUI_CMD_RESUME);
    expect_str("resume id", cmd.arg, "123e4567-e89b-12d3-a456-426614174000");

    expect_int("sessions parse", tui_parse_command("/sessions", &cmd), 1);
    expect_int("sessions type", cmd.type, TUI_CMD_SESSIONS);

    expect_int("rename parse", tui_parse_command("/rename important fix", &cmd), 1);
    expect_int("rename type", cmd.type, TUI_CMD_RENAME);
    expect_str("rename name", cmd.arg, "important fix");
}

static void test_runtime_control_commands(void) {
    TuiCommand cmd;

    expect_int("steer parse", tui_parse_command("/steer focus on src/tui.c", &cmd), 1);
    expect_int("steer type", cmd.type, TUI_CMD_STEER);
    expect_str("steer arg", cmd.arg, "focus on src/tui.c");

    expect_int("teer alias parse", tui_parse_command("/teer keep going", &cmd), 1);
    expect_int("teer alias type", cmd.type, TUI_CMD_STEER);
    expect_str("teer alias arg", cmd.arg, "keep going");

    expect_int("settings parse", tui_parse_command("/settings", &cmd), 1);
    expect_int("settings type", cmd.type, TUI_CMD_SETTINGS);
    expect_str("settings empty arg", cmd.arg ? cmd.arg : "", "");

    expect_int("settings arg parse",
        tui_parse_command("/settings statusline model off", &cmd), 1);
    expect_int("settings arg type", cmd.type, TUI_CMD_SETTINGS);
    expect_str("settings arg", cmd.arg, "statusline model off");

    expect_int("compact parse", tui_parse_command("/compact 50", &cmd), 1);
    expect_int("compact type", cmd.type, TUI_CMD_COMPACT);
    expect_int("compact percent", cmd.percent, 50);

    expect_int("compact invalid parse", tui_parse_command("/compact 49", &cmd), 1);
    expect_int("compact invalid type", cmd.type, TUI_CMD_UNKNOWN);
}

static void test_history_navigation_replays_previous_commands(void) {
    const char *items[] = {
        "/auth",
        "summarize src/main.c",
        "/sessions"
    };
    char input[128] = {0};
    size_t cursor = 3;

    expect_int("history up 1",
        tui_history_apply(items, 3, -1, &cursor, input, sizeof input), 1);
    expect_str("history input 1", input, "/sessions");

    expect_int("history up 2",
        tui_history_apply(items, 3, -1, &cursor, input, sizeof input), 1);
    expect_str("history input 2", input, "summarize src/main.c");

    expect_int("history down",
        tui_history_apply(items, 3, 1, &cursor, input, sizeof input), 1);
    expect_str("history input down", input, "/sessions");

    expect_int("history down blank",
        tui_history_apply(items, 3, 1, &cursor, input, sizeof input), 1);
    expect_str("history input blank", input, "");
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

static void test_multiline_input_wraps_for_display(void) {
    char **lines = NULL;
    size_t count = tui_build_input_lines("alpha beta\ngamma", 10, &lines);
    expect_int("input line count", (int)count, 3);
    expect_str("input line 0", lines[0], "› alpha");
    expect_str("input line 1", lines[1], "  beta");
    expect_str("input line 2", lines[2], "  gamma");
    tui_free_lines(lines, count);
}

int main(void) {
    test_verbose_command_accepts_required_modes();
    test_model_command_selects_predefined_models();
    test_new_and_exit_commands();
    test_meta_commands();
    test_auth_and_session_commands();
    test_runtime_control_commands();
    test_history_navigation_replays_previous_commands();
    test_wrapping_fits_narrow_widths();
    test_multiline_input_wraps_for_display();
    return 0;
}
