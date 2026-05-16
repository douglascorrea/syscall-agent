# Security best-practices review

Date: 2026-05-16

## Executive Summary

The installed `security-best-practices` skill does not include a C-specific
reference set, so this review applies general C and local-agent security
guidance. No critical issues remain open in the current tree. The highest-risk
surfaces are subprocess execution, local file reads/writes, URL fetching, and
credential visibility. The current implementation keeps subprocess tools gated,
keeps Linux sandboxing fail-closed, rejects non-HTTP(S) fetches, masks special
mode bits on writes, and redacts secret-like environment variables.

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

## Verification

- `make test`
- `make`
- `git diff --check`
