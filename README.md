# syscall-agent

`syscall-agent` is a compact coding agent written in pure **C**. It talks to
OpenRouter, keeps durable memory in Markdown, and exposes local tools that lean
on OS primitives such as `fork`, `execvp`, `mmap`, `rename`, `kqueue`,
`inotify`, `getaddrinfo`, non-blocking sockets, and process-table syscalls.

The goal is a small single-binary agent that can inspect code, edit files,
perform network lookups, run bounded local commands, and stay usable from both
plain CLI mode and a responsive Pi-style TUI.

> [!IMPORTANT]
> This is a local coding agent with powerful filesystem and optional process
> execution tools. Run it in a repository or disposable workspace you trust.

## Highlights

- Pure C implementation with one generated binary.
- OpenRouter chat-completions backend.
- Tool-calling loop with file, web, memory, process, watch, and network tools.
- Runtime tool catalog, auth status, host diagnostics, and local skill-pack tools.
- Optional syscall-backed command execution with resource limits.
- Atomic file writes via `mkstemp` + `rename`.
- `mmap` range reads for large files and logs.
- Persistent `MEMORY.md` with append locking.
- Interactive `--tui` mode inspired by Pi Agent.
- Focused regression tests for TUI commands, agent events, and security checks.

## Requirements

- macOS or Linux
- C compiler with C11 support
- `make`
- `libcurl`
- OpenRouter API key

On macOS, `libcurl` is usually already available. On Debian/Ubuntu:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev
```

## Build

```sh
make
```

The binary is written to:

```sh
build/agent
```

Run the test suite:

```sh
make test
```

## Quick Start

```sh
export OPENROUTER_API_KEY=sk-or-...

./build/agent "summarize this repository"
./build/agent -s 20 -m anthropic/claude-3.5-sonnet "find the largest C file and explain it"
echo "what changed recently?" | ./build/agent
```

Open the TUI:

```sh
./build/agent --tui
```

Enable subprocess tools:

```sh
./build/agent --allow-exec "run the tests and summarize failures"
```

## Configuration

| Variable | Purpose |
| --- | --- |
| `OPENROUTER_API_KEY` | Required OpenRouter API key. |
| `OPENROUTER_MODEL` | Default model when `-m/--model` is not set. |
| `BRAVE_SEARCH_API_KEY` | Optional Brave Search key; otherwise web search falls back to DuckDuckGo HTML. |
| `SYSTEM_PROMPT_PATH` | Override `SYSTEM_PROMPT.md`. |
| `MEMORY_PATH` | Override `MEMORY.md`. |
| `LLA_ALLOW_EXEC=1` | Enable subprocess tools. |
| `LLA_ALLOW_UNSAFE_EXEC=1` | Allow unsandboxed `profile=none`; implies exec tools. |
| `SYSCALL_AGENT_AUTH_PROVIDER` | Descriptive provider label shown by auth status tools. Defaults to `openrouter`. |
| `SYSCALL_AGENT_SKILLS_DIR` | Optional directory containing local `name/SKILL.md` skill packs. |

Flags:

```text
--tui                 open the interactive terminal UI
-s, --steps N         max agent-loop iterations (default 10)
-m, --model NAME      OpenRouter model id (default openai/gpt-4o-mini)
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

Slash commands:

| Command | Behavior |
| --- | --- |
| `/model` | List predefined OpenRouter model choices. |
| `/model N` | Select a model by list index. |
| `/model provider/model-id` | Select a predefined model by id. |
| `/verbose normal` | Show only user and assistant conversation text. |
| `/verbose tools` | Also show tool calls and tool results. |
| `/verbose reasoning` | Also show model reasoning fields when returned. |
| `/verbose reasioning` | Accepted alias for the original requested spelling. |
| `/verbose all` | Show tools and reasoning output. |
| `/tools` | Show tool families visible to the model. |
| `/skills` | List local skill packs from configured skill roots. |
| `/auth` | Show auth-provider status without printing secrets. |
| `/sysinfo` | Show host OS, architecture, cwd, CPU count, and page size. |
| `/new` | Clear the visible transcript. |
| `/exit` | Leave the TUI. |

Reasoning display supports OpenRouter-style `reasoning`, `reasoning_content`,
and `reasoning_details` fields when the selected model returns them.

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
| `stat` | Inspect metadata without reading file content. |
| `list_dir` | List directory entries with type, size, and mtime. |
| `write_file` | Atomic file replacement using a same-directory temp file. |
| `read_file_range` | `mmap`-backed byte-range read for large files. |
| `dns_lookup` | Resolve A and AAAA records through `getaddrinfo`. |
| `tcp_check` | Non-blocking TCP reachability probe with duration. |
| `watch_path` | Wait for file or directory changes. |
| `list_processes` | List top processes by RSS. |

Gated by `--allow-exec`:

| Tool | Capability |
| --- | --- |
| `exec_command` | Run an argv-only command, capture stdout/stderr, enforce rlimits. |
| `spawn_bg` | Start a background command and return a handle. |
| `bg_read` | Read buffered background output by offset. |
| `bg_kill` | Terminate a background process. |
| `bg_list` | List background processes owned by this agent. |

`exec_command` and `spawn_bg` never invoke a shell. The model must provide an
argv array, so shell metacharacters are ordinary arguments unless the chosen
executable is itself a shell.

## Auth Surfaces

Model calls currently go through OpenRouter with `OPENROUTER_API_KEY`. The
agent also reports other common coding-agent auth surfaces so operators can see
what is configured on the machine:

| Surface | Behavior |
| --- | --- |
| OpenRouter | Used for model requests through `OPENROUTER_API_KEY`. |
| OpenAI API | Detected through `OPENAI_API_KEY` for future provider work. |
| Codex CLI OAuth | Detects `CODEX_HOME/auth.json` or `~/.codex/auth.json` presence only. |
| GitHub/Copilot | Detects `GH_TOKEN` or `GITHUB_TOKEN` presence only. |

`syscall-agent` does not print, scrape, or repurpose ChatGPT/Codex OAuth or
GitHub Copilot subscription tokens. That keeps provider integration on the
documented side of the boundary while leaving room for official support later.

## Skills

Skill packs are simple directories containing `SKILL.md`. The agent searches:

```text
$SYSCALL_AGENT_SKILLS_DIR
./skills
~/.syscall-agent/skills
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
./build/agent --system prompts/system.md --memory state/MEMORY.md "use this context"
```

## Repository Layout

```text
src/
  main.c                 CLI entry and flag parsing
  agent.c                tool-calling loop and event emission
  openrouter.c           OpenRouter request/response handling
  tui.c                  raw terminal UI
  tools.c                shared tool registration and dispatch
  tools_meta.c           tool catalog, auth, host diagnostics, skills, grep/checksum
  tools_fs.c             stat, list_dir, write_file, read_file_range
  tools_proc.c           exec, background process, process listing
  tools_watch.c          kqueue/inotify path watching
  tools_net.c            DNS and TCP probes
  os_compat_*.c          platform-specific syscall shims
  memory.c               Markdown memory loading/appending
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

Useful smoke checks:

```sh
./build/agent --help
./build/agent -m openai/gpt-4o-mini -s 3 "Reply with OK only"
./build/agent --tui
```

The research notes behind the current roadmap live in
[`docs/research/2026-05-16-roadmap.md`](docs/research/2026-05-16-roadmap.md).
