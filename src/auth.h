#ifndef CEZAR_AUTH_H
#define CEZAR_AUTH_H

#include <stddef.h>

#define CEZAR_AUTH_ARGV_MAX 8

int auth_build_login_argv(const char *provider,
                          const char *host,
                          int free_flow,
                          char **argv,
                          size_t argv_cap,
                          char *err,
                          size_t err_cap);

char *auth_login_preview(const char *provider, const char *host, int free_flow);
char *auth_login_capture(const char *provider,
                         const char *host,
                         int free_flow,
                         int timeout_ms);
int auth_login_interactive(const char *provider,
                           const char *host,
                           int free_flow);

#endif
