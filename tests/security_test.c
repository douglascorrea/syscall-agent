#include "http.h"
#include "os_compat.h"
#include "tools_fs.h"
#include "tools_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void test_profile_none_requires_unsafe_flag(void) {
    ToolCtx ctx = {
        .allow_exec = 1,
        .allow_unsafe_exec = 0,
    };
    cJSON *args = cJSON_Parse("{\"argv\":[\"/bin/echo\",\"hi\"],\"profile\":\"none\"}");
    char *result = tools_proc_dispatch(&ctx, "exec_command", args);
    expect_contains("profile none", result, "requires --allow-unsafe-exec");
    free(result);
    cJSON_Delete(args);
}

static void test_http_rejects_non_http_schemes(void) {
    HttpResponse r = http_get("file:///etc/passwd", NULL);
    expect_contains("http scheme", r.error, "only http:// and https://");
    http_response_free(&r);
}

static void test_write_file_masks_special_mode_bits(void) {
    char path[] = "/tmp/syscall-agent-security-test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    close(fd);
    unlink(path);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", path);
    cJSON_AddStringToObject(args, "content", "ok\n");
    cJSON_AddNumberToObject(args, "mode", 04777);
    char *result = tools_fs_dispatch(NULL, "write_file", args);
    expect_contains("write file", result, "OK:");
    free(result);
    cJSON_Delete(args);

    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat");
        exit(1);
    }
    if ((st.st_mode & 07777) != 0777) {
        fprintf(stderr, "mode mask: got %04o want 0777\n", (unsigned)(st.st_mode & 07777));
        unlink(path);
        exit(1);
    }
    unlink(path);
}

int main(void) {
    test_profile_none_requires_unsafe_flag();
    test_http_rejects_non_http_schemes();
    test_write_file_masks_special_mode_bits();
    return 0;
}
