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

static void expect_not_contains(const char *name, const char *got, const char *bad) {
    if (got && strstr(got, bad)) {
        fprintf(stderr, "%s: got '%s', unexpected substring '%s'\n",
            name, got, bad);
        exit(1);
    }
}

static void test_list_tools_includes_meta_tools(void) {
    ToolCtx ctx = {0};
    char *result = tools_dispatch(&ctx, "list_tools", NULL);
    expect_contains("list_tools", result, "auth_status");
    expect_contains("list_tools", result, "system_info");
    expect_contains("list_tools", result, "list_skills");
    if (strstr(result, "delegate_codex")) {
        fprintf(stderr, "delegate tools should not be listed without allow_exec\n");
        exit(1);
    }
    free(result);
}

static void test_delegate_tools_are_gated(void) {
    ToolCtx ctx = { .allow_exec = 1, .allow_unsafe_exec = 0 };
    char *result = tools_dispatch(&ctx, "list_tools", NULL);
    expect_contains("delegate codex listed", result, "delegate_codex");
    expect_contains("delegate copilot listed", result, "delegate_copilot");
    expect_contains("auth login listed", result, "auth_login");
    free(result);

    cJSON *args = cJSON_Parse("{\"prompt\":\"hi\",\"mode\":\"workspace-write\"}");
    result = tools_dispatch(&ctx, "delegate_codex", args);
    expect_contains("delegate codex unsafe gate", result, "requires --allow-unsafe-exec");
    free(result);
    cJSON_Delete(args);
}

static void test_auth_login_dry_run_commands(void) {
    ToolCtx ctx = { .allow_exec = 1 };

    cJSON *args = cJSON_Parse("{\"provider\":\"codex\",\"dry_run\":true}");
    char *result = tools_dispatch(&ctx, "auth_login", args);
    expect_contains("codex login dry run", result, "codex --login");
    free(result);
    cJSON_Delete(args);

    args = cJSON_Parse("{\"provider\":\"copilot\",\"host\":\"example.ghe.com\",\"dry_run\":true}");
    result = tools_dispatch(&ctx, "auth_login", args);
    expect_contains("copilot login dry run", result, "copilot login --host example.ghe.com");
    free(result);
    cJSON_Delete(args);
}

static void test_auth_status_uses_cezar_branding(void) {
    setenv("CEZAR_AUTH_PROVIDER", "cezar-test", 1);
    setenv("CEZAR_PROVIDER", "codex", 1);

    char *result = tools_dispatch(NULL, "auth_status", NULL);
    expect_contains("auth status provider", result, "provider=cezar-test");
    expect_contains("auth status model provider", result, "model_provider: codex");
    expect_contains("auth status env", result, "CEZAR_PROVIDER=codex");
    expect_not_contains("auth status old env", result, "SYSCALL" "_AGENT");
    expect_not_contains("auth status old name", result, "syscall" "-agent");
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
    test_delegate_tools_are_gated();
    test_auth_login_dry_run_commands();
    test_auth_status_uses_cezar_branding();
    test_env_get_redacts_secret_values();
    test_file_digest_reports_algorithm();
    return 0;
}
