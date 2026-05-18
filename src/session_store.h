#ifndef CEZAR_SESSION_STORE_H
#define CEZAR_SESSION_STORE_H

#include <stddef.h>

#define CEZAR_SESSION_UUID_LEN 37

int session_store_create(const char *name,
                         const char *cwd,
                         char *uuid_out,
                         size_t uuid_cap);
int session_store_save(const char *uuid,
                       const char *name,
                       const char *cwd,
                       const char *conversation_json);
char *session_store_load_conversation(const char *uuid);
char *session_store_list_text(void);
int session_store_rename(const char *uuid, const char *name);
int session_store_home(char *out, size_t cap);

#endif
