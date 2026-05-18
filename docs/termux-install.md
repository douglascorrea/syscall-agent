# Android Termux install

These instructions install and run `cezar` directly inside Termux on
Android.

Official Termux notes that the main app and plugin apps must come from the same
source because APK signatures differ. Use F-Droid for the simplest stable setup,
or use GitHub releases consistently for both Termux and plugins. Do not mix a
Termux APK from F-Droid with a Termux:API plugin APK from GitHub.

References:

- Termux app installation: https://github.com/termux/termux-app#installation
- Termux package management: https://github.com/termux/termux-packages/wiki/Package-Management
- Termux:API app and helper package: https://github.com/termux/termux-api
- Termux:API package scripts: https://github.com/termux/termux-api-package

## 1. Install Termux

Install Termux from F-Droid:

https://f-droid.org/en/packages/com.termux/

If you use the GitHub release build instead, install Termux and all Termux
plugins from the GitHub release/debug source family only:

https://github.com/termux/termux-app/releases

## 2. Install build dependencies

Inside Termux:

```sh
pkg update
pkg upgrade
pkg install git clang make libcurl
```

If `make` later reports `curl-config: not found`, reinstall `libcurl`:

```sh
pkg install libcurl
```

## 3. Clone and build

```sh
git clone https://github.com/douglascorrea/cezar.git
cd cezar
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

## 4. Configure the model backend

`cezar` currently calls OpenRouter. Export your key before running it:

```sh
export OPENROUTER_API_KEY=sk-or-...
```

For a persistent Termux setup, add the export to `~/.bashrc` or `~/.zshrc`.

## 5. Run the agent

CLI prompt:

```sh
./build/cezar "inspect this repo and summarize the C modules"
```

Interactive TUI:

```sh
./build/cezar --tui
```

Subprocess tools are still disabled by default. Enable them only in a workspace
you trust:

```sh
./build/cezar --allow-exec --tui
```

## 6. Enable Android device integrations

Install the Termux:API Android app from the same source family as the Termux app
you installed. For F-Droid:

https://f-droid.org/en/packages/com.termux.api/

Then install the helper scripts inside Termux:

```sh
pkg install termux-api
```

For shared Android storage access:

```sh
termux-setup-storage
```

Approve the Android permission prompt. The agent's `termux_storage_status` tool
will show whether `~/storage/shared`, `~/storage/downloads`, and related links
exist.

## Termux-focused tools

`cezar` adds these Termux-aware tools. They use fixed `termux-*`
commands through `execvp`, never through a shell.

| Tool | What it does | Requires |
| --- | --- | --- |
| `termux_info` | Detect Termux environment variables and command availability. | Termux optional |
| `termux_api_status` | Check Termux:API helper command availability. | Termux optional |
| `termux_storage_status` | Check `termux-setup-storage` shared-storage links. | Termux optional |
| `termux_battery_status` | Read Android battery state. | Termux:API app + `pkg install termux-api` |
| `termux_wifi_info` | Read current Wi-Fi connection details. | Termux:API app + `pkg install termux-api` |
| `termux_clipboard_get` | Read Android clipboard text. | Termux:API app + `pkg install termux-api` |
| `termux_clipboard_set` | Set Android clipboard text. | Termux:API app + `pkg install termux-api` |
| `termux_notification` | Show an Android notification. | Termux:API app + `pkg install termux-api` |
| `termux_vibrate` | Vibrate the device for a bounded duration. | Termux:API app + `pkg install termux-api` |
| `termux_wake_lock` | Acquire or release Termux's wake lock. | Termux app |

## Troubleshooting

If package commands fail with repository errors, run `termux-change-repo`, pick a
current mirror, and then run `pkg upgrade`.

If Termux:API commands are missing, confirm both pieces are installed:

```sh
pkg install termux-api
which termux-battery-status
```

If Termux:API commands hang, open the Termux:API app once and disable battery
optimization for Termux and Termux:API in Android settings.

If Android kills long-running sessions, keep the TUI in the foreground, release
unneeded wake locks with `termux_wake_lock` action `unlock`, and avoid running
large background jobs without power attached.
