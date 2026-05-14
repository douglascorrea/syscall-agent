#ifndef LLA_AGENT_H
#define LLA_AGENT_H

typedef struct {
    const char *api_key;
    const char *model;
    const char *system_path;
    const char *memory_path;
    int max_steps;
    int verbose;
    int allow_exec;
    int allow_unsafe_exec;
} AgentConfig;

/* Runs the agent for a single user prompt. Returns 0 on success, non-zero on error. */
int agent_run(const AgentConfig *cfg, const char *user_prompt);

#endif
