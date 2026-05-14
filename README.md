# low_level_agent

A tiny AI agent written in pure **C**. Single binary, OpenRouter backend, syscall-backed tools, persistent memory in plain Markdown.

## Build

```sh
make
```

Requires `libcurl` (preinstalled on macOS). Outputs `build/agent`.

## Run

```sh
export OPENROUTER_API_KEY=sk-or-...
# optional, otherwise search_web falls back to DuckDuckGo HTML
export BRAVE_SEARCH_API_KEY=...

./build/agent "summarize the files in this repo"
./build/agent -s 20 -m anthropic/claude-3.5-sonnet "find the largest .c file and tell me what it does"
echo "what is in MEMORY.md?" | ./build/agent

# enable subprocess tools (default sandbox: read-only FS, no network)
./build/agent --allow-exec "run 'git log -n 5 --oneline' and summarize"
```

## Files

The binary is self-contained. Two Markdown files live next to it and are read at runtime:

- `SYSTEM_PROMPT.md` — base system prompt (loaded every run).
- `MEMORY.md` — long-term memory; created/appended by the agent itself via `save_memory`. `flock`-protected, so multiple agents can write in parallel.

Override paths with `--system PATH` / `--memory PATH` or the env vars `SYSTEM_PROMPT_PATH` / `MEMORY_PATH`.

## Tools

### Always available

| Tool              | Purpose                                                                                                                              |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `read_file`       | Read a local file (≤ 256 KB).                                                                                                        |
| `search_files`    | Recursively glob for filenames.                                                                                                      |
| `search_web`      | Web search. Brave Search API (if `BRAVE_SEARCH_API_KEY`) → DuckDuckGo HTML fallback.                                                 |
| `fetch_url`       | Raw HTTP GET.                                                                                                                        |
| `web_fetch`       | HTTP GET with HTML → text stripping.                                                                                                 |
| `save_memory`     | Append a note to `MEMORY.md`. `flock`-protected.                                                                                     |
| `stat`            | File metadata (size, mode, mtime, type, symlink target) — no content read.                                                           |
| `list_dir`        | Directory listing with each entry's type/size/mtime.                                                                                 |
| `write_file`      | Atomic write via `mkstemp` + `rename` (no torn writes for concurrent readers).                                                       |
| `read_file_range` | `mmap`-backed slice read for huge files.                                                                                             |
| `dns_lookup`      | `getaddrinfo` → A / AAAA records.                                                                                                    |
| `tcp_check`       | Non-blocking `connect()` probe: reachable / unreachable + duration.                                                                  |
| `watch_path`      | Block until a path changes. macOS: `kqueue`. Linux: `inotify`. Hard cap 120 s.                                                       |
| `list_processes`  | Top-N processes by RSS. macOS: `sysctl(KERN_PROC_ALL)`. Linux: walks `/proc`.                                                        |

### Gated behind `--allow-exec`

| Tool           | Purpose                                                                                                                                                                                                                              |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `exec_command` | `fork()` + per-call sandbox + `setrlimit` defaults + `execvp(argv)`. **argv-only, no shell.** Captures stdout/stderr/exit + `peak_mem_kb` via `getrusage`.                                                                           |
| `spawn_bg`     | Same machinery, but returns a handle and runs in the background. Output buffered up to 256 KB per stream.                                                                                                                            |
| `bg_read`      | Non-blocking read from a handle. Returns offsets so the next call resumes without re-reading.                                                                                                                                        |
| `bg_kill`      | SIGTERM → SIGKILL after 2 s.                                                                                                                                                                                                         |
| `bg_list`      | What's still running and how much output is buffered.                                                                                                                                                                                |

### Sandbox profiles (per `exec_command` / `spawn_bg` call)

Applied between `fork()` and `execvp()` via `sandbox_init(3)` on macOS (Linux is a stub for now).

| Profile    | FS read | FS write           | Network          | Notes                                                          |
| ---------- | ------- | ------------------ | ---------------- | -------------------------------------------------------------- |
| `readonly` | yes     | `/dev/null` + tmp  | denied           | Safest. The command runs but can't write or talk to the net.   |
| `default`  | yes     | `/dev/null` + tmp  | denied           | Default. Adds full process spawning for shell-driven workflows.|
| `network`  | yes     | `/dev/null` + tmp  | allowed          | For curl, git fetch, dev servers, etc.                         |
| `build`    | yes     | everywhere         | denied           | For make / compile loops. Best-effort.                         |
| `none`     | —       | —                  | —                | No sandbox. Requires `--allow-unsafe-exec`.                    |

### Resource limits applied to every exec/spawn

`RLIMIT_CPU=30s`, `RLIMIT_AS=512 MB`, `RLIMIT_FSIZE=256 MB`, `RLIMIT_NOFILE=256`.

## Flags

```
-s / --steps N        max agent-loop iterations (default 10)
-m / --model NAME     OpenRouter model id (default openai/gpt-4o-mini)
--system PATH         path to SYSTEM_PROMPT.md
--memory PATH         path to MEMORY.md
--allow-exec          enable exec_command, spawn_bg, bg_read, bg_kill, bg_list
--allow-unsafe-exec   also allow profile='none' (no sandbox). Implies --allow-exec.
-v / --verbose        trace tool calls to stderr
```

`LLA_ALLOW_EXEC=1` and `LLA_ALLOW_UNSAFE_EXEC=1` work the same way.

## Layout

```
src/
  main.c                 # CLI entry + flag parsing
  agent.c                # agent loop, message history, timing metadata
  openrouter.c           # OpenRouter chat completions client
  tools.c                # tool registration + top-level dispatch
  tools_fs.c             # stat, list_dir, write_file, read_file_range
  tools_proc.c           # exec_command, spawn_bg/read/kill/list, list_processes
  tools_watch.c          # watch_path
  tools_net.c            # dns_lookup, tcp_check
  os_compat.h            # cross-platform shim
  os_compat_darwin.c     # kqueue, sandbox_init, sysctl
  os_compat_linux.c      # inotify, /proc walker, seccomp stub
  memory.c               # MEMORY.md read/append with flock
  http.c                 # libcurl wrapper
  util.c                 # Buf, URL encode, HTML strip
vendor/cJSON.c           # JSON library
Makefile
SYSTEM_PROMPT.md
MEMORY.md                # created on first save_memory call
```
