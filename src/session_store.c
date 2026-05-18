#include "session_store.h"
#include "util.h"
#include "../vendor/cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int mkdir_if_needed(const char *path) {
    if (mkdir(path, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

int session_store_home(char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    const char *custom = getenv("CEZAR_HOME");
    if (custom && *custom) {
        snprintf(out, cap, "%s", custom);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(out, cap, "%s/.cezar", home);
    return 0;
}

static int ensure_layout(char *sessions_dir, size_t cap) {
    char home[PATH_MAX];
    if (session_store_home(home, sizeof home) != 0) return -1;
    if (mkdir_if_needed(home) != 0) return -1;
    snprintf(sessions_dir, cap, "%s/sessions", home);
    if (mkdir_if_needed(sessions_dir) != 0) return -1;
    return 0;
}

static int valid_uuid(const char *uuid) {
    if (!uuid || strlen(uuid) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return 0;
        } else if (!isxdigit((unsigned char)uuid[i])) {
            return 0;
        }
    }
    return 1;
}

static int session_file_path(const char *uuid, char *out, size_t cap) {
    if (!valid_uuid(uuid)) return -1;
    char sessions[PATH_MAX];
    if (ensure_layout(sessions, sizeof sessions) != 0) return -1;
    snprintf(out, cap, "%s/%s.json", sessions, uuid);
    return 0;
}

static long long now_unix(void) {
    return (long long)time(NULL);
}

static void format_uuid(const unsigned char b[16], char *out, size_t cap) {
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3],
        b[4], b[5],
        b[6], b[7],
        b[8], b[9],
        b[10], b[11], b[12], b[13], b[14], b[15]);
}

static int generate_uuid(char *out, size_t cap) {
    if (!out || cap < CEZAR_SESSION_UUID_LEN) return -1;
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, b, sizeof b) : -1;
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof b) {
        unsigned long long seed = (unsigned long long)time(NULL) ^
            ((unsigned long long)getpid() << 32);
        for (size_t i = 0; i < sizeof b; i++) {
            seed = seed * 6364136223846793005ULL + 1ULL;
            b[i] = (unsigned char)(seed >> 32);
        }
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    format_uuid(b, out, cap);
    return 0;
}

static cJSON *load_session_root(const char *uuid) {
    char path[PATH_MAX];
    if (session_file_path(uuid, path, sizeof path) != 0) return NULL;
    size_t len = 0;
    char *text = read_text_file(path, &len);
    if (!text) return NULL;
    cJSON *root = cJSON_Parse(text);
    free(text);
    return root;
}

static int write_session_root(const char *uuid, cJSON *root) {
    char path[PATH_MAX];
    if (session_file_path(uuid, path, sizeof path) != 0) return -1;
    char *text = cJSON_Print(root);
    if (!text) return -1;
    int rc = write_text_file(path, text, strlen(text));
    free(text);
    return rc;
}

int session_store_create(const char *name,
                         const char *cwd,
                         char *uuid_out,
                         size_t uuid_cap) {
    char sessions[PATH_MAX];
    if (ensure_layout(sessions, sizeof sessions) != 0) return -1;
    if (generate_uuid(uuid_out, uuid_cap) != 0) return -1;
    return session_store_save(uuid_out, name ? name : "untitled",
        cwd ? cwd : ".", "[]");
}

int session_store_save(const char *uuid,
                       const char *name,
                       const char *cwd,
                       const char *conversation_json) {
    if (!valid_uuid(uuid)) return -1;

    long long created = now_unix();
    cJSON *old = load_session_root(uuid);
    if (old) {
        cJSON *old_created = cJSON_GetObjectItem(old, "created_at");
        if (cJSON_IsNumber(old_created)) created = (long long)old_created->valuedouble;
        cJSON_Delete(old);
    }

    cJSON *messages = conversation_json ? cJSON_Parse(conversation_json) : NULL;
    if (!cJSON_IsArray(messages)) {
        if (messages) cJSON_Delete(messages);
        messages = cJSON_CreateArray();
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(messages);
        return -1;
    }
    cJSON_AddStringToObject(root, "uuid", uuid);
    cJSON_AddStringToObject(root, "name", name && *name ? name : "untitled");
    cJSON_AddStringToObject(root, "cwd", cwd && *cwd ? cwd : ".");
    cJSON_AddNumberToObject(root, "created_at", (double)created);
    cJSON_AddNumberToObject(root, "updated_at", (double)now_unix());
    cJSON_AddItemToObject(root, "messages", messages);
    int rc = write_session_root(uuid, root);
    cJSON_Delete(root);
    return rc;
}

char *session_store_load_conversation(const char *uuid) {
    cJSON *root = load_session_root(uuid);
    if (!root) return NULL;
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    char *out = cJSON_IsArray(messages) ? cJSON_PrintUnformatted(messages) : NULL;
    cJSON_Delete(root);
    return out;
}

char *session_store_list_text(void) {
    char sessions[PATH_MAX];
    Buf out;
    buf_init(&out);
    buf_append_cstr(&out, "SESSIONS\n---\n");
    if (ensure_layout(sessions, sizeof sessions) != 0) {
        buf_append_cstr(&out, "ERROR: could not open session store\n");
        return out.data;
    }

    DIR *d = opendir(sessions);
    if (!d) {
        buf_append_cstr(&out, "(no sessions)\n");
        return out.data;
    }

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n != 41 || strcmp(e->d_name + n - 5, ".json") != 0) continue;
        char uuid[CEZAR_SESSION_UUID_LEN];
        memcpy(uuid, e->d_name, 36);
        uuid[36] = '\0';
        if (!valid_uuid(uuid)) continue;
        cJSON *root = load_session_root(uuid);
        if (!root) continue;
        cJSON *name = cJSON_GetObjectItem(root, "name");
        cJSON *cwd = cJSON_GetObjectItem(root, "cwd");
        cJSON *updated = cJSON_GetObjectItem(root, "updated_at");
        buf_printf(&out, "%s\t%s\t%s\tupdated:%lld\n",
            uuid,
            cJSON_IsString(name) && name->valuestring ? name->valuestring : "untitled",
            cJSON_IsString(cwd) && cwd->valuestring ? cwd->valuestring : ".",
            cJSON_IsNumber(updated) ? (long long)updated->valuedouble : 0LL);
        count++;
        cJSON_Delete(root);
    }
    closedir(d);
    if (count == 0) buf_append_cstr(&out, "(no sessions)\n");
    return out.data;
}

int session_store_rename(const char *uuid, const char *name) {
    if (!valid_uuid(uuid) || !name || !*name) return -1;
    cJSON *root = load_session_root(uuid);
    if (!root) return -1;
    cJSON_ReplaceItemInObject(root, "name", cJSON_CreateString(name));
    cJSON_ReplaceItemInObject(root, "updated_at", cJSON_CreateNumber((double)now_unix()));
    int rc = write_session_root(uuid, root);
    cJSON_Delete(root);
    return rc;
}
