#include "agent.h"
#include "http.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "low_level_agent — a single-binary AI agent in C\n"
        "\n"
        "USAGE:\n"
        "  %s [options] [PROMPT]\n"
        "\n"
        "If PROMPT is omitted, reads one line from stdin.\n"
        "\n"
        "OPTIONS:\n"
        "  -s, --steps N         Max agent-loop iterations (default 10)\n"
        "  -m, --model NAME      OpenRouter model id (default openai/gpt-4o-mini\n"
        "                        or $OPENROUTER_MODEL)\n"
        "      --system PATH     Path to SYSTEM_PROMPT.md (default ./SYSTEM_PROMPT.md\n"
        "                        or $SYSTEM_PROMPT_PATH)\n"
        "      --memory PATH     Path to MEMORY.md (default ./MEMORY.md\n"
        "                        or $MEMORY_PATH)\n"
        "  -v, --verbose         Print step / tool-call traces to stderr\n"
        "  -h, --help            Show this help\n"
        "\n"
        "ENVIRONMENT:\n"
        "  OPENROUTER_API_KEY    Required.\n"
        "  OPENROUTER_MODEL      Default model.\n"
        "  SYSTEM_PROMPT_PATH    Default path to system prompt file.\n"
        "  MEMORY_PATH           Default path to memory file.\n",
        argv0);
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 1000) return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv) {
    AgentConfig cfg = {
        .api_key     = getenv("OPENROUTER_API_KEY"),
        .model       = getenv_or("OPENROUTER_MODEL", "openai/gpt-4o-mini"),
        .system_path = getenv_or("SYSTEM_PROMPT_PATH", "SYSTEM_PROMPT.md"),
        .memory_path = getenv_or("MEMORY_PATH", "MEMORY.md"),
        .max_steps   = 10,
        .verbose     = 0,
    };

    const char *prompt = NULL;

    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            cfg.verbose = 1;
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

    if (!cfg.api_key || !*cfg.api_key) {
        fprintf(stderr, "ERROR: OPENROUTER_API_KEY is not set.\n");
        return 2;
    }

    char *line = NULL;
    if (!prompt) {
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
    int rc = agent_run(&cfg, prompt);
    http_global_cleanup();

    free(line);
    return rc;
}
