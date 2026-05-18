# cezar

`cezar` is a compact coding agent written in pure **C**. It talks to
OpenRouter, keeps durable memory in Markdown, and exposes local tools that lean
on OS primitives such as `fork`, `execvp`, `mmap`, `rename`, `kqueue`,
`inotify`, `getaddrinfo`, non-blocking sockets, and process-table syscalls.

The goal is a small single-binary agent that can inspect code, edit files,
perform network lookups, run bounded local commands, and stay usable from both
plain CLI mode and a responsive terminal TUI.

> [!IMPORTANT]
> This is a local coding agent with powerful filesystem and optional process
> execution tools. Run it in a repository or disposable workspace you trust.

## Highlights

- Pure C implementation with one generated binary.
- OpenRouter chat-completions backend by default.
- Optional direct Codex/OpenAI subscription backend through local Codex
  ChatGPT auth (`CEZAR_PROVIDER=codex`).
- Tool-calling loop with file, web, memory, process, watch, network, and Termux tools.
- Runtime tool catalog, auth status, host diagnostics, and local skill-pack tools.
- Supported Codex/OpenAI and GitHub Copilot login entrypoints that keep tokens
  inside the official local clients.
- Manifest-based extension tools discovered from project, user, or configured roots.
- Persistent TUI command history and resumable UUID-addressed sessions.
- Optional syscall-backed command execution with resource limits.
- Atomic file writes via `mkstemp` + `rename`.
- `mmap` range reads for large files and logs.
- Persistent `MEMORY.md` with append locking.
- Interactive `--tui` mode inspired by Pi Agent.
- Android Termux install path and Termux:API device integrations.
- Focused regression tests for TUI commands, agent events, tools, and security checks.

## Requirements

- macOS, Linux, or Android through Termux
- C compiler with C11 support
- `make`
- `libcurl`
- OpenRouter API key, unless using `CEZAR_PROVIDER=codex`

On macOS, `libcurl` is usually already available. On Debian/Ubuntu:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev
```

On Android, install Termux from F-Droid or GitHub, then inside Termux:

```sh
pkg update
pkg upgrade
pkg install git clang make libcurl
```

Full Android setup, storage, and Termux:API instructions are in
[docs/termux-install.md](docs/termux-install.md).

## Build

```sh
make
```

The binary is written to:

```sh
build/cezar
```

Run the test suite:

```sh
make test
```

## Quick Start

```sh
export OPENROUTER_API_KEY=sk-or-...

./build/cezar "summarize this repository"
./build/cezar -s 20 -m anthropic/claude-3.5-sonnet "find the largest C file and explain it"
echo "what changed recently?" | ./build/cezar
```

Open the TUI:

```sh
./build/cezar --tui
```

Enable subprocess tools:

```sh
./build/cezar --allow-exec "run the tests and summarize failures"
```

Use a Codex/OpenAI subscription after signing in with ChatGPT:

```sh
codex --login
CEZAR_PROVIDER=codex ./build/cezar "summarize this repository"
```

In this mode the agent reads the local Codex ChatGPT auth file to attach a
bearer token and `ChatGPT-Account-ID` header to
`https://chatgpt.com/backend-api/codex/responses`. Tokens are never printed in
status output or tool results. If the token expires, run `/login codex` or
`codex --login` again.

## Configuration

