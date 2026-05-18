#include "session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", name, got, want);
        exit(1);
    }
}

static void expect_contains(const char *name, const char *got, const char *want) {
    if (!got || !strstr(got, want)) {
        fprintf(stderr, "%s: got '%s', expected substring '%s'\n",
            name, got ? got : "(null)", want);
        exit(1);
    }
}

static void test_session_registry_round_trip(void) {
    mkdir("build/session-store-home", 0777);
    setenv("CEZAR_HOME", "build/session-store-home", 1);

    char uuid[CEZAR_SESSION_UUID_LEN];
    expect_int("create session",
        session_store_create("round trip", "/tmp/project", uuid, sizeof uuid), 0);
    expect_int("uuid len", (int)strlen(uuid), 36);
    expect_int("uuid dash", uuid[8], '-');

    expect_int("save session",
        session_store_save(uuid, "round trip", "/tmp/project",
            "[{\"role\":\"system\",\"content\":\"hello\"}]"), 0);

    char *list = session_store_list_text();
    expect_contains("list uuid", list, uuid);
    expect_contains("list name", list, "round trip");
    free(list);

    expect_int("rename session",
        session_store_rename(uuid, "renamed session"), 0);

    list = session_store_list_text();
    expect_contains("list renamed", list, "renamed session");
    free(list);

    char *conversation = session_store_load_conversation(uuid);
    expect_contains("load conversation", conversation, "\"role\":\"system\"");
    free(conversation);
}

int main(void) {
    test_session_registry_round_trip();
    return 0;
}
