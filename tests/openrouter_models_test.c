#include "openrouter_models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", name, got, want);
        exit(1);
    }
}

static void expect_str(const char *name, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", name, got ? got : "(null)", want);
        exit(1);
    }
}

static void test_parse_models_catalog(void) {
    const char *json =
        "{"
        "  \"data\": ["
        "    {"
        "      \"id\": \"openai/gpt-5.4\","
        "      \"name\": \"GPT-5.4\","
        "      \"description\": \"Frontier coding model\","
        "      \"context_length\": 400000,"
        "      \"pricing\": {\"prompt\": \"0.000001\", \"completion\": \"0.000008\"}"
        "    },"
        "    {"
        "      \"id\": \"anthropic/claude-sonnet-4.5\","
        "      \"name\": \"Claude Sonnet 4.5\","
        "      \"context_length\": 200000,"
        "      \"pricing\": {\"prompt\": \"0.000003\", \"completion\": \"0.000015\"}"
        "    }"
        "  ]"
        "}";

    OpenRouterModelCatalog cat;
    openrouter_model_catalog_init(&cat);
    char err[256];
    int rc = openrouter_models_parse(json, &cat, err, sizeof err);
    expect_int("parse rc", rc, 0);
    expect_int("catalog len", (int)cat.len, 2);
    expect_str("first id", cat.items[0].id, "openai/gpt-5.4");
    expect_str("first name", cat.items[0].name, "GPT-5.4");
    expect_str("first prompt price", cat.items[0].prompt_price, "0.000001");
    expect_str("second id", cat.items[1].id, "anthropic/claude-sonnet-4.5");
    expect_int("second context", cat.items[1].context_length, 200000);
    openrouter_model_catalog_free(&cat);
}

static void test_parse_rejects_missing_data_array(void) {
    OpenRouterModelCatalog cat;
    openrouter_model_catalog_init(&cat);
    char err[256];
    int rc = openrouter_models_parse("{\"data\":{}}", &cat, err, sizeof err);
    expect_int("parse invalid rc", rc, -1);
    openrouter_model_catalog_free(&cat);
}

static void test_model_filter_matches_id_name_and_description(void) {
    OpenRouterModel m = {
        .id = "google/gemini-2.5-flash",
        .name = "Gemini 2.5 Flash",
        .description = "Fast multimodal model"
    };
    expect_int("empty matches", openrouter_model_matches(&m, ""), 1);
    expect_int("id matches", openrouter_model_matches(&m, "google"), 1);
    expect_int("name matches case-insensitive", openrouter_model_matches(&m, "FLASH"), 1);
    expect_int("description matches", openrouter_model_matches(&m, "multi"), 1);
    expect_int("no match", openrouter_model_matches(&m, "claude"), 0);
}

int main(void) {
    test_parse_models_catalog();
    test_parse_rejects_missing_data_array();
    test_model_filter_matches_id_name_and_description();
    return 0;
}