| Variable | Purpose |
| --- | --- |
| `OPENROUTER_API_KEY` | Required when `CEZAR_PROVIDER=openrouter`. |
| `OPENROUTER_MODEL` | Default model when `-m/--model` is not set. |
| `CEZAR_PROVIDER` | `openrouter` (default) or `codex`/`openai-codex`. |
| `OPENAI_MODEL` | Default model when `CEZAR_PROVIDER=codex` (default `codex-mini-latest`). |
| `BRAVE_SEARCH_API_KEY` | Optional Brave Search key; otherwise web search falls back to DuckDuckGo HTML. |
| `SYSTEM_PROMPT_PATH` | Override `SYSTEM_PROMPT.md`. |
| `MEMORY_PATH` | Override `MEMORY.md`. |
| `CEZAR_ALLOW_EXEC=1` | Enable subprocess tools. |
| `CEZAR_ALLOW_UNSAFE_EXEC=1` | Allow unsandboxed `profile=none`; implies exec tools. |
| `CEZAR_AUTH_PROVIDER` | Descriptive provider label shown by auth status tools. Defaults to `openrouter`. |
| `CEZAR_SKILLS_DIR` | Optional directory containing local `name/SKILL.md` skill packs. |
| `CEZAR_EXTENSIONS_DIR` | Optional directory containing extension manifests. |
| `CEZAR_HOME` | Optional state directory for TUI history and session registry. Defaults to `~/.cezar`. |
| `CEZAR_CONFIG` | Optional JSON config path. Defaults to `./cezar.json`, then `~/.cezar/config.json`, when either file exists. |

Flags:

```text
--tui                 open the interactive terminal UI
-s, --steps N         max agent-loop iterations (default 10)
-m, --model NAME      model id (default openai/gpt-4o-mini for OpenRouter,
                      codex-mini-latest for codex, or provider env vars)
--system PATH         path to SYSTEM_PROMPT.md
--memory PATH         path to MEMORY.md
--allow-exec          enable exec_command, spawn_bg, bg_read, bg_kill, bg_list
--allow-unsafe-exec   also allow profile=none; implies --allow-exec
-v, --verbose         trace tool calls to stderr in plain CLI mode
-h, --help            show help
```

## TUI

The TUI is intentionally small and terminal-native. It uses full-width accent
borders, muted status/footer lines, padded user blocks, and compact tool or
reasoning panels.

In TUI mode, model responses use OpenRouter streaming. Assistant text appears as
it arrives. Tool-call deltas appear in real time when `/verbose tools` or
`/verbose all` is enabled, and reasoning deltas appear in real time when
`/verbose reasoning` or `/verbose all` is enabled.

Slash commands:

| Command | Behavior |
| --- | --- |
| `/model` | Open a live OpenRouter model picker backed by `GET /api/v1/models`. |
| `/models` | Open the same model picker. |
| `/model N` | Select a built-in quick choice by list index. |
| `/model provider/model-id` | Set any OpenRouter model id directly. |
| `/verbose normal` | Show only user and assistant conversation text. |
| `/verbose tools` | Also show tool calls and tool results. |
| `/verbose reasoning` | Also show model reasoning fields when returned. |
| `/verbose reasioning` | Accepted alias for the original requested spelling. |
| `/verbose all` | Show tools and reasoning output. |
| `/steer INSTRUCTION` | Interrupt the active run and restart it with a steering update. `/teer` is accepted as a typo alias. |
| `/settings` | Show model, provider, context estimate, compaction, and statusline settings. |
| `/settings statusline FIELD on\|off` | Toggle `model`, `context`, `session`, or `verbose` details in the statusline. |
| `/compact PERCENT` | Set the automatic compaction threshold. Values are clamped to the supported `50..95` range. |
| `/tools` | Show tool families visible to the model. |
| `/skills` | List local skill packs from configured skill roots. |
| `/extensions` | List extension manifests and registered extension tools. |
| `/auth` | Show auth-provider status without printing secrets. |
| `/login codex` | Run `codex --login` in the terminal so the official Codex client handles ChatGPT/OpenAI sign-in. |
| `/login copilot [host]` | Run `copilot login` or `copilot login --host HOST` for GitHub Copilot OAuth. |
| `/sysinfo` | Show host OS, architecture, cwd, CPU count, and page size. |
| `/sessions` | List saved TUI sessions with UUIDs, names, cwd, and update times. |
| `/resume UUID` | Switch the TUI conversation context to a saved session. |
| `/rename NAME` | Rename the current saved session. |
| `/new` | Clear the visible transcript. |
| `/exit` | Leave the TUI. |

