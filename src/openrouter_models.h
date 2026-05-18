#ifndef CEZAR_OPENROUTER_MODELS_H
#define CEZAR_OPENROUTER_MODELS_H

#include <stddef.h>

typedef struct {
    char *id;
    char *name;
    char *description;
    char *prompt_price;
    char *completion_price;
    int context_length;
} OpenRouterModel;

typedef struct {
    OpenRouterModel *items;
    size_t len;
    size_t cap;
} OpenRouterModelCatalog;

void openrouter_model_catalog_init(OpenRouterModelCatalog *cat);
void openrouter_model_catalog_free(OpenRouterModelCatalog *cat);

int openrouter_models_parse(const char *json,
                            OpenRouterModelCatalog *cat,
                            char *err,
                            size_t err_len);

int openrouter_models_fetch(const char *api_key,
                            OpenRouterModelCatalog *cat,
                            char *err,
                            size_t err_len);

int openrouter_model_matches(const OpenRouterModel *model, const char *query);

#endif
