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

typedef enum {
    AGENT_EVENT_STEP_START = 1,
    AGENT_EVENT_MODEL_REQUEST_START,
    AGENT_EVENT_TOOL_CALL_START,
    AGENT_EVENT_TOOL_CALL_RESULT,
    AGENT_EVENT_FINAL_RESPONSE,
    AGENT_EVENT_ERROR,
} AgentEventType;

typedef struct {
    AgentEventType type;
    int step_index;
    int max_steps;
    long duration_ms;
    const char *message;
    const char *tool_name;
    const char *tool_args;
} AgentEvent;

typedef void (*AgentEventFn)(const AgentEvent *event, void *userdata);

typedef struct {
    AgentEventFn on_event;
    void *userdata;
} AgentHooks;

typedef struct AgentSession AgentSession;

AgentSession *agent_session_new(const AgentConfig *cfg);
int agent_session_run(AgentSession *session,
                      const char *user_prompt,
                      const AgentHooks *hooks,
                      char **out_final);
void agent_session_free(AgentSession *session);

/* Runs the agent for a single user prompt. Returns 0 on success, non-zero on error. */
int agent_run(const AgentConfig *cfg, const char *user_prompt);

#endif
