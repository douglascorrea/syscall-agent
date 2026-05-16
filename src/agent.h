#ifndef LLA_AGENT_H
#define LLA_AGENT_H

typedef struct {
    const char *api_key;
    const char *model;
    const char *system_path;
    const char *memory_path;
    int max_steps;
    int verbose;
    int stream;
    int allow_exec;
    int allow_unsafe_exec;
} AgentConfig;

#define AGENT_VERBOSE_NORMAL    0
#define AGENT_VERBOSE_TOOLS     (1 << 0)
#define AGENT_VERBOSE_REASONING (1 << 1)
#define AGENT_VERBOSE_ALL       (AGENT_VERBOSE_TOOLS | AGENT_VERBOSE_REASONING)

typedef enum {
    AGENT_EVENT_STATUS,
    AGENT_EVENT_TOOL_CALL,
    AGENT_EVENT_TOOL_RESULT,
    AGENT_EVENT_REASONING,
    AGENT_EVENT_REASONING_DELTA,
    AGENT_EVENT_ASSISTANT,
    AGENT_EVENT_ASSISTANT_DELTA,
    AGENT_EVENT_TOOL_CALL_DELTA,
    AGENT_EVENT_ERROR
} AgentEventType;

typedef struct {
    AgentEventType type;
    const char *title;
    const char *content;
} AgentEvent;

typedef void (*AgentEventHandler)(const AgentEvent *event, void *userdata);

typedef struct AgentConversation AgentConversation;

AgentConversation *agent_conversation_new(const AgentConfig *cfg);
void agent_conversation_reset(AgentConversation *conv, const AgentConfig *cfg);
void agent_conversation_free(AgentConversation *conv);

int agent_conversation_run_with_events(AgentConversation *conv,
                                       const AgentConfig *cfg,
                                       const char *user_prompt,
                                       AgentEventHandler handler,
                                       void *userdata);

/* Runs the agent for a single user prompt. Returns 0 on success, non-zero on error. */
int agent_run(const AgentConfig *cfg, const char *user_prompt);

/* Same agent loop, but emits UI-friendly events instead of printing the final answer. */
int agent_run_with_events(const AgentConfig *cfg,
                          const char *user_prompt,
                          AgentEventHandler handler,
                          void *userdata);

#endif
