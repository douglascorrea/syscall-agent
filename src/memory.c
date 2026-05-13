#include "memory.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

char *memory_load(const char *path, size_t *out_len) {
    if (!file_exists(path)) return NULL;
    return read_text_file(path, out_len);
}

int memory_append(const char *path, const char *topic, const char *content) {
    Buf b;
    buf_init(&b);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);

    if (!file_exists(path)) {
        buf_append_cstr(&b, "# MEMORY\n\nPersisted notes the agent decided to remember.\n");
    }
    buf_printf(&b, "\n## %s — %s\n\n%s\n",
        topic && *topic ? topic : "note", ts,
        content ? content : "");

    int rc = append_text_file(path, b.data, b.len);
    buf_free(&b);
    return rc;
}
