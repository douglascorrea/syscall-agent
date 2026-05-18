# Contributing

Thanks for improving `cezar`. Keep changes small, testable, and aligned
with the single-binary C design.

## Workflow

1. Build and test locally before opening a change:

   ```sh
   make clean
   make
   make test
   ```

2. Prefer focused patches. Avoid unrelated refactors, generated churn, or
   formatting-only edits mixed with behavior changes.

3. Add or update tests for behavior changes. New tools should cover both tool
   registration and dispatch behavior where practical.

4. Document user-visible features in `README.md` and update
   `SYSTEM_PROMPT.md` when the model-visible tool surface changes.

## Safety

- Do not add code that prints, scrapes, or repurposes private auth tokens.
- Keep subprocess execution argv-only. Do not route tool commands through a
  shell unless the user explicitly chooses a shell executable as the command.
- Gate powerful local execution paths behind `--allow-exec`; gate intentionally
  unsandboxed write-capable delegation behind `--allow-unsafe-exec`.
- Treat extension manifests as trusted local code because they execute programs
  with the user's permissions.

## Style

- Use C11 and the existing `Buf` and cJSON helpers.
- Keep comments sparse and useful.
- Keep platform-specific syscalls behind the existing `os_compat_*` boundary
  unless a feature is deliberately platform-specific.
