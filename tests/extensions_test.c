#include "os_compat.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static void write_manifest(void) {
    mkdir("build/test-extensions", 0777);
    mkdir("build/test-extensions/demo", 0777);
    FILE *f = fopen("build/test-extensions/demo/extension.json", "wb");
    if (!f) {
        perror("extension.json");
        exit(1);
    }
    fputs(
        "{\n"
        "  \"name\": \"demo\",\n"
        "  \"description\": \"Demo extension\",\n"
        "  \"tools\": [\n"
        "    {\n"
        "      \"name\": \"ext_echo\",\n"
        "      \"description\": \"Echo from an extension\",\n"
        "      \"command\": [\"/bin/echo\", \"extension-ok\"],\n"
        "      \"parameters\": {\"type\":\"object\",\"properties\":{},\"required\":[]}\n"
        "    }\n"
        "  ]\n"
        "}\n",
        f);
    fclose(f);
}

static void test_extension_tools_are_discovered_and_dispatched(void) {
    write_manifest();
    setenv("CEZAR_EXTENSIONS_DIR", "build/test-extensions", 1);

    ToolCtx ctx = { .allow_exec = 1 };
    char *result = tools_dispatch(&ctx, "list_tools", NULL);
    expect_contains("list tools extension", result, "ext_echo");
    free(result);

    result = tools_dispatch(&ctx, "list_extensions", NULL);
    expect_contains("list extensions", result, "demo");
    expect_contains("list extension tool", result, "ext_echo");
    free(result);

    result = tools_dispatch(&ctx, "ext_echo", NULL);
    expect_contains("extension dispatch", result, "extension-ok");
    free(result);
}

int main(void) {
    test_extension_tools_are_discovered_and_dispatched();
    return 0;
}
