#include "memory.h"
#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

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

    /* O_APPEND on most platforms gives us atomic appends per-write, but we
     * still take an exclusive flock so parallel agents can't interleave the
     * header + body of one entry. */
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) { buf_free(&b); return -1; }
    if (flock(fd, LOCK_EX) != 0) { close(fd); buf_free(&b); return -1; }
    ssize_t wn = write(fd, b.data, b.len);
    int ok = (wn >= 0 && (size_t)wn == b.len);
    flock(fd, LOCK_UN);
    close(fd);
    buf_free(&b);
    return ok ? 0 : -1;
}
