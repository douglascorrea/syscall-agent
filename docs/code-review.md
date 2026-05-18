# Code review

Date: 2026-05-16

Scope: current tree after adding TUI conversation state and OpenRouter
streaming support.

## Findings

No blocking correctness, security, or regression findings remain open.

## Review Notes

- Tool visibility now has a runtime path through `list_tools`, and the tool is
  covered by `tools_meta_test`.
- Auth visibility avoids printing raw secret values and keeps Codex/Copilot
  subscription credentials out of the model-call path. Subscription usage goes
  through official CLI delegation tools instead.
- New TUI commands are parse-tested, and the rendered command bodies are local
  status surfaces rather than network calls.
- `/model` and `/models` now open a live OpenRouter model picker backed by
  `GET https://openrouter.ai/api/v1/models`, with search, navigation, refresh,
  and Enter-to-select behavior.
- The model catalog parser is isolated in `openrouter_models.c` and covered by
  `openrouter_models_test`, including response parsing and case-insensitive
  filtering.
- TUI prompts now run against a reusable `AgentConversation`, so previous user,
  assistant, and tool messages stay in the model context until `/new`.
- TUI mode enables OpenRouter SSE streaming. Assistant text deltas, reasoning
  deltas, and tool-call argument deltas are forwarded through agent events as
  they arrive, while the final accumulated assistant message is still appended
  to conversation history for tool execution and follow-up turns.
- `agent_conversation_test` covers multi-turn context retention and reset.
- `agent_streaming_test` covers streamed assistant, reasoning, and tool-call
  deltas before tool execution.
- `grep_text` output is capped by result count and per-line bytes to avoid
  pathological large-line output.
- The README and system prompt have both been updated so the model-facing and
  human-facing docs match the registered tool surface.
- Termux wrappers are fixed-command `execvp` calls with short timeouts, output
  caps, command-availability checks, and bounded user-provided text arguments.
- `termux_clipboard_set` sends user text over stdin, avoiding option parsing for
  clipboard content that begins with `-`.
- `tools_termux_test` covers catalog registration, non-Termux status behavior,
  and validation paths for notification, clipboard, vibration, and wake-lock
  inputs.

## Residual Risk

- `cezar` remains intentionally powerful when `--allow-exec` is enabled.
  The default-off registration, profile validation, resource limits, and macOS
  sandboxing reduce that risk, but untrusted repositories should still be run in
  disposable workspaces.
- The C buffer helper exits on unchecked `realloc` failure in existing code
  paths by dereferencing `NULL`. That was pre-existing and outside this feature
  pass; a future hardening PR should make `Buf` growth fallible throughout the
  project.
- Termux device tools can expose mobile state such as clipboard and Wi-Fi
  details. This is intentional for the Termux feature set, but users should run
  the agent only in trusted sessions and avoid exposing private clipboard
  content to untrusted prompts.
- The model picker performs a synchronous catalog fetch in the TUI thread. That
  keeps the implementation simple, but the UI can pause briefly while
  OpenRouter responds.
- Streaming still depends on provider behavior. Some providers may buffer tool
  calls or reasoning chunks before OpenRouter receives them; the agent displays
  deltas as soon as OpenRouter sends them.

## Verification

- `make test`
- `make`
- `git diff --check`
