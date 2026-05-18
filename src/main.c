#include "agent.h"
#include "http.h"
#include "tui.h"
#include "util.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int context_limit;
    int compaction_percent;
    char *compaction_model;
    char *compaction_prompt;
    int statusline_model;
    int statusline_context;
    int statusline_session;
    int statusline_verbose;
} CezarRuntimeConfig;

static void usage(const char *argv0) {
    fprintf(stderr,
        "cezar — a single-binary coding agent in C\n"
        "\n"
        "USAGE:\n"
        "  %s [options] [PROMPT]\n"
        "\n"
        "If PROMPT is omitted, reads one line from stdin.\n"
        "\n"
        "OPTIONS:\n"
        "      --tui             Start the interactive terminal UI\n"
        "  -s, --steps N         Max agent-loop iterations (default 10)\n"
        "  -m, --model NAME      Model id (default openai/gpt-4o-mini for OpenRouter,\n"
        "                        codex-mini-latest for codex, or provider env vars)\n"
        "      --system PATH     Path to SYSTEM_PROMPT.md (default ./SYSTEM_PROMPT.md\n"
        "                        or $SYSTEM_PROMPT_PATH)\n"
        "      --memory PATH     Path to MEMORY.md (default ./MEMORY.md\n"
        "                        or $MEMORY_PATH)\n"
        "      --allow-exec      Enable exec_command and spawn_bg tools\n"
        "                        (sandboxed by default on macOS; Linux fails closed\n"
        "                        until sandbox support exists).\n"
        "      --allow-unsafe-exec\n"
        "                        Allow profile='none' in exec/spawn_bg (no sandbox).\n"
        "                        Implies --allow-exec.\n"
        "  -v, --verbose         Print step / tool-call traces to stderr\n"
        "  -h, --help            Show this help\n"
        "\n"
        "ENVIRONMENT:\n"
        "  OPENROUTER_API_KEY    Required for the default OpenRouter provider.\n"
        "  OPENROUTER_MODEL      Default model.\n"
        "  CEZAR_PROVIDER\n"
        "                        openrouter (default) or codex/openai-codex.\n"
        "  OPENAI_MODEL          Default model when CEZAR_PROVIDER=codex.\n"
        "  BRAVE_SEARCH_API_KEY  Optional Brave Search API key for search_web.\n"
        "  CEZAR_AUTH_PROVIDER\n"
        "                        Descriptive provider label for auth_status.\n"
        "  CEZAR_SKILLS_DIR\n"
        "                        Optional directory containing name/SKILL.md packs.\n"
        "  CEZAR_EXTENSIONS_DIR\n"
        "                        Optional directory containing extension manifests.\n"
        "  CEZAR_HOME\n"
        "                        Optional TUI history/session state directory.\n"
        "  CEZAR_CONFIG          Optional JSON config path (default ./cezar.json\n"
        "                        or ~/.cezar/config.json when present).\n"
        "  SYSTEM_PROMPT_PATH    Default path to system prompt file.\n"
        "  MEMORY_PATH           Default path to memory file.\n",
        argv0);
}

static void runtime_config_init(CezarRuntimeConfig *rc, const char *default_model) {
    memset(rc, 0, sizeof *rc);
    rc->context_limit = 128000;
    rc->compaction_percent = 75;
    rc->compaction_model = strdup(default_model ? default_model : "openai/gpt-4o-mini");
    rc->compaction_prompt = strdup(
        "Summarize the conversation so far for a coding agent. Preserve user goals, "
        "decisions, files changed, commands run, errors, and unresolved next steps.");
    rc->statusline_model = 1;
    rc->statusline_context = 1;
    rc->statusline_session = 1;
    rc->statusline_verbose = 1;
}

static void runtime_config_free(CezarRuntimeConfig *rc) {
    free(rc->compaction_model);
    free(rc->compaction_prompt);
}

static char *default_config_path(void) {
    const char *custom = getenv("CEZAR_CONFIG");
    if (custom && *custom) return strdup(custom);
    if (file_exists("cezar.json")) return strdup("cezar.json");
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char path[4096];
    snprintf(path, sizeof path, "%s/.cezar/config.json", home);
    return file_exists(path) ? strdup(path) : NULL;
}

static int json_int_range(cJSON *obj, const char *key, int min, int max, int current) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(v)) return current;
    int n = (int)v->valueint;
    if (n < min) n = min;
    if (n > max) n = max;
    return n;
}

static void replace_json_string(char **dst, cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring || !*v->valuestring) return;
    char *copy = strdup(v->valuestring);
    if (!copy) return;
    free(*dst);
    *dst = copy;
}

static int json_bool(cJSON *obj, const char *key, int current) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return current;
}

