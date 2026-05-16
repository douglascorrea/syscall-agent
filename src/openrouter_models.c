#include "openrouter_models.h"
#include "http.h"

#include "../vendor/cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPENROUTER_MODELS_URL "https://openrouter.ai/api/v1/models"

static void set_err(char *err, size_t err_len, const char *msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg ? msg : "unknown error");
}

static char *dup_string(const char *s) {
    return strdup(s ? s : "");
}

static char *dup_json_string(cJSON *node, const char *fallback) {
    if (cJSON_IsString(node) && node->valuestring) return dup_string(node->valuestring);
    return dup_string(fallback ? fallback : "");
}

void openrouter_model_catalog_init(OpenRouterModelCatalog *cat) {
    if (!cat) return;
    cat->items = NULL;
    cat->len = 0;
    cat->cap = 0;
}

void openrouter_model_catalog_free(OpenRouterModelCatalog *cat) {
    if (!cat) return;
    for (size_t i = 0; i < cat->len; i++) {
        free(cat->items[i].id);
        free(cat->items[i].name);
        free(cat->items[i].description);
        free(cat->items[i].prompt_price);
        free(cat->items[i].completion_price);
    }
    free(cat->items);
    cat->items = NULL;
    cat->len = 0;
    cat->cap = 0;
}

static int catalog_add(OpenRouterModelCatalog *cat, OpenRouterModel model) {
    if (cat->len >= cat->cap) {
        size_t nc = cat->cap ? cat->cap * 2 : 64;
        OpenRouterModel *items = realloc(cat->items, nc * sizeof(OpenRouterModel));
        if (!items) return -1;
        cat->items = items;
        cat->cap = nc;
    }
    cat->items[cat->len++] = model;
    return 0;
}

static void model_free(OpenRouterModel *model) {
    free(model->id);
    free(model->name);
    free(model->description);
    free(model->prompt_price);
    free(model->completion_price);
    memset(model, 0, sizeof *model);
}

int openrouter_models_parse(const char *json,
                            OpenRouterModelCatalog *cat,
                            char *err,
                            size_t err_len) {
    if (!json || !cat) {
        set_err(err, err_len, "missing JSON or catalog");
        return -1;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        set_err(err, err_len, "invalid JSON response");
        return -1;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        set_err(err, err_len, "OpenRouter response did not contain a data array");
        return -1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, data) {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || !id->valuestring || !*id->valuestring) continue;

        OpenRouterModel model;
        memset(&model, 0, sizeof model);
        model.id = dup_string(id->valuestring);
        model.name = dup_json_string(cJSON_GetObjectItem(item, "name"), id->valuestring);
        model.description = dup_json_string(cJSON_GetObjectItem(item, "description"), "");
        cJSON *ctx = cJSON_GetObjectItem(item, "context_length");
        model.context_length = cJSON_IsNumber(ctx) ? (int)ctx->valueint : 0;

        cJSON *pricing = cJSON_GetObjectItem(item, "pricing");
        model.prompt_price = dup_json_string(cJSON_GetObjectItem(pricing, "prompt"), "");
        model.completion_price = dup_json_string(cJSON_GetObjectItem(pricing, "completion"), "");

        if (!model.id || !model.name || !model.description ||
            !model.prompt_price || !model.completion_price ||
            catalog_add(cat, model) != 0) {
            model_free(&model);
            cJSON_Delete(root);
            set_err(err, err_len, "out of memory while parsing model catalog");
            return -1;
        }
    }

    cJSON_Delete(root);
    set_err(err, err_len, "");
    return 0;
}

int openrouter_models_fetch(const char *api_key,
                            OpenRouterModelCatalog *cat,
                            char *err,
                            size_t err_len) {
    if (!cat) {
        set_err(err, err_len, "missing catalog");
        return -1;
    }

    char auth[2048];
    const char *headers[2] = {NULL, NULL};
    if (api_key && *api_key) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
        headers[0] = auth;
    }

    HttpResponse r = http_get(OPENROUTER_MODELS_URL, headers[0] ? headers : NULL);
    if (r.error) {
        set_err(err, err_len, r.error);
        http_response_free(&r);
        return -1;
    }
    if (r.status < 200 || r.status >= 300) {
        char msg[256];
        snprintf(msg, sizeof msg, "OpenRouter models request failed with HTTP %ld", r.status);
        set_err(err, err_len, msg);
        http_response_free(&r);
        return -1;
    }

    openrouter_model_catalog_free(cat);
    openrouter_model_catalog_init(cat);
    int rc = openrouter_models_parse(r.body, cat, err, err_len);
    http_response_free(&r);
    return rc;
}

static int contains_nocase(const char *haystack, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!haystack) haystack = "";
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;

    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static int model_contains_token(const OpenRouterModel *model, const char *token) {
    return contains_nocase(model->id, token) ||
           contains_nocase(model->name, token) ||
           contains_nocase(model->description, token);
}

int openrouter_model_matches(const OpenRouterModel *model, const char *query) {
    if (!model) return 0;
    if (!query || !*query) return 1;

    char *copy = strdup(query);
    if (!copy) return 0;
    int matched_any = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, " \t\r\n", &save);
         tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        matched_any = 1;
        if (!model_contains_token(model, tok)) {
            free(copy);
            return 0;
        }
    }
    free(copy);
    return matched_any ? 1 : 1;
}
