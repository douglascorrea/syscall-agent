#include "tools_net.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static char *dup_or_default(cJSON *node, const char *def) {
    if (cJSON_IsString(node) && node->valuestring) return strdup(node->valuestring);
    return def ? strdup(def) : NULL;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static cJSON *make_function_tool(const char *name, const char *desc, cJSON *params) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "description", desc);
    cJSON_AddItemToObject(fn, "parameters", params);
    return tool;
}

static cJSON *param_obj(const char *const *required) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "object");
    cJSON_AddObjectToObject(p, "properties");
    cJSON *req = cJSON_AddArrayToObject(p, "required");
    if (required) {
        for (size_t i = 0; required[i]; i++) {
            cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
        }
    }
    return p;
}

static void add_prop(cJSON *params, const char *name, const char *type, const char *desc) {
    cJSON *props = cJSON_GetObjectItem(params, "properties");
    cJSON *p = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
}

/* ---------- dns_lookup ---------- */

static char *tool_dns_lookup(cJSON *args) {
    char *host = dup_or_default(cJSON_GetObjectItem(args, "host"), NULL);
    if (!host) return strdup("ERROR: missing required arg 'host'");
    cJSON *fam_j = cJSON_GetObjectItem(args, "family");
    const char *fam_s = cJSON_IsString(fam_j) ? fam_j->valuestring : NULL;

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    if      (fam_s && strcmp(fam_s, "ipv4") == 0) hints.ai_family = AF_INET;
    else if (fam_s && strcmp(fam_s, "ipv6") == 0) hints.ai_family = AF_INET6;
    else                                          hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    Buf out; buf_init(&out);
    buf_printf(&out, "DNS_LOOKUP %s\n---\n", host);
    if (rc != 0) {
        buf_printf(&out, "ERROR: %s\n", gai_strerror(rc));
        free(host);
        return out.data;
    }
    int n = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next, n++) {
        char buf[64] = "?";
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf);
            buf_printf(&out, "A     %s\n", buf);
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *sa = (struct sockaddr_in6 *)p->ai_addr;
            inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof buf);
            buf_printf(&out, "AAAA  %s\n", buf);
        }
    }
    if (n == 0) buf_append_cstr(&out, "(no records)\n");
    freeaddrinfo(res);
    free(host);
    return out.data;
}

/* ---------- tcp_check ---------- */

static char *tool_tcp_check(cJSON *args) {
    char *host = dup_or_default(cJSON_GetObjectItem(args, "host"), NULL);
    cJSON *port_j = cJSON_GetObjectItem(args, "port");
    cJSON *t_j    = cJSON_GetObjectItem(args, "timeout_ms");
    int port = cJSON_IsNumber(port_j) ? (int)port_j->valueint : 0;
    int timeout_ms = cJSON_IsNumber(t_j) ? (int)t_j->valueint : 3000;
    if (!host || port <= 0 || port > 65535) {
        free(host);
        return strdup("ERROR: 'host' and valid 'port' required");
    }

    char service[16];
    snprintf(service, sizeof service, "%d", port);

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, service, &hints, &res);
    Buf out; buf_init(&out);
    if (rc != 0) {
        buf_printf(&out, "TCP_CHECK %s:%d -> ERROR: %s\n",
            host, port, gai_strerror(rc));
        free(host);
        return out.data;
    }

    long t0 = now_ms();
    int reachable = 0;
    int last_err = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) { last_err = errno; continue; }
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int cr = connect(s, p->ai_addr, p->ai_addrlen);
        if (cr == 0) { reachable = 1; close(s); break; }
        if (errno != EINPROGRESS) { last_err = errno; close(s); continue; }

        struct pollfd pfd = { .fd = s, .events = POLLOUT };
        int pn = poll(&pfd, 1, timeout_ms);
        if (pn > 0) {
            int err = 0;
            socklen_t sl = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &sl);
            if (err == 0) { reachable = 1; close(s); break; }
            last_err = err;
        } else if (pn == 0) {
            last_err = ETIMEDOUT;
        } else {
            last_err = errno;
        }
        close(s);
    }
    freeaddrinfo(res);

    long dt = now_ms() - t0;
    if (reachable) {
        buf_printf(&out, "TCP_CHECK %s:%d -> REACHABLE in %ld ms\n", host, port, dt);
    } else {
        buf_printf(&out, "TCP_CHECK %s:%d -> UNREACHABLE (%s) in %ld ms\n",
            host, port, last_err ? strerror(last_err) : "no candidate", dt);
    }
    free(host);
    return out.data;
}

void tools_net_register(cJSON *arr) {
    {
        const char *req[] = {"host", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "host",   "string", "Hostname to resolve.");
        add_prop(p, "family", "string", "'ipv4', 'ipv6', or unset (both).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "dns_lookup",
            "Resolve a hostname via getaddrinfo. Returns A and AAAA records.",
            p));
    }
    {
        const char *req[] = {"host", "port", NULL};
        cJSON *p = param_obj(req);
        add_prop(p, "host",       "string",  "Hostname or IP.");
        add_prop(p, "port",       "integer", "TCP port 1-65535.");
        add_prop(p, "timeout_ms", "integer", "Connect timeout in ms (default 3000).");
        cJSON_AddItemToArray(arr, make_function_tool(
            "tcp_check",
            "Try to open a TCP connection to host:port; close immediately. Returns reachable/unreachable + duration.",
            p));
    }
}

char *tools_net_dispatch(ToolCtx *ctx, const char *name, cJSON *args) {
    (void)ctx;
    if (!name) return NULL;
    if (strcmp(name, "dns_lookup") == 0) return tool_dns_lookup(args);
    if (strcmp(name, "tcp_check") == 0)  return tool_tcp_check(args);
    return NULL;
}
