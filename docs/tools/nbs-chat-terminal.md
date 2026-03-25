# nbs-chat-terminal: Interactive Terminal Client

The human's window into an NBS chat. A raw-mode terminal client with line editing, background message polling, slash commands for team control, and a watchdog that restarts dead agents. This is where the operator sits.

## Usage

```
nbs-chat-terminal <file> <handle> [--restart] [--goal-file=PATH]
```

| Argument | Purpose |
|----------|---------|
| `<file>` | Path to the chat file (must already exist — use `nbs-chat create` first) |
| `<handle>` | Your display name in the chat. ASCII-only — multi-byte characters break cursor positioning |
| `--restart` | Kill and restart the agent team immediately on launch |
| `--goal-file=PATH` | Inject file contents into chat before restart, then disable the auto-restart watchdog thread |

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 1 | General error |
| 2 | Chat file not found |
| 4 | Invalid arguments |

## What Happens on Startup

The terminal performs these steps in order. The order matters — a failure at any point aborts before later steps run.

1. **Validate arguments.** Handle must be ASCII-only. Chat file must exist.

2. **Goal file injection** (if `--goal-file` given). The file is validated thoroughly — must be a regular file, non-empty, under 64KB, no null bytes. Contents are posted to chat as your handle. If injection fails, the terminal aborts. No agents are launched, no state is modified beyond the chat file. This is deliberate: a half-injected goal with a running team is worse than no injection at all.

3. **Restart confirmation** (if `--restart` given). Counts alive sessions for this project via `nbs-ts list`. If any are running, prompts `N agent session(s) currently running. Restart will kill them all. Continue? [y/N]`. This happens before raw mode — it uses canonical input for the Y/N prompt.

4. **Helper check.** Looks for `~/.nbs-ts/helper.sock`. If absent, prints a warning that SSH, proxy access, and git push will not work. The helper must be started separately in a user terminal: `nbs-ts-helper`.

5. **Raw mode.** Disables echo and canonical mode, keeps ISIG so Ctrl-C still generates SIGINT. Verifies the terminal driver actually accepted the flags — POSIX permits silent rejection.

6. **Display existing messages.** Reads the chat file and renders all messages.

7. **Restart execution** (if `--restart` given). Runs the restart script synchronously, blocking until complete.

8. **Watchdog initialisation.** Always initialises the watchdog state (needed for project root resolution used by oracle commands). Only starts the auto-restart *thread* if `--goal-file` is not set — goal-file mode relies on fixup for crash recovery instead.

9. **Event loop.** Enters the main poll loop.


## Slash Commands

All commands are entered at the prompt and submitted with Enter.

### General

| Command | Action |
|---------|--------|
| `/help` | Print the command reference |
| `/exit` | Leave the chat cleanly |
| `/edit` | Open `$EDITOR` for multi-line message composition |
| `/search <pattern>` | Case-insensitive substring search across all messages |
| `/filter <handle>` | Show only messages from one participant; redisplays last 50 matching messages |
| `/filter` | Show current filter status |
| `/unfilter` | Clear filter; redisplays last 20 messages from all participants |

### Team Control

| Command | Action |
|---------|--------|
| `/pause` | Freeze the team. Creates `.nbs/control-pause`, disables watchdog, broadcasts stop order to `@team`. Agents keep context but stop receiving work |
| `/resume` | Resume a paused team. Removes pause file, re-enables watchdog, broadcasts resume order. Also recovers from `/shutdown` (which disables the watchdog without a pause file) |
| `/shutdown` | Announce shutdown with 10-second warning, then kill all agent sessions, sidecars, and nbs-claude processes for this project. Disables watchdog |
| `/restart` | Manually trigger a team restart via the restart script. Bypasses rate limiting. Double-forks to avoid blocking the terminal |

### Oracle Triggers

