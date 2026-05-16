# Security best-practices review

Date: 2026-05-16

## Executive Summary

The installed `security-best-practices` skill does not include a C-specific
reference set, so this review applies general C and local-agent security
guidance. No critical issues remain open in the current tree. The highest-risk
surfaces are subprocess execution, Termux device integrations, local file
reads/writes, URL fetching, and credential visibility. The current
implementation keeps arbitrary subprocess tools gated, keeps Linux sandboxing
fail-closed, rejects non-HTTP(S) fetches, masks special mode bits on writes,
redacts secret-like environment variables, runs Termux integrations through
fixed argv-only commands, and uses the OpenRouter API key only as a bearer token
for OpenRouter API calls.

## Critical

None found.

## High

None found.

## Medium

### M-1: Subscription credential misuse risk

Impact: A coding agent that reuses ChatGPT/Codex or Copilot subscription tokens
as undocumented API credentials can leak account privileges and create a
publishability risk.

Status: Mitigated.

Evidence:

- `auth_status` only checks whether credential surfaces are configured and does
  not print token values: `src/tools_meta.c:92`.
- The auth status output explicitly states that Codex OAuth and Copilot
  subscription credentials are visibility-only and are not repurposed:
  `src/tools_meta.c:121`.
- `env_get` redacts variables whose names contain sensitive markers:
  `src/tools_meta.c:56`.
- Codex/Copilot subscription usage is implemented through official local CLI
  delegation rather than token scraping. Delegation requires `--allow-exec`;
  write-capable delegation requires `--allow-unsafe-exec`.

### M-2: Unbounded tool output from text search

Impact: A local file containing very long matching lines could cause excessive
memory use when returned to the model.

Status: Fixed.

Evidence:

- `grep_text` caps matching lines to 200 results and each emitted matching line
  to 4096 bytes: `src/tools_meta.c:288`, `src/tools_meta.c:313`.

## Low

### L-1: Local skill roots can expose local file paths

Impact: `list_skills` prints skill-pack paths. This is useful for operators, but
it can expose local usernames or directory layout in a model transcript.

Status: Accepted and documented.

Evidence:

- Skill names are constrained before reading skill content:
  `src/tools_meta.c:339`.
- `read_skill` only opens `name/SKILL.md` under configured roots and truncates
  content at 64 KiB: `src/tools_meta.c:390`, `src/tools_meta.c:418`.

### L-2: Exec remains intentionally powerful when enabled

Impact: `--allow-exec` exposes local process execution. This is expected for a
coding agent, but it is still the highest-risk runtime mode.

Status: Mitigated by default-off gating and sandbox/resource controls.

Evidence:

- Exec tools are only registered when exec is enabled:
  `src/tools_proc.c:708`.
- `profile=none` requires the explicit unsafe flag:
  `src/tools_proc.c:209`.
- Child processes receive resource limits before `execvp`:
  `src/tools_proc.c:270`.
- HTTP tools reject non-HTTP(S) schemes:
  `src/http.c:63`.
- Atomic writes mask mode bits and check `fchmod`/`close` errors:
  `src/tools_fs.c:197`, `src/tools_fs.c:204`.

### L-3: Termux device tools expose mobile device state

Impact: Termux integrations can read device-adjacent state such as battery,
Wi-Fi details, and clipboard text, and can cause bounded side effects such as
notifications, vibration, and wake-lock changes.

Status: Mitigated by narrow wrappers and documented operator expectations.

Evidence:

- Termux tools are fixed command wrappers dispatched by explicit tool name:
  `src/tools_termux.c:379`, `src/tools_termux.c:391`,
  `src/tools_termux.c:405`, `src/tools_termux.c:421`.
- The wrapper uses `fork`/`execvp` directly and never invokes a shell:
  `src/tools_termux.c:137`, `src/tools_termux.c:173`.
- Output is capped and each command has a short timeout:
  `src/tools_termux.c:19`, `src/tools_termux.c:139`,
  `src/tools_termux.c:214`.
- User-provided notification and clipboard arguments are length-limited:
  `src/tools_termux.c:399`, `src/tools_termux.c:410`.
- Clipboard text is sent through stdin instead of as a parsed command-line
  option: `src/tools_termux.c:401`, `src/tools_termux.c:402`.
- Termux install and source-signature caveats are documented:
  `docs/termux-install.md:6`, `docs/termux-install.md:96`.

### L-4: Model catalog fetch uses the OpenRouter API key

Impact: The TUI model picker fetches OpenRouter's model catalog and may include
`OPENROUTER_API_KEY` as a bearer token when available.

Status: Accepted. This is the same provider boundary already used for chat
requests, and the key is not printed into the TUI transcript.

Evidence:

- Catalog fetch is limited to OpenRouter's documented models endpoint:
  `src/openrouter_models.c:10`, `src/openrouter_models.c:140`.
- The Authorization header is constructed only for the request and is not
  persisted or displayed: `src/openrouter_models.c:129`,
  `src/openrouter_models.c:133`.

## Verification

- `make test`
- `make`
- `git diff --check`
