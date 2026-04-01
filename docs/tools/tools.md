# NBS Tools Reference

This document defines every tool available in `~/.nbs/bin/`. EACH tool entry specifies purpose, usage, arguments, exit codes, a copy-pasteable example, and when to use the tool. The document is written in Precise Technical English (PTE) with Honest type definitions for structured contracts.

---

## Team Monitoring and Control

These tools check team health and control agent lifecycle. The operator SHOULD use these tools to diagnose problems and restart misbehaving agents. The `nbs-chat-terminal` slash commands provide the same functionality from inside the chat client.

### nbs-team-check

**Purpose:** Report the health status of every agent in a running NBS team.

**Usage:**
```bash
nbs-team-check <chat-tag> <project-root>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<chat-tag>` | Yes | The chat tag derived from the chat filename (e.g. `live` from `live.chat`) |
| `<project-root>` | Yes | Absolute path to the project directory containing `.nbs/` |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | All agents healthy |
| 1 | ONE OR MORE agents unhealthy |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-team-check nsb-gw /home/user/nbs-tools-rewrite
```

**When to use:** Use `nbs-team-check` to verify whether the supervisor and all agents are alive. IF an agent appears stuck or silent, THEN run `nbs-team-check` BEFORE attempting a restart. The `/health` slash command in `nbs-chat-terminal` calls this tool.

---

### nbs-kick-agent

**Purpose:** Hard-restart a single agent without affecting the rest of the team.

**Usage:**
```bash
nbs-kick-agent <agent> <project-root> <chat-file>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<agent>` | Yes | Agent role to restart (e.g. `supervisor`, `generalist`, `scribe`, `testkeeper`) |
| `<project-root>` | Yes | Absolute path to the project directory containing `.nbs/` |
| `<chat-file>` | Yes | Path to the chat file for the team |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Agent restarted |
| 1 | Restart failed |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-kick-agent generalist /home/user/myproject .nbs/chat/live.chat
```

**When to use:** Use `nbs-kick-agent` to restart a single stalled agent. IF only one agent is misbehaving, THEN use `nbs-kick-agent` instead of restarting the entire team. The `/kick <agent>` slash command in `nbs-chat-terminal` calls this tool.

---

### nbs-chat-terminal

**Purpose:** Provide an interactive terminal client for human participation in NBS chat with team control slash commands.

**Usage:**
```bash
nbs-chat-terminal <file> <handle> [--restart] [--goal-file=PATH] [--no-restart] [--highlight-mention]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file (MUST already exist) |
| `<handle>` | Yes | Display name in the chat (ASCII only) |
| `--restart` | No | Kill and restart the agent team on launch |
| `--goal-file=PATH` | No | Inject file contents into chat as the session goal BEFORE restart |
| `--no-restart` | No | Disable watchdog auto-restart (manual restarts only) |
| `--highlight-mention` | No | Render `@<handle>` mentions with inverse video |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 1 | General error |
| 2 | Chat file not found |
| 4 | Invalid arguments |

#### Slash Commands

| Command | Action |
|---------|--------|
| `/help` | Print the command reference |
| `/exit` | Leave the chat cleanly |
| `/edit` | Open `$EDITOR` for multi-line message composition |
| `/search <pattern>` | Case-insensitive substring search across all messages |
| `/filter <handle>` | Show only messages from one participant |
| `/unfilter` | Clear the filter |
| `/mention <handle>` | Show only messages that `@handle` the specified participant |
| `/unmention` | Clear the mention filter |
| `/redraw` | Clear screen and repaint messages |
| `/health` | Report per-agent session and sidecar status via `nbs-team-check` |
| `/kick <agent>` | Hard-restart a single agent |
| `/pause` | Freeze the team (creates `.nbs/control-pause`, disables watchdog) |
| `/resume` | Resume a paused or shut-down team |
| `/shutdown` | Announce shutdown, wait 10 seconds, kill all agent sessions |
| `/restart` | Manually trigger a full team restart |
| `/pythia` | Spawn a Pythia trajectory and risk assessment |
| `/shepard` | Spawn a Shepard team effectiveness check |
| `/librarian` | Spawn a Librarian institutional memory search |
| `/fixup` | Spawn a Fixup diagnostic on all agents |

**Example:**
```bash
nbs-chat-terminal .nbs/chat/live.chat alex --restart --goal-file=goal.md
```

**When to use:** Use `nbs-chat-terminal` to join or start an NBS team session as a human operator. This is the primary entry point for human interaction with an agent team. IF you want to monitor agents without auto-restart, use `--no-restart`.

---

## Chat and Communication

These tools read from and write to NBS chat files. Chat files use atomic locking for safe concurrent access by multiple agents and the human operator.

### nbs-chat

**Purpose:** Perform file-based AI-to-AI chat operations with atomic locking.

**Usage:**
```bash
nbs-chat <command> [args...]
```

**Exit Codes (all subcommands):**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | File not found or already exists |
| 3 | Timeout (poll only) |
| 4 | Invalid arguments |

#### nbs-chat create

**Purpose:** Create a new chat file.

**Usage:**
```bash
nbs-chat create <file>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path for the new chat file |

**Example:**
```bash
nbs-chat create .nbs/chat/experiment.chat
```

**When to use:** Use `nbs-chat create` to initialise a new chat channel. `nbs-chat-init` calls this automatically during project bootstrap.

#### nbs-chat send

**Purpose:** Send a message to a chat file.

**Usage:**
```bash
nbs-chat send <file> <handle> <message>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `<handle>` | Yes | Sender's display name |
| `<message>` | Yes | Message text |

**Example:**
```bash
nbs-chat send .nbs/chat/live.chat supervisor "@team Build complete, all tests pass."
```

**When to use:** Use `nbs-chat send` for programmatic message posting. Agents use this to communicate with the team.

#### nbs-chat read

**Purpose:** Read messages from a chat file with filtering options.

**Usage:**
```bash
nbs-chat read <file> [--last=N] [--since=<handle>] [--unread=<handle>] [--after=<time>] [--before=<time>]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `--last=N` | No | Show only the last N messages |
| `--since=<handle>` | No | Show messages after the last message from this handle |
| `--unread=<handle>` | No | Show messages after the read cursor for this handle (auto-advances cursor) |
| `--after=<time>` | No | Show messages after the specified time |
| `--before=<time>` | No | Show messages before the specified time |

**Example:**
```bash
nbs-chat read .nbs/chat/live.chat --unread=supervisor
```

**When to use:** Use `nbs-chat read` to check for new messages. The `--unread` flag is the standard way for agents to poll — it tracks a per-handle cursor and auto-advances.

#### nbs-chat search

**Purpose:** Search message history for a pattern.