static void runtime_config_load(CezarRuntimeConfig *rc) {
    char *path = default_config_path();
    if (!path) return;
    size_t len = 0;
    char *text = read_text_file(path, &len);
    free(path);
    if (!text) return;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;

    cJSON *compaction = cJSON_GetObjectItem(root, "compaction");
    if (cJSON_IsObject(compaction)) {
        rc->context_limit = json_int_range(compaction, "context_limit", 1000, 10000000, rc->context_limit);
        rc->compaction_percent = json_int_range(compaction, "threshold_percent", 50, 95, rc->compaction_percent);
        replace_json_string(&rc->compaction_model, compaction, "model");
        replace_json_string(&rc->compaction_prompt, compaction, "prompt");
    }

    cJSON *statusline = cJSON_GetObjectItem(root, "statusline");
    if (cJSON_IsObject(statusline)) {
        rc->statusline_model = json_bool(statusline, "model", rc->statusline_model);
        rc->statusline_context = json_bool(statusline, "context", rc->statusline_context);
        rc->statusline_session = json_bool(statusline, "session", rc->statusline_session);
        rc->statusline_verbose = json_bool(statusline, "verbose", rc->statusline_verbose);
    }
    cJSON_Delete(root);
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 1000) return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv) {
    const char *provider = getenv_or("CEZAR_PROVIDER", "openrouter");
    const char *default_model =
        (strcmp(provider, "codex") == 0 ||
         strcmp(provider, "openai-codex") == 0 ||
         strcmp(provider, "chatgpt") == 0)
            ? getenv_or("OPENAI_MODEL", "codex-mini-latest")
            : getenv_or("OPENROUTER_MODEL", "openai/gpt-4o-mini");
    CezarRuntimeConfig runtime;
    runtime_config_init(&runtime, default_model);
    runtime_config_load(&runtime);
    AgentConfig cfg = {
        .api_key     = getenv("OPENROUTER_API_KEY"),
        .provider    = provider,
        .model       = default_model,
        .system_path = getenv_or("SYSTEM_PROMPT_PATH", "SYSTEM_PROMPT.md"),
        .memory_path = getenv_or("MEMORY_PATH", "MEMORY.md"),
        .max_steps         = 10,
        .verbose           = AGENT_VERBOSE_NORMAL,
        .allow_exec        = getenv("CEZAR_ALLOW_EXEC") ? 1 : 0,
        .allow_unsafe_exec = getenv("CEZAR_ALLOW_UNSAFE_EXEC") ? 1 : 0,
        .context_limit     = runtime.context_limit,
        .compaction_percent = runtime.compaction_percent,
        .compaction_model  = runtime.compaction_model,
        .compaction_prompt = runtime.compaction_prompt,
        .statusline_model  = runtime.statusline_model,
        .statusline_context = runtime.statusline_context,
        .statusline_session = runtime.statusline_session,
        .statusline_verbose = runtime.statusline_verbose,
    };
    if (cfg.allow_unsafe_exec) cfg.allow_exec = 1;

    const char *prompt = NULL;
    int use_tui = 0;

    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--tui") == 0) {
            use_tui = 1;
            i++;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            cfg.verbose = AGENT_VERBOSE_TOOLS;
            i++;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--steps") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a); return 2; }
            if (parse_int(argv[++i], &cfg.max_steps) != 0) {
                fprintf(stderr, "invalid --steps value: %s\n", argv[i]);
                return 2;
            }
            i++;
        } else if (strcmp(a, "-m") == 0 || strcmp(a, "--model") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a); return 2; }
            cfg.model = argv[++i];
            i++;
        } else if (strcmp(a, "--system") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a); return 2; }
            cfg.system_path = argv[++i];
            i++;
        } else if (strcmp(a, "--memory") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a); return 2; }
            cfg.memory_path = argv[++i];
            i++;
        } else if (strcmp(a, "--allow-exec") == 0) {
            cfg.allow_exec = 1;
            i++;
        } else if (strcmp(a, "--allow-unsafe-exec") == 0) {
            cfg.allow_exec = 1;
            cfg.allow_unsafe_exec = 1;
            i++;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]);
            return 2;
        } else {
            prompt = a;
            i++;
            break;
        }
    }

    int provider_is_codex =
        cfg.provider &&
        (strcmp(cfg.provider, "codex") == 0 ||
         strcmp(cfg.provider, "openai-codex") == 0 ||
         strcmp(cfg.provider, "chatgpt") == 0);
    int provider_is_openrouter = !cfg.provider || strcmp(cfg.provider, "openrouter") == 0;
    if (!provider_is_codex && !provider_is_openrouter) {
        fprintf(stderr, "ERROR: unknown CEZAR_PROVIDER '%s'\n", cfg.provider);
        return 2;
    }
    if (provider_is_openrouter &&
        (!cfg.api_key || !*cfg.api_key)) {
        fprintf(stderr, "ERROR: OPENROUTER_API_KEY is not set.\n");
        return 2;
    }

    char *line = NULL;
    if (!prompt && !use_tui) {
        size_t cap = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n <= 0) {
            fprintf(stderr, "ERROR: no prompt provided (give one as an arg or via stdin)\n");
            free(line);
            return 2;
        }
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        prompt = line;
    }

    http_global_init();
    if (use_tui) cfg.stream = 1;
    int rc = use_tui ? tui_run(&cfg) : agent_run(&cfg, prompt);
    http_global_cleanup();

    free(line);
    runtime_config_free(&runtime);
    return rc;
}