Input history is persisted in `$CEZAR_HOME/history`. In the main prompt,
Up replays older prompts or slash commands and Down moves forward again.
Long input is wrapped across prompt rows instead of being clipped. `Ctrl-J`
inserts a newline into the current prompt; `Enter` submits it. While a run is
active, `/verbose`, `/settings`, `/compact`, and `/steer` are accepted without
waiting for the model to finish. `Esc` or `Ctrl-C` requests cancellation of the
active run.

Model picker controls:

| Key | Behavior |
| --- | --- |
| Type text | Filter models by id, name, or description. |
| Up/Down | Move through the filtered list. |
| PageUp/PageDown | Jump through the filtered list. |
| Enter | Select the highlighted model for the current session. |
| Esc | Close the picker without changing the model. |
| Ctrl-R | Refresh the OpenRouter model catalog. |

Reasoning display supports OpenRouter-style `reasoning`, `reasoning_content`,
and `reasoning_details` fields when the selected model returns them, including
streamed `choices[].delta.reasoning_details` chunks.

## Config File

`cezar` optionally reads a JSON config from `CEZAR_CONFIG`, `./cezar.json`, or
`~/.cezar/config.json`.

```json
{
  "compaction": {
    "threshold_percent": 75,
    "context_limit": 128000,
    "model": "openai/gpt-4o-mini",
    "prompt": "Summarize the conversation for later continuation."
  },
  "statusline": {
    "model": true,
    "context": true,
    "session": true,
    "verbose": true
  }
}
```

Automatic compaction uses an approximate token estimate and runs before a new
model call once the conversation reaches the configured percentage of the
context limit. The threshold cannot be set below `50%`.

## Tools

Always available:

| Tool | Capability |
| --- | --- |
| `list_tools` | List every currently visible tool and one-line description. |
| `read_file` | Read up to 256 KB from a local file. |
| `search_files` | Recursively search filenames by glob or substring. |
| `search_web` | Brave Search or DuckDuckGo HTML search. |
| `fetch_url` | HTTP/HTTPS GET with raw response output. |
| `web_fetch` | HTTP/HTTPS GET with HTML stripped to text. |
| `save_memory` | Append durable notes to `MEMORY.md`. |
| `auth_status` | Report configured auth surfaces without exposing secrets. |
| `system_info` | Inspect host OS, architecture, cwd, CPU count, and page size. |
| `disk_usage` | Inspect filesystem capacity and inode counts via `statvfs`. |
| `env_get` | Read allowlisted configuration environment variables with secret redaction. |
| `which` | Locate executables on `PATH` without invoking a shell. |
| `file_digest` | Compute an FNV-1a 64-bit checksum for change detection. |
| `grep_text` | Search one text file for literal matching lines. |
| `list_skills` | List local `name/SKILL.md` skill packs. |
| `read_skill` | Read a local skill pack by safe skill name. |
| `list_extensions` | List extension manifests and the tools they register. |
| `stat` | Inspect metadata without reading file content. |
| `list_dir` | List directory entries with type, size, and mtime. |
| `write_file` | Atomic file replacement using a same-directory temp file. |
| `read_file_range` | `mmap`-backed byte-range read for large files. |
| `dns_lookup` | Resolve A and AAAA records through `getaddrinfo`. |
| `tcp_check` | Non-blocking TCP reachability probe with duration. |
| `watch_path` | Wait for file or directory changes. |
| `list_processes` | List top processes by RSS. |
| `termux_info` | Detect Termux/Android environment and command availability. |
| `termux_api_status` | Check Termux:API helper command availability and setup hints. |
| `termux_storage_status` | Check Android shared-storage symlinks from `termux-setup-storage`. |
| `termux_battery_status` | Read Android battery state through Termux:API. |
| `termux_wifi_info` | Read current Wi-Fi connection details through Termux:API. |
| `termux_clipboard_get` | Read Android clipboard text through Termux:API. |
| `termux_clipboard_set` | Set Android clipboard text through Termux:API. |
| `termux_notification` | Show an Android notification through Termux:API. |
| `termux_vibrate` | Vibrate the Android device for a bounded duration. |
| `termux_wake_lock` | Acquire or release Termux's wake lock. |

