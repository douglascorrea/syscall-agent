#include "os_compat.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int os_list_processes(ProcInfo *procs, int max) {
    (void)procs;
    (void)max;
    return 0;
}

int os_apply_sandbox(const char *profile) {
    (void)profile;
    return -1;
}

int os_watch_path(const char *path, int timeout_ms, WatchEvent *out) {
    (void)path;
    (void)timeout_ms;
    (void)out;
    return -1;
}

static void expect_contains(const char *name, const char *got, const char *want) {
    if (!got || !strstr(got, want)) {
        fprintf(stderr, "%s: got '%s', expected substring '%s'\n",
            name, got ? got : "(null)", want);
        exit(1);
    }
}

static void test_list_tools_includes_termux_tools(void) {
    ToolCtx ctx = {0};
    char *result = tools_dispatch(&ctx, "list_tools", NULL);
    expect_contains("termux_info listed", result, "termux_info");
    expect_contains("termux_api_status listed", result, "termux_api_status");
    expect_contains("termux_storage_status listed", result, "termux_storage_status");
    expect_contains("termux_battery_status listed", result, "termux_battery_status");
    expect_contains("termux_wifi_info listed", result, "termux_wifi_info");
    expect_contains("termux_clipboard_get listed", result, "termux_clipboard_get");
    expect_contains("termux_clipboard_set listed", result, "termux_clipboard_set");
    expect_contains("termux_notification listed", result, "termux_notification");
    expect_contains("termux_vibrate listed", result, "termux_vibrate");
    expect_contains("termux_wake_lock listed", result, "termux_wake_lock");
    free(result);
}

static void test_termux_status_tools_do_not_require_termux(void) {
    char *result = tools_dispatch(NULL, "termux_api_status", NULL);
    expect_contains("termux_api_status", result, "TERMUX_API_STATUS");
    free(result);

    result = tools_dispatch(NULL, "termux_storage_status", NULL);
    expect_contains("termux_storage_status", result, "TERMUX_STORAGE_STATUS");
    free(result);
}

static void test_termux_validates_side_effect_args(void) {
    cJSON *args = cJSON_Parse("{\"content\":\"body\"}");
    char *result = tools_dispatch(NULL, "termux_notification", args);
    expect_contains("termux_notification missing title", result, "missing required arg 'title'");
    free(result);
    cJSON_Delete(args);

    args = cJSON_Parse("{}");
    result = tools_dispatch(NULL, "termux_clipboard_set", args);
    expect_contains("termux_clipboard_set missing text", result, "missing required arg 'text'");
    free(result);
    cJSON_Delete(args);

    args = cJSON_Parse("{\"duration_ms\":0}");
    result = tools_dispatch(NULL, "termux_vibrate", args);
    expect_contains("termux_vibrate invalid duration", result, "duration_ms must be between 1 and 5000");
    free(result);
    cJSON_Delete(args);

    args = cJSON_Parse("{\"action\":\"status\"}");
    result = tools_dispatch(NULL, "termux_wake_lock", args);
    expect_contains("termux_wake_lock invalid action", result, "action must be 'lock' or 'unlock'");
    free(result);
    cJSON_Delete(args);
}

int main(void) {
    test_list_tools_includes_termux_tools();
    test_termux_status_tools_do_not_require_termux();
    test_termux_validates_side_effect_args();
    return 0;
}