| Command | Role | What it does |
|---------|------|-------------|
| `/pythia` | pythia | Trajectory and risk assessment. Reads the last 500 lines of the scribe log, runs a checkpoint, posts assessment to chat |
| `/shepard` | shepard | Team effectiveness check. Lists nbs-ts sessions, captures panes, reads last 20 chat messages, posts assessment |
| `/librarian` | librarian | Institutional memory search. Reads last 100 chat messages, searches scribe log for answers to blockers, posts findings with `@team!` tag |
| `/fixup` | fixup | Diagnose and restart stalled agents. Runs `/nbs-teams-fixup` on all agents, posts summary |

All oracle commands check two conditions before spawning:
- The watchdog must be enabled (team not paused or shut down)
- The `control-pause` file must not exist

If either check fails, the command prints an error and does nothing. This prevents spawning oracles that would sit idle in a paused team.


## The `/edit` Command

Opens your `$EDITOR` (defaulting to `vim`) in a temporary file. When you save and quit, the contents are sent as a single message. Empty files or editor errors result in nothing being sent.

Editor validation: the terminal checks the editor binary against an allowlist (`vi`, `vim`, `nvim`, `nano`, `emacs`, `ed`). Unlisted editors are accepted if they contain no shell metacharacters. This prevents command injection via a malicious `$EDITOR` value.

The editor runs with a sanitised environment — only `PATH`, `HOME`, `TERM`, and `LANG` are passed through. The temp file is created with 0600 permissions in `$TMPDIR` (or `/tmp`), and is deleted after reading regardless of outcome.

On return from the editor, the terminal resets the alternate screen buffer, cursor visibility, and attributes before resuming raw mode. This handles the case where vim (or similar) entered the alternate screen.


## The `/search` Command

Searches all messages in the chat file for a case-insensitive substring match. Results are displayed with their message index (`[N]`) and full content. Shows a count of matches at the end, or "No matches found." if none.

```
alex> /search parser
  [42] test-runner> Found 3 failing tests in parser module
  [67] alex> Both of you focus on parse_int first
  2 match(es)
```


## The `/filter` and `/unfilter` Commands

Filtering changes which messages are displayed by the background poller. When a filter is active, only messages from the specified handle appear. Your own messages are always suppressed in the poll display (you see them when you send them).

`/filter <handle>` immediately redisplays the last 50 matching messages from that handle, in chronological order. This gives you context without scrolling.

`/unfilter` clears the filter and redisplays the last 20 messages from all participants.

Both commands update the global filter state (`g_filter_handle`), which the background poller checks on every cycle.


## The `/pause` and `/resume` System

Pause creates the file `.nbs/control-pause` in the project root. This file is checked by:
- The watchdog thread (skips restart evaluation when paused)
- Sidecars (skip all work when this file exists)
- Oracle spawn commands (refuse to launch when paused)

The pause file contains a Unix timestamp of when the pause was initiated.

`/resume` removes the pause file and re-enables the watchdog. It also broadcasts a resume message. Importantly, `/resume` recovers from `/shutdown` as well — shutdown disables the watchdog without creating a pause file, so `/resume` checks both the file and the watchdog's enabled state.


## The `/shutdown` Command

A hard stop. The sequence:

1. Broadcast a 10-second warning to `@team` via the chat file
2. Disable the watchdog (prevents auto-restart from fighting the shutdown)
3. Sleep 10 seconds (gives agents time to finish current actions)
4. Kill all nbs-ts sessions for this project (by chat tag)
5. Kill all sidecars matching `--root=<project_root>`
6. Kill all nbs-claude processes matching the project root

After shutdown, the terminal remains running. You can use `/resume` to re-enable the watchdog and let it restart the team, or `/restart` to restart manually.


## Watchdog System

The watchdog is a background thread that monitors agent health and auto-restarts dead teams.

### How It Works