Termux tools degrade gracefully outside Termux or when the relevant `termux-*`
command is missing. They invoke fixed Termux commands through argv-only
`execvp`, never through a shell. See
[docs/termux-install.md](docs/termux-install.md) for Android setup.

Gated by `--allow-exec`:

| Tool | Capability |
| --- | --- |
| `exec_command` | Run an argv-only command, capture stdout/stderr, enforce rlimits. |
| `spawn_bg` | Start a background command and return a handle. |
| `bg_read` | Read buffered background output by offset. |
| `bg_kill` | Terminate a background process. |
| `bg_list` | List background processes owned by this agent. |
| `auth_login` | Start a supported subscription login flow through `codex --login`, `codex --free`, or `copilot login`; `dry_run` previews the command. |
| `delegate_codex` | Delegate to the official Codex CLI using its own supported auth. |
| `delegate_copilot` | Delegate to the official GitHub Copilot CLI using its own supported auth. |
| extension tools | Command-backed tools declared by trusted extension manifests. |

`exec_command` and `spawn_bg` never invoke a shell. The model must provide an
argv array, so shell metacharacters are ordinary arguments unless the chosen
executable is itself a shell.

Auth, delegation, and extension tools are also gated by `--allow-exec`.
Subscription login runs official local clients (`codex --login`, `codex --free`,
or `copilot login`) through argv-only `fork`/`execvp`; delegation invokes
`codex exec` and `copilot -p` the same way. Direct Codex model calls are
available with `CEZAR_PROVIDER=codex`; they read the same local Codex
ChatGPT auth file used by the official client, attach the token to the request,
and never print the token. Delegation is read-only by default.
`mode=workspace-write` requires `--allow-unsafe-exec`.

## Auth Surfaces

Model calls go through OpenRouter by default. They can instead use local Codex
ChatGPT auth when `CEZAR_PROVIDER=codex` is set. The agent also reports
common coding-agent auth surfaces so operators can see what is configured on
the machine:

| Surface | Behavior |
| --- | --- |
| OpenRouter | Default model backend through `OPENROUTER_API_KEY`. |
| OpenAI API | Detected through `OPENAI_API_KEY` for future provider work. |
| Codex/OpenAI | Login via `/login codex` or `auth_login(provider="codex")`; direct model backend via `CEZAR_PROVIDER=codex`; delegation via `delegate_codex`. |
| GitHub/Copilot | Login via `/login copilot [host]` or `auth_login(provider="copilot")`, detected by `auth_status`, usable through `delegate_copilot` when the official `copilot` CLI is installed and logged in. |

`cezar` does not print ChatGPT/Codex OAuth or GitHub Copilot
subscription tokens. It only reads the Codex ChatGPT access token for direct
Codex backend requests when `CEZAR_PROVIDER=codex` is explicitly set.

## Extensions

Extensions are trusted local JSON manifests that register command-backed tools
without changing the core C binary. This is intentionally smaller than Pi's
TypeScript extension API, but it follows the same shape: discovered extension
packages can add tools, and tools are refreshed from configured roots at runtime.

The agent searches:

```text
$CEZAR_EXTENSIONS_DIR
./extensions
~/.cezar/extensions
```

Each extension can be a `*.json` file or a directory containing
`extension.json`:

```json
{
  "name": "demo",
  "description": "Demo extension",
  "tools": [
    {
      "name": "ext_echo",
      "description": "Echo from an extension",
      "command": ["/bin/echo", "hello from extension"],
      "parameters": {
        "type": "object",
        "properties": {},
        "required": []
      }
    }
  ]
}
```

Tool names must be valid function names (`[A-Za-z_][A-Za-z0-9_]*`). Extension
commands never run through a shell. The model's tool arguments are written as
JSON to the command's stdin, and stdout/stderr are captured into the tool
result. Extension tools require `--allow-exec` because manifests execute local
programs with the user's permissions.

