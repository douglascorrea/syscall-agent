#ifndef CEZAR_CODEX_PROVIDER_H
#define CEZAR_CODEX_PROVIDER_H

#include "../vendor/cJSON.h"

#include <stddef.h>

typedef struct {
    char *access_token;
    char *account_id;
    char *auth_path;
} CodexAuthInfo;

int codex_auth_load(CodexAuthInfo *out, char *err, size_t err_cap);
void codex_auth_info_free(CodexAuthInfo *auth);

cJSON *codex_responses_chat(const char *model,
                            cJSON *messages,
                            cJSON *tools,
                            int want_reasoning);

#endif