**Usage:**
```bash
nbs-chat search <file> <pattern> [--handle=<name>] [--after=<time>] [--before=<time>] [--include-archives]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `<pattern>` | Yes | Search pattern (literal substring) |
| `--handle=<name>` | No | Restrict search to messages from this handle |
| `--after=<time>` | No | Restrict to messages after this time |
| `--before=<time>` | No | Restrict to messages before this time |
| `--include-archives` | No | Search archived chat files in addition to the live file |

**Example:**
```bash
nbs-chat search .nbs/chat/live.chat "build failure" --handle=generalist
```

**When to use:** Use `nbs-chat search` to find past messages about a specific topic. The Librarian uses this to find answers for stuck agents.

#### nbs-chat poll

**Purpose:** Block until a new message arrives in the chat file.

**Usage:**
```bash
nbs-chat poll <file> <handle> [--timeout=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `<handle>` | Yes | Handle to poll for (cursor-based) |
| `--timeout=N` | No | Timeout in seconds (default: 10) |

**Example:**
```bash
nbs-chat poll .nbs/chat/live.chat supervisor --timeout=30
```

**When to use:** Use `nbs-chat poll` to wait for new messages without busy-looping. Returns exit code 3 on timeout.

#### nbs-chat export

**Purpose:** Export chat messages with ANSI colour formatting and filtering.

**Usage:**
```bash
nbs-chat export <file> [--last=N] [--from=N] [--to=N] [--handle=h1,h2] [--after=<time>] [--before=<time>] [--grep=<pattern>]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `--last=N` | No | Show only the last N messages |
| `--from=N` | No | Start from message N (0-based) |
| `--to=N` | No | End at message N (exclusive) |
| `--handle=h1,h2` | No | Only messages from these handles |
| `--after=<time>` | No | Messages after this time |
| `--before=<time>` | No | Messages before this time |
| `--grep=<pattern>` | No | Only messages matching this pattern |

**Example:**
```bash
nbs-chat export .nbs/chat/live.chat --last=50 --handle=supervisor,generalist
```

**When to use:** Use `nbs-chat export` to extract formatted chat logs for review or archiving.

#### nbs-chat warn

**Purpose:** Post a `[MEDIC-WARNING]` message to a chat file.

**Usage:**
```bash
nbs-chat warn <file> <message>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `<message>` | Yes | Warning text |

**Example:**
```bash
nbs-chat warn .nbs/chat/live.chat "Agent supervisor has not responded in 5 minutes"
```

**When to use:** Use `nbs-chat warn` for medic-level warnings. The bracket handle `[MEDIC-WARNING]` renders in terracotta and cannot be produced by `nbs-chat send`.

#### nbs-chat error

**Purpose:** Post a `[SIDECAR-ERROR]` message to a chat file.

**Usage:**
```bash
nbs-chat error <file> <message>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `<message>` | Yes | Error text |

**Example:**
```bash
nbs-chat error .nbs/chat/live.chat "Bus publish failed: events directory missing"
```

**When to use:** Use `nbs-chat error` for sidecar-level errors. The bracket handle `[SIDECAR-ERROR]` renders in dusty red and cannot be produced by `nbs-chat send`.

#### nbs-chat participants

**Purpose:** List all participants in a chat file with message counts.

**Usage:**
```bash
nbs-chat participants <file>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |

**Example:**
```bash
nbs-chat participants .nbs/chat/live.chat
```

**When to use:** Use `nbs-chat participants` to see who has posted to a chat and how many messages each participant has sent.

#### nbs-chat delete

**Purpose:** Delete messages from a chat file after a specified time.

**Usage:**
```bash
nbs-chat delete <file> --after=<time> [--dry-run]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |
| `--after=<time>` | Yes | Delete messages at or after this time |
| `--dry-run` | No | Show what the tool would delete without modifying the file |

**Example:**
```bash
nbs-chat delete .nbs/chat/live.chat --after=2h --dry-run
```

**When to use:** Use `nbs-chat delete` to prune old messages. Always use `--dry-run` first to verify what the tool will remove.

---

### nbs-chat-edit

**Purpose:** Open a chat file in `$EDITOR` for manual inspection or repair.

**Usage:**
```bash
nbs-chat-edit <file>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to the chat file |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 4 | Invalid arguments or file not found |

**Example:**
```bash
nbs-chat-edit .nbs/chat/live.chat
```

**When to use:** Use `nbs-chat-edit` to inspect or manually repair a corrupted chat file. This tool handles the base64 encoding and locking that direct editing would bypass.

---

### nbs-chat-remote

**Purpose:** Execute `nbs-chat` commands on a remote machine via SSH.

**Usage:**
```bash
nbs-chat-remote <command> [args...]
```

Commands mirror `nbs-chat`: `create`, `send`, `read`, `poll`, `participants`, `help`.

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<command>` | Yes | The nbs-chat subcommand to execute remotely |
| Environment: `NBS_CHAT_HOST` | Yes | SSH target (e.g. `user@server`) |
| Environment: `NBS_CHAT_PORT` | No | SSH port (default: 22) |
| Environment: `NBS_CHAT_KEY` | No | Path to SSH identity file |
| Environment: `NBS_CHAT_BIN` | No | Remote nbs-chat path (default: `nbs-chat`) |
| Environment: `NBS_CHAT_OPTS` | No | Comma-separated SSH `-o` options |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error (including SSH failures) |
| 2 | File not found or already exists |
| 3 | Timeout (poll only) |
| 4 | Invalid arguments or missing configuration |

**Example:**
```bash
NBS_CHAT_HOST=user@remote-host nbs-chat-remote read /home/user/project/.nbs/chat/live.chat --last=10
```

**When to use:** Use `nbs-chat-remote` to read or write to chat files on a remote machine. IF the project `.nbs/` directory lives on a remote host, THEN use `nbs-chat-remote` instead of `nbs-chat`.

---

### nbs-chat-repair

**Purpose:** Diagnose and repair a corrupted chat file.

**Usage:**
```bash
nbs-chat-repair <chat-file> [--dry-run]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<chat-file>` | Yes | Path to the chat file to repair |
| `--dry-run` | No | Report problems without modifying the file |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success (or no problems found) |
| 1 | Repair failed |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-chat-repair .nbs/chat/live.chat --dry-run
```

**When to use:** Use `nbs-chat-repair` IF `nbs-chat read` reports errors or IF messages appear garbled. Always run with `--dry-run` first.

---

## Session Management

These tools manage PTY sessions via the `nbs-ts-helper` daemon. Sessions provide persistent terminal processes that agents and tools interact with by sending input and reading output. Most agents SHOULD use the higher-level `nbs-remote-*` and `nbs-local-*` tools instead of calling `nbs-ts` directly.

```pascal
type
  SessionStatus = (Alive, Dead);

  SessionInfo = record
    handle  : String;         { 8-character hex identifier }
    status  : SessionStatus;
    name    : String;         { optional metadata, '-' if unnamed }
    command : String;         { the command that created the session }
  end;

  SessionList = sequence of SessionInfo;
```

### nbs-ts

**Purpose:** Create and manage PTY sessions through the `nbs-ts-helper` daemon.

**Usage:**
```bash
nbs-ts <command> [options]
```

**Exit Codes (all subcommands):**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Error |
| 2 | Not found |
| 3 | Timeout |
| 4 | Invalid arguments |

#### nbs-ts create

**Purpose:** Create a new PTY session running a command.

**Usage:**
```bash
nbs-ts create [--name=NAME] <command>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--name=NAME` | No | Session name for debugging (alphanumeric, max 64 chars) |
| `<command>` | Yes | Command to run in the session |

**Example:**
```bash
nbs-ts create --name=nbs-supervisor-build 'ssh user@remote-host'
```

**When to use:** Use `nbs-ts create` to start a persistent process. Higher-level tools (`nbs-remote-session`, `nbs-local-session`) call this automatically. Use `nbs-ts create` directly only IF no higher-level tool fits.

#### nbs-ts send

**Purpose:** Send text to a session's stdin.

**Usage:**
```bash
nbs-ts send <handle> <text>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle (8 hex chars) |
| `<text>` | Yes | Text to send (a newline is appended automatically) |

**Example:**
```bash
nbs-ts send a1b2c3d4 'make -j8'
```

**When to use:** Use `nbs-ts send` to send commands to a session created by `nbs-ts create`, `nbs-remote-session`, or `nbs-local-session`.

#### nbs-ts read-new

**Purpose:** Read new output from a session since the last read.

**Usage:**
```bash
nbs-ts read-new <handle> [--strip]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |
| `--strip` | No | Remove ANSI escape sequences from output |

**Example:**
```bash
nbs-ts read-new a1b2c3d4 --strip
```

**When to use:** Use `nbs-ts read-new` to poll for new output. The `--strip` flag removes colour codes for cleaner parsing.

#### nbs-ts read

**Purpose:** Read output from a session at a specific offset or the last N lines.

**Usage:**
```bash
nbs-ts read <handle> [--offset=N|--last=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |
| `--offset=N` | No | Read from byte offset N |
| `--last=N` | No | Read the last N lines |

**Example:**
```bash
nbs-ts read a1b2c3d4 --last=50
```

**When to use:** Use `nbs-ts read` to retrieve historical output. Use `--last=N` for recent output; use `--offset=N` for precise positioning.

#### nbs-ts status

**Purpose:** Check whether a session is alive or dead and show the exit code.

**Usage:**
```bash
nbs-ts status <handle>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |

**Example:**
```bash
nbs-ts status a1b2c3d4
```

**When to use:** Use `nbs-ts status` to check whether a long-running command has finished.

#### nbs-ts exit-code

**Purpose:** Retrieve the exit code of a completed session.

**Usage:**
```bash
nbs-ts exit-code <handle>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |

**Example:**
```bash
nbs-ts exit-code a1b2c3d4
```

**When to use:** Use `nbs-ts exit-code` AFTER `nbs-ts status` reports the session as dead. This returns the process exit code.

#### nbs-ts list

**Purpose:** List all sessions, optionally filtered by name.

**Usage:**
```bash
nbs-ts list [--name=PATTERN]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--name=PATTERN` | No | Filter sessions by name substring |

Output format: `handle\tstatus\tname\tcommand`

**Example:**
```bash
nbs-ts list --name=nbs-supervisor
```

**When to use:** Use `nbs-ts list` to see all running sessions. The watchdog and `/health` command use this to count alive agents.

#### nbs-ts find

**Purpose:** Find a session by exact name and print its handle.

**Usage:**
```bash
nbs-ts find <name>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<name>` | Yes | Exact session name to find |

**Example:**
```bash
nbs-ts find nbs-live-supervisor
```

**When to use:** Use `nbs-ts find` to resolve a session name to a handle. Returns exit code 2 IF the session does not exist.

#### nbs-ts kill

**Purpose:** Terminate a session and clean up its resources.

**Usage:**
```bash
nbs-ts kill <handle>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |

**Example:**
```bash
nbs-ts kill a1b2c3d4
```

**When to use:** Use `nbs-ts kill` to stop a session that is no longer needed. Higher-level tools call this automatically on cleanup.

#### nbs-ts gc

**Purpose:** Remove dead sessions older than a threshold.

**Usage:**
```bash
nbs-ts gc [--hours=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--hours=N` | No | Age threshold in hours (default: 4) |

**Example:**
```bash
nbs-ts gc --hours=2
```

**When to use:** Use `nbs-ts gc` to clean up stale sessions. Run periodically to prevent resource leaks.

#### nbs-ts attach

**Purpose:** Tail a session's output in real time (human viewer).

**Usage:**
```bash
nbs-ts attach <handle>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |

**Example:**
```bash
nbs-ts attach a1b2c3d4
```

**When to use:** Use `nbs-ts attach` to watch a session's output live. This is a read-only view — use `nbs-ts send` to interact.

#### nbs-ts wait-complete

**Purpose:** Block until a command completes in a session.

**Usage:**
```bash
nbs-ts wait-complete <handle> [--timeout=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |
| `--timeout=N` | No | Timeout in seconds |

**Example:**
```bash
nbs-ts wait-complete a1b2c3d4 --timeout=120
```

**When to use:** Use `nbs-ts wait-complete` to wait for a command to finish. This detects completion via `PROMPT_COMMAND` signalling — it does NOT detect session exit. Use `nbs-ts exit-code` for that.

#### nbs-ts wait-pattern

**Purpose:** Block until a pattern appears in a session's output.

**Usage:**
```bash
nbs-ts wait-pattern <handle> <pattern> [--timeout=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Session handle |
| `<pattern>` | Yes | Pattern to wait for (literal string) |
| `--timeout=N` | No | Timeout in seconds |

**Example:**
```bash
nbs-ts wait-pattern a1b2c3d4 'Build succeeded' --timeout=300
```

**When to use:** Use `nbs-ts wait-pattern` to wait for specific output (e.g. a build success message). Returns exit code 3 on timeout.

---

### nbs-ts-sysctl

**Purpose:** Manage the `nbs-ts-helper` systemd user service.

**Usage:**
```bash
nbs-ts-sysctl <command>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<command>` | Yes | One of: `install`, `remove`, `start`, `stop`, `restart`, `status`, `doctor`, `help` |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Operation failed |
| 4 | Invalid arguments |

| Subcommand | Action |
|------------|--------|
| `install` | Install and start the systemd user service (`--reinstall` to replace) |
| `remove` | Stop and remove the service |
| `start` | Start the service |
| `stop` | Stop the service |
| `restart` | Restart the service (use after rebuilding the binary) |
| `status` | Show service status, PID, socket, and recent logs |
| `doctor` | Run health checks with pass/fail reporting |

**Example:**
```bash
nbs-ts-sysctl status
```

**When to use:** Use `nbs-ts-sysctl` to manage the `nbs-ts-helper` daemon. IF `nbs-ts` reports "helper not running", THEN run `nbs-ts-sysctl start` or `nbs-ts-sysctl doctor` to diagnose.

---

### nbs-ts-helper

**Purpose:** Provide the PTY session daemon that `nbs-ts` communicates with via Unix socket.

`nbs-ts-helper` is a compiled C binary. It listens on `~/.nbs-ts/helper.sock` and manages PTY sessions. `nbs-ts-sysctl install` starts it automatically; operators MAY start it manually for debugging.

**Usage:**
```bash
nbs-ts-helper
```

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Clean shutdown |
| 1 | Failed to start (socket bind error, another instance running) |

**Example:**
```bash
nbs-ts-helper
```

**When to use:** Operators SHOULD NOT run `nbs-ts-helper` directly in normal use. Use `nbs-ts-sysctl` instead. Run `nbs-ts-helper` directly only for debugging.

---

### nbs-ts-grep

**Purpose:** Search across nbs-ts session logs for a pattern, scoped by chat tag and agent.

**Usage:**
```bash
nbs-ts-grep <pattern> <chat-tag> <agent|--all> [--from=N] [--to=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<pattern>` | Yes | Search pattern |
| `<chat-tag>` | Yes | Chat tag to scope the search |
| `<agent>` | Yes | Agent name or `--all` for all agents |
| `--from=N` | No | Start from line N |
| `--to=N` | No | End at line N |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Matches found |
| 1 | No matches |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-ts-grep "error" live --all
```

**When to use:** Use `nbs-ts-grep` to search for errors or patterns across all agent session logs. This is useful for post-incident investigation.

---

### nbs-ts-query

**Purpose:** Extract a range of lines from an agent's nbs-ts session log.

**Usage:**
```bash
nbs-ts-query <chat-tag> <agent> --from=N --to=N
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<chat-tag>` | Yes | Chat tag to scope the query |
| `<agent>` | Yes | Agent name |
| `--from=N` | Yes | Start line |
| `--to=N` | Yes | End line |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Error |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-ts-query live supervisor --from=100 --to=200
```

**When to use:** Use `nbs-ts-query` to extract a specific range of an agent's session log for review.

---

### nbs-ts-render

**Purpose:** Process raw PTY output through a virtual terminal emulator and output plain UTF-8 text.

**Usage:**
```bash
nbs-ts-render [--width=N] [--height=N] < input.log
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--width=N` | No | Screen width in columns (default: 80) |
| `--height=N` | No | Screen height in rows (default: 24) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Runtime error |
| 4 | Invalid arguments |

**Example:**
```bash
cat /tmp/nbs-ts-*/output.log | nbs-ts-render --width=120 --height=40
```

**When to use:** Use `nbs-ts-render` to convert raw terminal output (with escape sequences) into readable plain text. This is useful for processing session logs that contain cursor movement and colour codes. Use `--no-strip` to preserve colour/style information in the output.

---

### nbs-md-viewer

Terminal markdown viewer. Renders markdown with full styling (headings, tables, code, lists) in a scrollable pager.

```bash
nbs-md-viewer < document.md
cat README.md | nbs-md-viewer
nbs-md-viewer --width=100 < document.md
```

**Features:** Paragraph reflow, Unicode box drawing tables, syntax highlighting (C, C++, JS, TS, Python, Pascal), nested lists, blockquotes, horizontal panning for wide content, bidirectional text support.

**Keys:** Arrow keys scroll, Page Up/Down for pages, Home/End for top/bottom, Left/Right to pan wide tables, `h` for help, `q` to quit.

**When to use:** Use `nbs-md-viewer` to read markdown documents in the terminal with proper formatting and colour. Especially useful for viewing plan documents, specifications, and documentation.

---

## Remote Machine Access

These tools run commands and manage files on remote machines via SSH. They handle session creation, SSH connection, and cleanup automatically. Agents SHOULD use these tools instead of calling `nbs-ts` directly for remote work.

### nbs-remote-run

**Purpose:** Run a single command on a remote machine and return the output.

**Usage:**
```bash
nbs-remote-run <host> [--cwd=PATH] [--timeout=N] <command>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<host>` | Yes | SSH hostname or `user@hostname` |
| `--cwd=PATH` | No | Working directory on the remote machine |
| `--timeout=N` | No | Timeout in seconds (default: 300) |
| `<command>` | Yes | Command to run (single quoted string) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Command ran (check output for command-level errors) |
| 1 | SSH connection failed |
| 2 | Invalid arguments |
| 3 | Command timed out |

**Example:**
```bash
nbs-remote-run remote-host --cwd=/home/user/project 'make -j8'
```

**When to use:** Use `nbs-remote-run` for one-shot commands on a remote machine. It creates an ephemeral session, runs the command, captures output, and cleans up. IF you need to run multiple commands interactively, use `nbs-remote-session` instead.

---

### nbs-remote-session

**Purpose:** Create a persistent SSH shell on a remote machine.

**Usage:**
```bash
nbs-remote-session <host> [--name=NAME] [--cwd=PATH]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<host>` | Yes | SSH hostname or `user@hostname` |
| `--name=NAME` | No | Session name for debugging |
| `--cwd=PATH` | No | Working directory after connecting |

Prints the session handle to stdout. Use the handle with `nbs-ts send`, `nbs-ts read-new`, and `nbs-ts kill`.

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Session created (handle printed to stdout) |
| 1 | SSH connection failed |
| 2 | Invalid arguments |

**Example:**
```bash
HANDLE=$(nbs-remote-session remote-host --name=build --cwd=/home/user/project)
nbs-ts send "$HANDLE" 'make -j8'
nbs-ts read-new "$HANDLE" --strip
nbs-ts kill "$HANDLE"
```

**When to use:** Use `nbs-remote-session` for interactive remote work that requires multiple commands. IF you only need to run one command, use `nbs-remote-run` instead.

---

### nbs-remote-read

**Purpose:** Read a file (or part of it) on a remote machine via SCP.

**Usage:**
```bash
nbs-remote-read <server> <remote-path> [--head=N] [--tail=N] [--lines=M-N] [--grep=PAT]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<server>` | Yes | SSH hostname or `user@hostname` (NOT an nbs-ts handle) |
| `<remote-path>` | Yes | Absolute path to the file on the remote machine |
| `--head=N` | No | Read the first N lines |
| `--tail=N` | No | Read the last N lines |
| `--lines=M-N` | No | Read lines M through N |
| `--grep=PAT` | No | Show only lines matching the pattern |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | File not found |
| 3 | Transfer failed |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-remote-read remote-host /home/user/project/build.log --tail=50
```

**When to use:** Use `nbs-remote-read` to glance at remote files — code, logs, or config — without downloading them. IF you need to edit a file, use `nbs-remote-edit pull` instead.

---

### nbs-remote-edit

**Purpose:** Download, edit, and upload files on a remote machine via SCP.

**Usage:**
```bash
nbs-remote-edit <subcommand> <server> <remote-path>
```

| Subcommand | Action |
|------------|--------|
| `pull` | Download a remote file to the local staging directory |
| `push` | Upload the edited file back to the remote machine |
| `diff` | Show the diff between the local and remote versions |

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<server>` | Yes | SSH hostname or `user@hostname` (NOT an nbs-ts handle) |
| `<remote-path>` | Yes | Absolute path to the file on the remote machine |
| Environment: `NBS_REMOTE_EDIT_DIR` | No | Local staging directory (default: `.nbs/remote-edit`) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | File not found (remote or local) |
| 3 | Transfer failed |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-remote-edit pull remote-host /home/user/project/src/main.cpp
# Edit locally with the Edit tool
nbs-remote-edit diff remote-host /home/user/project/src/main.cpp
nbs-remote-edit push remote-host /home/user/project/src/main.cpp
```

**When to use:** Use `nbs-remote-edit` to modify remote files safely. The pull/edit/diff/push workflow prevents sed corruption and gives you a local diff review before pushing.

---

### nbs-remote-build

**Purpose:** Run a build command on a remote nbs-ts session while checking chat between polls.

**Usage:**
```bash
nbs-remote-build <session> <build-command> [--prompt=PATTERN] [--timeout=N] [--poll=N] [--chat=FILE --handle=NAME] [--quiet]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<session>` | Yes | nbs-ts session handle or session name |
| `<build-command>` | Yes | Build command to run |
| `--prompt=PATTERN` | No | Shell prompt pattern for completion detection (default: `\$[[:space:]]*$`) |
| `--timeout=N` | No | Build timeout in seconds (default: 300) |
| `--poll=N` | No | Poll interval in seconds (default: 5) |
| `--chat=FILE` | No | Chat file to check between polls |
| `--handle=NAME` | No | Chat handle for `--unread` (required IF `--chat` is set) |
| `--quiet` | No | Suppress progress dots |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Build completed (prompt detected) |
| 1 | General error |
| 2 | Session not found |
| 3 | Build timed out |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-remote-build my-server 'cmake --build build -j8' --chat=.nbs/chat/live.chat --handle=generalist --timeout=600
```

**When to use:** Use `nbs-remote-build` for long-running builds. It solves the "build-time chat blindness" problem — agents using plain `nbs-ts` block on sleep and cannot read chat messages until the build finishes. IF the build takes more than 30 seconds, use `nbs-remote-build` instead of `nbs-ts send` followed by `sleep`.

---

### nbs-remote-diff

**Purpose:** Pull `git diff` output from a remote nbs-ts session.

**Usage:**
```bash
nbs-remote-diff <session> [--path=PATH] [--stat] [--staged] [--commit=REF] [--cwd=DIR] [--chat=FILE --handle=NAME] [--last=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<session>` | Yes | nbs-ts session handle or name |
| `--path=PATH` | No | Restrict diff to a specific file or directory |
| `--stat` | No | Show diffstat only |
| `--staged` | No | Show staged changes (`git diff --cached`) |
| `--commit=REF` | No | Diff against a specific commit (e.g. `HEAD~1`) |
| `--cwd=DIR` | No | Working directory on the remote machine |
| `--chat=FILE` | No | Post the diff to this chat file |
| `--handle=NAME` | No | Chat handle (required with `--chat`) |
| `--last=N` | No | Lines of scrollback to capture (default: 200) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 2 | Session not found |
| 3 | Timeout |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-remote-diff my-server --cwd=/home/user/project --stat
```

**When to use:** Use `nbs-remote-diff` to review changes on a remote machine without downloading files. IF you want to share the diff with the team, use `--chat`.

---

### nbs-remote-status

**Purpose:** Retrieve HEAD commit, branch, and modified files from a remote nbs-ts session.

**Usage:**
```bash
nbs-remote-status <session> [--cwd=DIR] [--chat=FILE --handle=NAME] [--last=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<session>` | Yes | nbs-ts session handle or name |
| `--cwd=DIR` | No | Working directory on the remote machine |
| `--chat=FILE` | No | Post status to this chat file |
| `--handle=NAME` | No | Chat handle (required with `--chat`) |
| `--last=N` | No | Lines of scrollback to capture (default: 100) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 2 | Session not found |
| 3 | Timeout |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-remote-status my-server --cwd=/home/user/project
```

**When to use:** Use `nbs-remote-status` for a quick overview of the remote repository state. It combines `git log`, `git status`, and `git diff --stat` into one command.

---

### nbs-remote-git

**Purpose:** Synchronise git repositories between this machine and a remote host via SSH.

**Usage:**
```bash
nbs-remote-git <host> <remote-path> <command> [args]
```

**Commands:**

| Command | Arguments | Description |
|---------|-----------|-------------|
| `clone` | `<local-path>` | Clone the remote repository to a local path |
| `push` | `[branch]` | Push local commits to the remote (default: HEAD) |
| `pull` | `[branch]` | Fetch from remote, or pull a specific branch |
| `status` | — | Show the remote repository's recent commits and working tree |

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<host>` | Yes | Remote hostname (e.g. `build-server.example.com`) |
| `<remote-path>` | Yes | Absolute path to the git repository on the remote machine |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Operation failed (git error, SSH failure, not a repository) |
| 4 | Invalid arguments |

**Example:**
```bash
# Clone a repo from a remote build machine
nbs-remote-git build-server.example.com /home/user/project clone /local/project

# Push changes
cd /local/project
nbs-remote-git build-server.example.com /home/user/project push my-branch

# Check remote status
nbs-remote-git build-server.example.com /home/user/project status
```

**When to use:** Use `nbs-remote-git` to synchronise an entire repository between this machine and a remote build/test machine. IF you need to sync individual files, THEN use `nbs-remote-edit` instead. IF you need to run a command on the remote without syncing code, THEN use `nbs-remote-run` instead. The tool auto-adds a git remote on first `push` or `pull`.

---

## Local Command Execution

These tools run commands through the `nbs-ts-helper` daemon with the user's full login environment (proxy access, credentials, SSH agent). Agents SHOULD use these tools for local commands that require credentials.

### nbs-local-run

**Purpose:** Run a command through `nbs-ts-helper` and return the output.

**Usage:**
```bash
nbs-local-run [--timeout=N] <command>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--timeout=N` | No | Timeout in seconds (default: 300) |
| `<command>` | Yes | Command to run (single quoted string) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Command ran (check output for command-level errors) |
| 1 | `nbs-ts` not found or helper not running |
| 2 | Invalid arguments |
| 3 | Command timed out |

**Example:**
```bash
nbs-local-run 'git push origin main'
```

**When to use:** Use `nbs-local-run` for commands that require the user's full login environment — git push, proxy access, authenticated API calls. IF a command runs fine without credentials, THEN a plain shell command suffices and `nbs-local-run` is unnecessary.

---

### nbs-local-session

**Purpose:** Create a persistent local shell via `nbs-ts-helper` with the user's full login environment.

**Usage:**
```bash
nbs-local-session [--name=NAME]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--name=NAME` | No | Session name for debugging |

Prints the session handle to stdout.

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Session created (handle printed to stdout) |
| 1 | `nbs-ts` not found or helper not running |
| 2 | Invalid arguments |

**Example:**
```bash
HANDLE=$(nbs-local-session --name=build)
nbs-ts send "$HANDLE" 'make -j8'
nbs-ts read-new "$HANDLE" --strip
nbs-ts kill "$HANDLE"
```

**When to use:** Use `nbs-local-session` for interactive local work that requires multiple commands with the user's credentials. IF you only need to run one command, use `nbs-local-run` instead.

---

## Worker and Agent Management

These tools spawn, monitor, and dismiss Claude Code worker agents. Workers execute specific tasks and report results through the NBS framework.

### nbs-workers

**Purpose:** Manage worker lifecycle — spawn, monitor, search output, and dismiss workers.

**Usage:**
```bash
nbs-workers <command> [args...]
```

**Exit Codes (all subcommands):**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Worker not found |
| 4 | Invalid arguments |

#### nbs-workers spawn

**Purpose:** Create a task file, start a Claude worker, and send the initial prompt.

**Usage:**
```bash
nbs-workers spawn <slug> <project-dir> <task-description>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<slug>` | Yes | Short identifier for the worker (e.g. `parser`, `tests`) |
| `<project-dir>` | Yes | Project root directory |
| `<task-description>` | Yes | Task instructions for the worker |

Prints the generated worker name (e.g. `parser-a3f1`) to stdout.

**Example:**
```bash
nbs-workers spawn parser /home/user/myproject "Fix the recursive descent parser bug in parse_float"
```

**When to use:** Use `nbs-workers spawn` to create a worker for a specific task. The supervisor uses this to delegate work.

#### nbs-workers status

**Purpose:** Report a worker's status from its nbs-ts session and task file.

**Usage:**
```bash
nbs-workers status <name>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<name>` | Yes | Worker name (e.g. `parser-a3f1`) |

**Example:**
```bash
nbs-workers status parser-a3f1
```

**When to use:** Use `nbs-workers status` to check whether a worker is alive and what state its task is in.

#### nbs-workers search

**Purpose:** Search a worker's persistent log for a regex pattern.

**Usage:**
```bash
nbs-workers search <name> <regex> [--context=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<name>` | Yes | Worker name |
| `<regex>` | Yes | Search pattern |
| `--context=N` | No | Lines of context around matches (default: 50) |

**Example:**
```bash
nbs-workers search parser-a3f1 "error|fail" --context=10
```

**When to use:** Use `nbs-workers search` to find errors or specific output in a worker's log.

#### nbs-workers results

**Purpose:** Extract the Log section from a completed worker's task file.

**Usage:**
```bash
nbs-workers results <name>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<name>` | Yes | Worker name |

**Example:**
```bash
nbs-workers results parser-a3f1
```

**When to use:** Use `nbs-workers results` to read what a completed worker produced.

#### nbs-workers dismiss

**Purpose:** Kill a worker's nbs-ts session and mark its task file as dismissed.

**Usage:**
```bash
nbs-workers dismiss <name>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<name>` | Yes | Worker name |

**Example:**
```bash
nbs-workers dismiss parser-a3f1
```

**When to use:** Use `nbs-workers dismiss` to stop a worker that is no longer needed or has gone wrong.

#### nbs-workers continue

**Purpose:** Resume an agent from its session metadata by killing the old nbs-ts session and respawning with `claude --resume`.

**Usage:**
```bash
nbs-workers continue <handle> [--model=MODEL]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Worker handle |
| `--model=MODEL` | No | Override the model |

**Example:**
```bash
nbs-workers continue parser-a3f1 --model=sonnet
```

**When to use:** Use `nbs-workers continue` to resume a worker that crashed or that the operator killed, when the conversation context should carry forward.

#### nbs-workers session

**Purpose:** Display session metadata (session ID, model, PID, status) for a worker.

**Usage:**
```bash
nbs-workers session <handle>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Worker handle |

**Example:**
```bash
nbs-workers session parser-a3f1
```

**When to use:** Use `nbs-workers session` to inspect the internal state of a worker's Claude session.

#### nbs-workers list

**Purpose:** Show all workers with a status summary.

**Usage:**
```bash
nbs-workers list
```

**Example:**
```bash
nbs-workers list
```

**When to use:** Use `nbs-workers list` to get an overview of all workers and their current statuses.

---

### nbs-claude

**Purpose:** Launch Claude Code with the `nbs-sidecar` monitor attached.

**Usage:**
```bash
nbs-claude [--root=PATH] [--model=MODEL] [--continue=SESSION_ID] [claude-args...]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--root=PATH` | No | Project root containing `.nbs/` (default: current directory) |
| `--model=MODEL` | No | Model to use (e.g. `opus`, `sonnet`) |
| `--continue=SESSION_ID` | No | Session ID to continue |
| Environment: `NBS_HANDLE` | No | Agent handle (default: `claude`) |
| Environment: `NBS_ROOT` | No | Project root (overridden by `--root`) |
| Environment: `NBS_POLL_DISABLE` | No | Set to `1` to disable sidecar |
| Environment: `NBS_INITIAL_PROMPT` | No | Custom initial prompt |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 1 | General error |
| 4 | Invalid arguments |

**Example:**
```bash
NBS_HANDLE=generalist nbs-claude --root=/home/user/myproject
```

**When to use:** Use `nbs-claude` to start a Claude Code agent with NBS monitoring (bus events, chat notifications, periodic oracle triggers). IF you want Claude without monitoring, use `claude` directly.

---

### nbs-claude-remote

**Purpose:** Launch Claude Code on a remote machine via SSH.

**Usage:**
```bash
nbs-claude-remote --host=USER@HOST --root=PATH [--handle=NAME] [--name=NAME] [--ssh-key=PATH] [--ssh-opts=OPTS] [--resume] [--list]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--host=USER@HOST` | Yes | SSH target |
| `--root=PATH` | Yes | Project root on the remote machine (MUST contain `.nbs/`) |
| `--handle=NAME` | No | Agent handle (default: `remote`) |
| `--name=NAME` | No | Chat name for session naming (default: `live`) |
| `--ssh-key=PATH` | No | SSH key file |
| `--ssh-opts=OPTS` | No | Additional SSH options (space-separated single tokens) |
| `--resume` | No | Attach to an existing remote nbs-ts session |
| `--list` | No | List existing NBS nbs-ts sessions on the remote machine |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-claude-remote --host=user@remote-host --root=/home/user/project --handle=remote-builder
```

**When to use:** Use `nbs-claude-remote` to run a Claude Code agent on a remote machine where the project lives. All file access happens on the remote machine.

---

### nbs-launch-agent

**Purpose:** Provide a shared function to launch a single `nbs-claude` agent process.

**Usage:**
```bash
source nbs-launch-agent
launch_agent <handle> <project-root> <nbs-claude-path> <initial-prompt>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<handle>` | Yes | Agent handle |
| `<project-root>` | Yes | Project root directory |
| `<nbs-claude-path>` | Yes | Path to the `nbs-claude` binary |
| `<initial-prompt>` | Yes | Initial prompt for the agent |

This is a sourced library, not a standalone tool. `nbs-chat-terminal-restart.sh` and `nbs-spawn-worker` source it internally.

**Example:**
```bash
source ~/.nbs/bin/nbs-launch-agent
launch_agent supervisor /home/user/myproject ~/.nbs/bin/nbs-claude "You are the supervisor."
```

**When to use:** Use `nbs-launch-agent` only from scripts that need to spawn agents. Direct use is rare — prefer `nbs-workers spawn` or `nbs-chat-terminal --restart`.

---

### nbs-spawn-worker

**Purpose:** Spawn a skill-based worker agent with a given role and task.

**Usage:**
```bash
nbs-spawn-worker <role> <project-root> <skill-file> <task-instructions>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<role>` | Yes | Worker role name |
| `<project-root>` | Yes | Project root directory |
| `<skill-file>` | Yes | Path to the skill file for the worker |
| `<task-instructions>` | Yes | Task instructions |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Worker spawned |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-spawn-worker pythia /home/user/myproject .nbs/commands/nbs-pythia.md "Run trajectory assessment"
```

**When to use:** Use `nbs-spawn-worker` for oracle-type workers (pythia, shepard, librarian, fixup). `nbs-workers spawn` is preferred for general task workers.

---

### nbs-sidecar

**Purpose:** Monitor a Claude Code session in the background — handle notifications, bus events, and periodic oracle triggers.

**Usage:**
```bash
nbs-sidecar --handle=NAME --root=PATH --session=HANDLE [--transport=ts] [--initial-prompt=TEXT] [--log=PATH]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--handle=NAME` | Yes | Agent handle |
| `--root=PATH` | Yes | Project root containing `.nbs/` |
| `--session=HANDLE` | Yes | nbs-ts session handle |
| `--transport=ts` | No | Transport mode (default: `ts`) |
| `--initial-prompt=TEXT` | No | Custom initial prompt |
| `--log=PATH` | No | Log file for sidecar stderr |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 1 | General error |

**Example:**
```bash
nbs-sidecar --handle=supervisor --root=/home/user/myproject --session=a1b2c3d4
```

**When to use:** Agents SHOULD NOT run `nbs-sidecar` directly. `nbs-claude` starts a sidecar automatically. Run `nbs-sidecar` directly only for debugging.

---

### nbs-sidecar-restart

**Purpose:** Hot-restart or respawn `nbs-sidecar` processes without affecting Claude sessions.

**Usage:**
```bash
nbs-sidecar-restart [--respawn] [--name=PATTERN] [handle]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--respawn` | No | Scan all teams, spawn missing sidecars, restart existing ones |
| `--name=PATTERN` | No | Filter nbs-ts sessions by name substring |
| `[handle]` | No | Restart only the sidecar for this handle |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Error |

**Example:**
```bash
nbs-sidecar-restart --respawn
```

**When to use:** Use `nbs-sidecar-restart` after rebuilding the sidecar binary to pick up changes without restarting agents. Use `--respawn` to also spawn any missing sidecars.

---

## Event Bus

The event bus provides file-based, priority-ordered event queues. Agents and sidecars publish events to coordinate actions. The bus supports deduplication and acknowledgement.

### nbs-bus

**Purpose:** Publish, read, acknowledge, and manage events in the file-based event queue.

**Usage:**
```bash
nbs-bus <command> [args...]
```

**Exit Codes (all subcommands):**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Events directory not found |
| 3 | Event file not found |
| 4 | Invalid arguments |
| 5 | Deduplication (event dropped) |

```pascal
type
  BusPriority = (critical, high, normal, low);

  BusEvent = record
    source   : String;   { handle of the publishing agent }
    event_type : String;  { event type identifier }
    priority : BusPriority;
    payload  : String;    { free-form event data }
  end;
```

#### nbs-bus publish

**Purpose:** Write an event file to the queue.

**Usage:**
```bash
nbs-bus publish <dir> <source> <type> <priority> [payload] [--dedup-window=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory (e.g. `.nbs/events/`) |
| `<source>` | Yes | Source handle (e.g. `supervisor`) |
| `<type>` | Yes | Event type (e.g. `chat-message`, `build-complete`) |
| `<priority>` | Yes | One of: `critical`, `high`, `normal`, `low` |
| `[payload]` | No | Event payload text |
| `--dedup-window=N` | No | Drop IF same `source:type` exists within N seconds (default: 0, disabled) |

**Example:**
```bash
nbs-bus publish .nbs/events/ supervisor task-assigned normal "parser bug fix"
```

**When to use:** Use `nbs-bus publish` to notify other agents or sidecars of events. The sidecar checks the bus periodically and reacts to events.

#### nbs-bus check

**Purpose:** List pending events, highest priority first.

**Usage:**
```bash
nbs-bus check <dir> [--handle=<name>]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory |
| `--handle=<name>` | No | Show only events from this source |

**Example:**
```bash
nbs-bus check .nbs/events/
```

**When to use:** Use `nbs-bus check` to see what events are pending. Sidecars call this on every poll cycle.

#### nbs-bus read

**Purpose:** Read the contents of a single event file.

**Usage:**
```bash
nbs-bus read <dir> <event-file>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory |
| `<event-file>` | Yes | Event filename |

**Example:**
```bash
nbs-bus read .nbs/events/ 1774856400-supervisor-task-assigned.event
```

**When to use:** Use `nbs-bus read` to inspect the contents of a specific event.

#### nbs-bus ack

**Purpose:** Acknowledge a single event (move it to `processed/`).

**Usage:**
```bash
nbs-bus ack <dir> <event-file>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory |
| `<event-file>` | Yes | Event filename to acknowledge |

**Example:**
```bash
nbs-bus ack .nbs/events/ 1774856400-supervisor-task-assigned.event
```

**When to use:** Use `nbs-bus ack` after processing an event to remove it from the pending queue.

#### nbs-bus ack-all

**Purpose:** Acknowledge all pending events.

**Usage:**
```bash
nbs-bus ack-all <dir> [--handle=<name>]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory |
| `--handle=<name>` | No | Acknowledge only events from this source |

**Example:**
```bash
nbs-bus ack-all .nbs/events/
```

**When to use:** Use `nbs-bus ack-all` to clear the entire event queue or a specific source's events.

#### nbs-bus prune

**Purpose:** Delete the oldest processed events when total size exceeds the limit.

**Usage:**
```bash
nbs-bus prune <dir> [--max-bytes=N]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory |
| `--max-bytes=N` | No | Size limit (default: 16 MB) |

**Example:**
```bash
nbs-bus prune .nbs/events/ --max-bytes=8388608
```

**When to use:** Use `nbs-bus prune` to prevent the processed events archive from growing indefinitely.

#### nbs-bus status

**Purpose:** Show a summary of the event bus — pending count by priority and processed count.

**Usage:**
```bash
nbs-bus status <dir>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<dir>` | Yes | Events directory |

**Example:**
```bash
nbs-bus status .nbs/events/
```

**When to use:** Use `nbs-bus status` for a quick overview of event queue health.

---

## Decision Log

These tools read from and write to the scribe decision log. The decision log records every significant decision the team makes, with rationale, participants, and linkage to superseded decisions.

### nbs-scribe-log

**Purpose:** Append a structured decision entry to the scribe log.

**Usage:**
```bash
nbs-scribe-log <log-file> <summary> --participants=<a,b,c> --rationale=<text> [--chat-ref=<file:~Lnnn>] [--artefacts=<paths>] [--risk-tags=<tags>] [--status=<status>] [--supersedes=<D-timestamp>] [--bus-dir=<path>]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<log-file>` | Yes | Path to the scribe log (`.md`) |
| `<summary>` | Yes | One-line decision summary |
| `--participants=<a,b,c>` | Yes | Comma-separated participant handles |
| `--rationale=<text>` | Yes | 1–3 sentence rationale |
| `--chat-ref=<file:~Lnnn>` | No | Chat file and approximate line reference |
| `--artefacts=<paths>` | No | Commit hashes, file paths, or `-` |
| `--risk-tags=<tags>` | No | Comma-separated risk tags, or `none` |
| `--status=<status>` | No | One of: `decided`, `superseded`, `reversed`, `mitigated` (default: `decided`) |
| `--supersedes=<D-timestamp>` | No | Link to the superseded decision |
| `--bus-dir=<path>` | No | Bus directory (default: `.nbs/events/`) |

```pascal
type
  DecisionStatus = (decided, superseded, reversed, mitigated);

  DecisionEntry = record
    id           : String;          { format: D-<unix-timestamp> }
    summary      : String;
    participants : sequence of String;
    rationale    : String;
    status       : DecisionStatus;
    chat_ref     : String;          { format: <file>:~L<line> }
    artefacts    : String;
    risk_tags    : String;
    case has_supersedes : Boolean of
      True  : (supersedes : String);  { D-<timestamp> of superseded decision }
      False : ();
  end;
```

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success (decision ID printed to stdout) |
| 1 | General error (I/O, lock failure) |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-scribe-log .nbs/scribe/live-log.md "Use recursive descent parser" \
  --participants=alex,supervisor \
  --rationale="Grammar is LL(1), Pratt adds unnecessary complexity." \
  --chat-ref=live.chat:~L42
```

**When to use:** Use `nbs-scribe-log` to record a team decision. The scribe agent calls this. Other agents SHOULD ask the scribe to record decisions rather than calling `nbs-scribe-log` directly.

---

### nbs-scribe-query

**Purpose:** Search the scribe decision log for decisions matching a pattern, handle, tag, or ID.

**Usage:**
```bash
nbs-scribe-query --chat=<chat-file> <pattern> [--regex]
nbs-scribe-query --chat=<chat-file> --id=D-<ts>
nbs-scribe-query --chat=<chat-file> --last=N
nbs-scribe-query --chat=<chat-file> --by=<handle>
nbs-scribe-query --chat=<chat-file> --tag=<tag>
nbs-scribe-query --chat=<chat-file> --count
nbs-scribe-query --chat=<chat-file> --superseded
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--chat=<chat-file>` | Yes | Chat file (the log file path is derived from the chat filename) |
| `<pattern>` | No | Search pattern (literal substring by default) |
| `--regex` | No | Treat the pattern as an ERE regex |
| `--id=D-<ts>` | No | Look up a specific decision by ID |
| `--last=N` | No | Show the last N decisions |
| `--by=<handle>` | No | Decisions involving this handle |
| `--tag=<tag>` | No | Decisions with this risk tag |
| `--count` | No | Count total decisions |
| `--superseded` | No | Show all corrections (superseded decisions) |

The tool automatically searches archive files in addition to the live log.

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success (matches found) |
| 1 | No matches |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-scribe-query --chat=.nbs/chat/live.chat "parser" --by=theologian
```

**When to use:** Use `nbs-scribe-query` to find past decisions. The Librarian uses this to answer questions about prior team decisions. Search is literal by default; use `--regex` for pattern matching.

---

## Project Initialisation and Diagnostics

These tools bootstrap NBS infrastructure for a project and diagnose installation problems.

### nbs-chat-init

**Purpose:** Bootstrap NBS framework infrastructure for a project directory.

**Usage:**
```bash
nbs-chat-init --name=NAME [--root=PATH] [--project-name=NAME] [--force] [--dry-run] [--spawn-scribe] [--spawn-pythia] [--spawn-all] [--spawn-only] [--compact-log] [--help]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--name=NAME` | Yes | Chat name — all resource names derive from this |
| `--root=PATH` | No | Project root directory (default: current directory) |
| `--project-name=NAME` | No | Override project name (default: dirname of root) |
| `--force` | No | Skip confirmation prompts |
| `--dry-run` | No | Print actions without executing |
| `--spawn-scribe` | No | Launch Scribe in an nbs-ts session |
| `--spawn-pythia` | No | Launch Pythia in an nbs-ts session |
| `--spawn-all` | No | Launch Scribe + Pythia + main Claude instance |
| `--spawn-only` | No | Skip infrastructure creation, just spawn agents |
| `--compact-log` | No | Archive the decision log IF it exceeds 100 entries |

Infrastructure created:
- `.nbs/events/` — Event bus queue
- `.nbs/chat/<name>.chat` — Primary chat channel
- `.nbs/scribe/<name>-log.md` — Decision log
- `.nbs/workers/` — Worker task files
- `.nbs/pids/` — Agent PID tracking
- `.nbs/commands/` — Skill files (symlinked from `~/.nbs/commands/`)
- `.nbs/docs/` — Tool reference (symlinked from `~/.nbs/docs/`)

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 4 | Invalid arguments |

**Example:**
```bash
cd ~/projects/myapp
nbs-chat-init --name=myapp-dev
nbs-chat-terminal .nbs/chat/myapp-dev.chat alex --restart --goal-file=goal.md
```

**When to use:** Use `nbs-chat-init` to set up NBS in any project. Run once per project. To restart agents after a reboot, use `--spawn-only --spawn-all`.

---

### nbs-doctor

**Purpose:** Diagnose and optionally repair the NBS environment.

**Usage:**
```bash
nbs-doctor [--fix]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--fix` | No | Attempt to repair problems (without `--fix`, only reports) |

The tool checks: nbs-ts-helper status, socket health, symlinks, permissions, and common configuration issues. It NEVER deletes chat logs, scribe logs, digests, or events.

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | All checks pass (or all fixes applied) |
| 1 | Problems found |

**Example:**
```bash
nbs-doctor --fix
```

**When to use:** Use `nbs-doctor` when tools report unexpected errors. Run without `--fix` first to see what is wrong, THEN run with `--fix` to repair.

---

### nbs-hub

**Purpose:** Enforce deterministic process control for AI supervisors — gate worker spawns behind audit submissions.

**Usage:**
```bash
nbs-hub [--project <path>] <command> [args...]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--project <path>` | No | Project path (default: current directory) |
| `<command>` | Yes | Subcommand |

| Subcommand | Description |
|------------|-------------|
| `init <project-dir> <goal>` | Initialise the hub |
| `status` | Full project state dump |
| `spawn <slug> <task-desc>` | Spawn a worker (audit-gated) |
| `check <worker-name>` | Check worker status |
| `result <worker-name>` | Read results (increments audit counter) |
| `dismiss <worker-name>` | Kill and dismiss a worker |
| `list` | List all workers |
| `audit <file>` | Submit a self-check audit |
| `gate <phase> <tests> <audit>` | Submit a phase gate |
| `phase` | Show current phase |
| `phase-name <name>` | Set current phase name |
| `doc register <name> <path>` | Register a document |
| `doc list` | List registered documents |
| `doc read <name>` | Read a registered document |
| `decision <text>` | Record a decision |
| `log [n]` | Show last n log entries |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Validation error |
| 2 | Hub not found or document not registered |
| 3 | Spawn refused — audit required |
| 4 | Usage error |

**Example:**
```bash
nbs-hub init /home/user/myproject "Optimise the parser"
nbs-hub spawn parser "Fix parse_float regression"
```

**When to use:** Use `nbs-hub` for structured, audit-gated project execution. The hub enforces that supervisors submit audits before spawning new workers, preventing runaway worker creation.

---

### nbs-oracle-reaper

**Purpose:** Check for and clean up oracle sessions that have completed their work.

**Usage:**
```bash
nbs-oracle-reaper check <project-root>
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `check` | Yes | The check subcommand |
| `<project-root>` | Yes | Project root directory |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-oracle-reaper check /home/user/myproject
```

**When to use:** The `nbs-chat-terminal` runs the oracle reaper automatically every 10 seconds. Direct invocation is rarely needed.

---

### nbs-digest-spawn

**Purpose:** Spawn a chat digest agent to summarise recent chat history.

**Usage:**
```bash
nbs-digest-spawn <chat-file> [--wait]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<chat-file>` | Yes | Path to the chat file |
| `--wait` | No | Wait for the digest to complete |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Error |
| 4 | Invalid arguments |

**Example:**
```bash
nbs-digest-spawn .nbs/chat/live.chat --wait
```

**When to use:** Use `nbs-digest-spawn` to generate a summary of recent chat activity. The restart script calls this automatically.

---

## Data Format Tools (Honest)

These tools parse, format, query, and build Honest documents. Honest is the self-describing data format used by the NBS framework for structured data interchange.

### honest-parse

**Purpose:** Parse and validate an Honest document, reporting errors.

**Usage:**
```bash
honest-parse [-q|-v] <file.honest | ->
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to an Honest file, or `-` for stdin |
| `-q` | No | Quiet mode (exit code only) |
| `-v` | No | Verbose mode (detailed parse output) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Valid Honest document |
| 1 | Parse error |

**Example:**
```bash
honest-parse message.honest
```

**When to use:** Use `honest-parse` to validate Honest documents. IF you receive a parse error from another tool, run `honest-parse -v` to see detailed diagnostics.

---

### honest-fmt

**Purpose:** Reformat an Honest document in document mode or wire mode.

**Usage:**
```bash
honest-fmt [--wire|--document] <file.honest | ->
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to an Honest file, or `-` for stdin |
| `--wire` | No | Output in wire mode (comments stripped, minimal whitespace) |
| `--document` | No | Output in document mode (human-readable, default) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Parse or format error |

**Example:**
```bash
honest-fmt --wire message.honest
```

**When to use:** Use `honest-fmt` to normalise formatting or convert between document and wire modes. Use `--wire` for transmission, `--document` for human reading.

---

### honest-get

**Purpose:** Extract a field value from an Honest document.

**Usage:**
```bash
honest-get <file | -> <var-name> <field> [field ...]
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to an Honest file, or `-` for stdin |
| `<var-name>` | Yes | Variable name in the document |
| `<field>` | Yes | Field name to extract (chain multiple for nested fields) |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success (value printed to stdout) |
| 1 | Field not found or parse error |

**Example:**
```bash
honest-get message.honest message sender
```

**When to use:** Use `honest-get` to extract individual values from Honest documents in shell scripts. Chain field names for nested access: `honest-get doc.honest msg payload content`.

---

### honest-build

**Purpose:** Construct an Honest document from command-line field=value arguments.

**Usage:**
```bash
honest-build --type=TypeName [--var-name=name] [--rulebook=file | --typedef='type ...'] field=value ...
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `--type=TypeName` | Yes | The Honest type to construct |
| `--var-name=name` | No | Variable name in the output document |
| `--rulebook=file` | No | Rulebook file containing type definitions |
| `--typedef='type ...'` | No | Inline type definition |
| `field=value ...` | Yes | Field assignments |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success (document printed to stdout) |
| 1 | Build error |

**Example:**
```bash
honest-build --type=Message --typedef='type AgentRole = (Supervisor, Generalist); Message = record id : LongInt; sender : AgentRole; end;' id=42 sender=Supervisor
```

**When to use:** Use `honest-build` to programmatically create Honest documents from shell scripts without manual formatting.

---

### honest-extract

**Purpose:** Extract structured data from an Honest document.

**Usage:**
```bash
honest-extract <file.honest | ->
```

**Arguments:**

| Argument | Required | Description |
|----------|----------|-------------|
| `<file>` | Yes | Path to an Honest file, or `-` for stdin |

**Exit Codes:**

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Parse error or no valid document found |

**Example:**
```bash
honest-extract report.honest
```

**When to use:** Use `honest-extract` to pull data from Honest documents for processing by other tools.