1. Every 60 seconds (`WATCHDOG_POLL_INTERVAL_S`), the thread polls `nbs-ts list` and counts alive sessions
2. If the count drops below 3 (`WATCHDOG_MIN_ALIVE`), the watchdog considers the team dead
3. It calls `watchdog_evaluate()` — a pure state machine with no I/O — to decide what to do
4. If the decision is `WATCHDOG_RESTART`, it runs the restart script and blocks until it completes. This prevents concurrent restarts (digest takes minutes)

### Rate Limiting

The watchdog enforces limits to prevent restart loops:

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `WATCHDOG_MAX_RESTARTS` | 5 | Maximum restarts per rolling hour |
| `WATCHDOG_RATE_WINDOW_S` | 3600 | Rolling window (1 hour) |
| `WATCHDOG_COOLDOWN_S` | 120 | Minimum gap between restarts |

If the rate limit is hit, the watchdog disables itself permanently for the session. The decision enum returns `WATCHDOG_RATE_LIMITED` and the thread exits.

### `--goal-file` Disables Auto-Restart

When `--goal-file` is passed, the watchdog state is initialised (so oracle commands work, since they need `project_root`), but the auto-restart thread is never started. The terminal prints: `Auto-restart disabled (--goal-file mode: fixup handles crashes)`.

The rationale: goal-file mode is for directed sessions where the fixup oracle handles crash recovery, not the blunt instrument of full team restart.

### Project Root Resolution

