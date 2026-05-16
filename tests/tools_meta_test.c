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

static void test_list_tools_includes_meta_tools(void) {
    ToolCtx ctx = {0};
    char *result = tools_dispatch(&ctx, "list_tools", NULL);
    expect_contains("list_tools", result, "auth_status");
    expect_contains("list_tools", result, "system_info");
    expect_contains("list_tools", result, "list_skills");
    free(result);
}

static void test_env_get_redacts_secret_values(void) {
    setenv("OPENROUTER_API_KEY", "should-not-print", 1);
    cJSON *args = cJSON_Parse("{\"name\":\"OPENROUTER_API_KEY\"}");
    char *result = tools_dispatch(NULL, "env_get", args);
    expect_contains("env_get redacted", result, "set (redacted)");
    if (strstr(result, "should-not-print")) {
        fprintf(stderr, "env_get leaked a secret value\n");
        exit(1);
    }
    free(result);
    cJSON_Delete(args);
}

static void test_file_digest_reports_algorithm(void) {
    cJSON *args = cJSON_Parse("{\"path\":\"SYSTEM_PROMPT.md\"}");
    char *result = tools_dispatch(NULL, "file_digest", args);
    expect_contains("file_digest", result, "algorithm: fnv1a64");
    free(result);
    cJSON_Delete(args);
}

int main(void) {
    test_list_tools_includes_meta_tools();
    test_env_get_redacts_secret_values();
    test_file_digest_reports_algorithm();
    return 0;
}
