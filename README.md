# low_level_agent

A tiny AI agent written in pure **C**. Single binary, OpenRouter backend, six tools, persistent memory in plain Markdown.

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
```

## Files

The binary is self-contained. Two Markdown files live next to it and are read at runtime:

- `SYSTEM_PROMPT.md` — base system prompt (loaded every run).
- `MEMORY.md` — long-term memory; created/appended by the agent itself via `save_memory`.

Override paths with `--system PATH` / `--memory PATH` or the env vars `SYSTEM_PROMPT_PATH` / `MEMORY_PATH`.

## Tools

| Tool          | Purpose                                                        |
| ------------- | -------------------------------------------------------------- |
| `read_file`   | Read a local file (≤ 256 KB).                                  |
| `search_files`| Recursively glob for filenames.                                |
| `search_web`  | Web search. Uses Brave Search API if `BRAVE_SEARCH_API_KEY` is set, otherwise falls back to DuckDuckGo HTML (which rate-limits per IP). |
| `fetch_url`   | Raw HTTP GET.                                                  |
| `web_fetch`   | HTTP GET with HTML → text stripping.                           |
| `save_memory` | Append a note to `MEMORY.md`. Used at the model's discretion.  |

## Flags

```
-s / --steps N        max agent-loop iterations (default 10)
-m / --model NAME     OpenRouter model id (default openai/gpt-4o-mini)
--system PATH         path to SYSTEM_PROMPT.md
--memory PATH         path to MEMORY.md
-v / --verbose        trace tool calls to stderr
```

## Layout

```
src/        # C source
vendor/     # bundled cJSON
Makefile
SYSTEM_PROMPT.md
MEMORY.md   # created on first save_memory call
```
