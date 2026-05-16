# Code review

Date: 2026-05-16

Scope: changes from `3a24b01` on `master` through the current
`feature/research-roadmap-capabilities` branch.

## Findings

No blocking correctness, security, or regression findings remain open.

## Review Notes

- Tool visibility now has a runtime path through `list_tools`, and the tool is
  covered by `tools_meta_test`.
- Auth visibility avoids printing raw secret values and keeps Codex/Copilot
  subscription credentials out of the model-call path.
- New TUI commands are parse-tested, and the rendered command bodies are local
  status surfaces rather than network calls.
- `grep_text` output is capped by result count and per-line bytes to avoid
  pathological large-line output.
- The README and system prompt have both been updated so the model-facing and
  human-facing docs match the registered tool surface.

## Residual Risk

- `syscall-agent` remains intentionally powerful when `--allow-exec` is enabled.
  The default-off registration, profile validation, resource limits, and macOS
  sandboxing reduce that risk, but untrusted repositories should still be run in
  disposable workspaces.
- The C buffer helper exits on unchecked `realloc` failure in existing code
  paths by dereferencing `NULL`. That was pre-existing and outside this feature
  pass; a future hardening PR should make `Buf` growth fallible throughout the
  project.

## Verification

- `make test`
- `make`
- `git diff --check`