## Sessions

TUI sessions are persisted under `$CEZAR_HOME/sessions` as one JSON file
per UUID. The TUI creates a session automatically, saves the conversation after
each submitted prompt or slash command, and can switch context with
`/resume UUID`. Use `/sessions` to discover UUIDs and `/rename NAME` to give the
current session a stable label.

## Skills

Skill packs are simple directories containing `SKILL.md`. The agent searches:

```text
$CEZAR_SKILLS_DIR
./skills
~/.cezar/skills
```

Use `list_skills` to discover available packs and `read_skill` to load one into
the conversation. In the TUI, `/skills` shows the same roots.

## Execution Safety

Every subprocess gets these resource limits:

| Limit | Value |
| --- | --- |
| CPU time | 30 seconds |
| Address space | 512 MB |
| Output file size | 256 MB |
| Open files | 256 |
| Captured stdout/stderr | 256 KB per stream |

OpenRouter SSE streaming is only enabled for the interactive TUI. Plain CLI mode
keeps the one-shot response path unless a future flag enables stream printing.

Sandbox profiles:

| Profile | macOS behavior | Linux behavior |
| --- | --- | --- |
| `readonly` | Read filesystem, write only tmp/devnull paths, no network. | Fails closed until Linux sandboxing is implemented. |
| `default` | Read filesystem, process spawning, write only tmp/devnull paths, no network. | Fails closed until Linux sandboxing is implemented. |
| `network` | `default` plus outbound network. | Fails closed until Linux sandboxing is implemented. |
| `build` | Filesystem writes allowed for build loops, no network. | Fails closed until Linux sandboxing is implemented. |
| `none` | No sandbox; requires `--allow-unsafe-exec`. | No sandbox; requires `--allow-unsafe-exec`. |

HTTP tools accept only `http://` and `https://` URLs. `write_file` masks mode
bits to regular permission bits so tool calls cannot request setuid/setgid
outputs.

## Memory

The agent reads `SYSTEM_PROMPT.md` and `MEMORY.md` at startup. `MEMORY.md` is
created on first use and appended through `save_memory`.

Override paths:

```sh
./build/cezar --system prompts/system.md --memory state/MEMORY.md "use this context"
```

## Repository Layout

```text
src/
  main.c                 CLI entry and flag parsing
  agent.c                tool-calling loop and event emission
  auth.c                 supported Codex/OpenAI and Copilot login wrappers
  codex_provider.c       direct Codex Responses API transport using local ChatGPT auth
  extensions.c           manifest-discovered extension tool registry
  openrouter.c           OpenRouter request/response handling
  openrouter_models.c    OpenRouter model catalog fetch/parse/filter helpers
  tui.c                  raw terminal UI
  tools.c                shared tool registration and dispatch
  tools_meta.c           tool catalog, auth, host diagnostics, skills, grep/checksum
  tools_termux.c         Android Termux detection and Termux:API wrappers
  tools_fs.c             stat, list_dir, write_file, read_file_range
  tools_proc.c           exec, background process, process listing
  tools_watch.c          kqueue/inotify path watching
  tools_net.c            DNS and TCP probes
  os_compat_*.c          platform-specific syscall shims
  memory.c               Markdown memory loading/appending
  session_store.c        UUID session registry and persistent TUI state
  http.c                 libcurl wrapper
vendor/
  cJSON.c                vendored JSON parser
tests/
  *_test.c               focused regression tests
```

## Development

```sh
make clean
make
make test
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution workflow and safety
expectations. This project is released under the [MIT License](LICENSE).

Useful smoke checks:

```sh
./build/cezar --help
./build/cezar -m openai/gpt-4o-mini -s 3 "Reply with OK only"
./build/cezar --tui
```

The research notes behind the current roadmap live in
[`docs/research/2026-05-16-roadmap.md`](docs/research/2026-05-16-roadmap.md).
