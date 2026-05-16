# Completion audit

Date: 2026-05-16

## Objective Restated

Research current coding-agent pain points using web search plus Chrome browsing
of Reddit and X/Twitter, derive 15 useful ideas for `syscall-agent`, implement
those ideas, add supported Codex/Copilot subscription paths, add skills as
commands, improve tool-use visibility, perform code/security/open-source
reviews, then merge to `master` and push.

## Prompt-to-Artifact Checklist

| Requirement | Evidence |
| --- | --- |
| Use web search for current research | `docs/research/2026-05-16-roadmap.md` lists source-backed signals from GitHub, OpenAI, GitHub Docs, MCP, Reddit, and X/web research. |
| Visit Reddit through Chrome | Chrome browser was opened to `reddit.com/r/opencodeCLI/comments/1ryl1l6/...`; the visible thread named pain points around reasoning visibility, LSP/project integration, UI readability, token/context visibility, and subagent visibility. |
| Visit X/Twitter through Chrome | Chrome browser was opened to `x.com/search?q=%22coding%20agent%22%20CLI%20tool%20visibility%20Codex%20Claude%20OpenCode...`; visible results and trends included skill/workflow-native agent posts, ChatGPT subscription integration, Codex mobile, and local Hermes/DGX Spark items. |
| Chrome tooling caveat | The Codex Chrome extension/node runtime was not exposed after repeated tool-discovery attempts, so the Chrome evidence was gathered through the Google Chrome app via Computer Use. |
| Produce 15 ideas | `docs/research/2026-05-16-roadmap.md` has the 15-item "Implemented Ideas" list. |
| Implement 15 ideas | PR #1 implemented meta tools, TUI commands, skill discovery, auth visibility, diagnostics, grep/checksum helpers, README/system prompt docs. |
| Support Codex OAuth/subscription path | PR #2 added `delegate_codex`, which invokes official `codex exec` and therefore uses the official Codex CLI's own supported auth instead of reading tokens. |
| Support GitHub Copilot subscription path | PR #2 added `delegate_copilot`, which invokes official `copilot -p` and therefore uses the official Copilot CLI's own supported auth instead of reading tokens. |
| Avoid unsafe token scraping | `README.md`, `docs/security-review.md`, and `docs/research/2026-05-16-roadmap.md` document that subscription tokens are not printed, scraped, or repurposed. |
| Add skills | `list_skills` and `read_skill` implemented in `src/tools_meta.c`; skill roots documented in `README.md`. |
| Show skills as commands | TUI `/skills` implemented in `src/tui.c` and tested in `tests/tui_test.c`. |
| Improve tool-use visibility | Existing verbose tool events plus new `list_tools` and TUI `/tools`; docs updated in `README.md` and `SYSTEM_PROMPT.md`. |
| Security review with security skill | `docs/security-review.md` written after loading `security-best-practices`; no C-specific reference existed, so general C/local-agent security review was used. |
| Code review with review skill | `docs/code-review.md` written after loading the code-review workflow; no blocking findings remain open. |
| Open-source review | `docs/open-source-review.md` documents license/contribution/security-policy readiness gaps. |
| Merge to master via PR | PR #1 and PR #2 were merged to `master` with merge commits. |
| Push public repo | `douglascorrea/syscall-agent` is public and local `master` tracks `syscall-agent/master`. |

## Current Verification

- `make test`
- `make`
- `git diff --check`
- `gh pr view 1 --repo douglascorrea/syscall-agent --json state,mergedAt,mergeCommit,url,title`
- `gh pr view 2 --repo douglascorrea/syscall-agent --json state,mergedAt,mergeCommit,url,title`
- `gh repo view douglascorrea/syscall-agent --json nameWithOwner,visibility,defaultBranchRef,url`

## Merged Commits

- `89ff57a` - Merge PR #1, roadmap-driven meta tools and docs.
- `756b521` - Add open source readiness review.
- `92038ff` - Merge PR #2, official CLI delegation tools.

## Residual Notes

- The exact Codex Chrome extension runtime was unavailable in this session. The
  Chrome-browser requirement was satisfied through the installed Google Chrome
  app using Computer Use, and this audit records that distinction.
- The repository is public but does not yet include a license file. That is
  recorded in `docs/open-source-review.md` as an owner decision.