The watchdog needs the project root to find restart scripts, oracle binaries, and the pause file. It resolves this by walking up the directory tree from the chat file path, looking for a `.nbs/` directory. It skips any directory that *is* named `.nbs` (that's the state directory, not the project root). Maximum depth: 10 levels.

Example: `/home/alex/myproject/.nbs/chat/live.chat` resolves to `/home/alex/myproject`.

### Restart Script Resolution

The restart script is found by checking two locations in order:
1. `<project_root>/.nbs/bin/nbs-chat-terminal-restart.sh` (installed projects)
2. `<project_root>/bin/nbs-chat-terminal-restart.sh` (framework source tree)

The script receives two arguments: `project_root` and `chat_path`.

### Pause File Awareness

The watchdog thread checks for `.nbs/control-pause` at the start of every poll cycle. If the file exists, it skips the entire evaluation — no session counting, no restart decision.


## Oracle Spawning

Oracle commands (`/pythia`, `/shepard`, `/librarian`, `/fixup`) share a single code path: `spawn_trigger_worker()`.

### Mechanism

1. Find `nbs-workers` binary (check `.nbs/bin/` then `bin/`)
2. Build a `--skill=<file>` flag from the trigger definition
3. Double-fork: parent forks an intermediate child, which forks the grandchild. Intermediate child exits immediately (parent reaps it). Grandchild is reparented to init — no zombies
4. Grandchild execs: `nbs-workers spawn <role> <project_root> --skill=<file> <task_desc>`

### Trigger Definitions

Role definitions live in `src/nbs-common/trigger_defs.h` — the single source of truth for skill files and task descriptions used by both the terminal and sidecars.

| Role | Skill file | Handle |
|------|-----------|--------|
| pythia | `commands/nbs-pythia.md` | pythia |
| shepard | `commands/nbs-shepard.md` | shepard |
| librarian | `commands/nbs-librarian.md` | librarian |
| fixup | `commands/nbs-fixup-auto.md` | fixup |


## Oracle Reaper

The terminal runs the oracle reaper as a sidecar concern. Every 10 seconds (checked on poll timeout), it executes `nbs-oracle-reaper check <project_root>` via double-fork. The reaper is stateless — it discovers oracle sessions from nbs-ts and kills any that have already posted their output to chat.

The reaper binary is resolved the same way as other binaries: `.nbs/bin/` first, then `bin/`.

The reaper only runs when the watchdog is enabled and the project root is set.


## Chat Tag Derivation

Several subsystems need a tag derived from the chat filename for session naming. The derivation is:

1. Take the basename of the chat file path
2. Strip the `.chat` suffix
3. Replace dots with dashes

Examples:
- `live.chat` becomes `live`
- `nn.Module.chat` becomes `nn-Module`
- `poem.chat` becomes `poem`

This tag is used for:
- Watchdog session counting (`nbs-ts list --name=<tag>`)
- Restart confirmation (counting alive sessions)
- Shutdown (killing sessions by tag)


## Line Editing

The terminal implements its own line editor in raw mode. No readline dependency.

### Key Bindings

| Key | Action |
|-----|--------|
| Left/Right arrows | Move cursor within line |
| Home (or `ESC [1~`) | Jump to start of line |
| End (or `ESC [4~`) | Jump to end of line |
| Backspace | Delete character before cursor |
| Delete (`ESC [3~`) | Delete character at cursor |
| Up arrow | Browse history (older) |
| Down arrow | Browse history (newer) |
| Enter | Send message |
| Ctrl-C | Exit (sends pending input first) |
| Ctrl-D | Exit (sends pending input if buffer non-empty) |
| Tab | Inserted as literal character |

### History

Input history stores the last 50 sent messages in a ring buffer. Duplicate consecutive entries are suppressed. History browsing starts from the most recent entry when Up is pressed with an empty buffer or while already browsing. Down past the newest entry clears the buffer.

### Wrapped Lines

The line editor handles terminal wrapping correctly. It tracks the cursor's visual row relative to the first row of the input area, using ANSI escape sequences to move between rows during redraw. Terminal width is queried via `ioctl(TIOCGWINSZ)`, falling back to 80 columns.

The redraw algorithm accounts for deferred wrap — when output fills exactly to the last column, the cursor stays on that column rather than advancing to the next row until the next character is printed.


## Background Polling

The event loop uses `poll()` with a 1.5-second timeout (`POLL_INTERVAL_MS = 1500`). On timeout, it calls `poll_and_display()` to check for new messages.

### Non-Destructive Display

New messages are rendered without destroying the user's in-progress input:

1. Move cursor up to the first row of the input area
2. Clear from cursor to end of screen (`ESC [J`)
3. Print new messages (skipping the user's own messages, respecting filter)
4. Redraw prompt and input buffer with cursor in the correct position

### Auto-Archive Detection

If the message count in the file is *lower* than the terminal's last-known count, the file has been archived (first N messages moved to an archive file). The terminal resets its counter and prints a notice rather than going permanently deaf.

### Message Count Tracking

After sending a message, the terminal does *not* increment its message counter. Instead, it lets the next poll read the actual count from the file. This prevents a desync bug: if another agent also sent between our send and the next poll, incrementing locally would leave us one behind, permanently skipping that agent's message.


## Bus Bridge

After every send, the terminal publishes two bus events via `bus_bridge_after_send()` and `bus_bridge_human_input()`:

- A standard `chat-message` event (same as any participant)
- A `human-input` priority signal (allows sidecars to react faster to human messages)


## Signal Handling

SIGINT and SIGTERM are caught via `sigaction`. The handler sets a flag (`g_quit`) which the event loop checks. On Ctrl-C, pending input is sent before exiting. On exit, the terminal restores the original termios state and prints "Left chat."


## Typical Session

```
$ nbs-chat-terminal .nbs/chat/live.chat alex --restart
2 agent session(s) currently running. Restart will kill them all.
Continue? [y/N] y
Restarting team...
Team restart complete.
alex> @team Focus on the parser regression today
  pythia> [checkpoint] 3 agents alive, trajectory nominal
alex> /filter pythia
  Filtering: showing only messages from pythia
  pythia> [checkpoint] Team started 14:00. 2 tasks assigned.
  pythia> [checkpoint] 3 agents alive, trajectory nominal
alex> /unfilter
  Filter cleared — showing last 20 messages
alex> /search parser
  [12] worker-1> Found regression in parse_float
  1 match(es)
alex> /pause
  Team paused. Type /resume to continue.
alex> /resume
  Team resumed.
alex> /exit
Left chat.
```
