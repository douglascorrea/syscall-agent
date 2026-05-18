#ifndef CEZAR_MEMORY_H
#define CEZAR_MEMORY_H

#include <stddef.h>

/* Returns malloc'd contents of memory file, or NULL if not present. */
char *memory_load(const char *path, size_t *out_len);

/* Appends a memory entry with timestamp and topic header. */
int memory_append(const char *path, const char *topic, const char *content);

#endif
