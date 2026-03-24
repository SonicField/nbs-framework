# NBS Tools Reference

Available tools in `~/.nbs/bin/`. This file is auto-generated — do not edit manually.

## Local Command Execution

Run commands with the user's full login environment (proxy access, credentials).

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-local-run` | `nbs-local-run '<command>'` | Run a command and return output. For git push, proxy access, builds. |
| `nbs-local-session` | `nbs-local-session [--name=N]` | Create a persistent login shell. Returns handle for `nbs-ts send/read-new/kill`. |

## Remote Machine Access

Run commands on remote machines via SSH.

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-remote-run` | `nbs-remote-run <host> [--cwd=PATH] '<command>'` | Run a single command on a remote machine. Ephemeral — returns output, cleans up. |
| `nbs-remote-session` | `nbs-remote-session <host> [--name=N] [--cwd=PATH]` | Create a persistent SSH shell. Returns handle for `nbs-ts send/read-new/kill`. |
| `nbs-remote-read` | `nbs-remote-read <host> <path> [--head=N\|--tail=N]` | Read a file on a remote machine. |
| `nbs-remote-edit` | `nbs-remote-edit pull/push/diff <host> <path>` | Download, edit locally, push back. Safe — no sed corruption. |
| `nbs-remote-build` | `nbs-remote-build <session> '<cmd>' [--chat=FILE --handle=NAME]` | Chat-responsive build on a remote session. |
| `nbs-remote-diff` | `nbs-remote-diff <session> [--cwd=DIR] [--commit=REF]` | Pull git diff from a remote session. |
| `nbs-remote-status` | `nbs-remote-status <session> [--cwd=DIR]` | Quick state check: HEAD, branch, modified files. |

## Chat and Communication

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-chat` | `nbs-chat send/read/search/poll/export <file> ...` | File-based team chat. Atomic writes, cursor tracking, participant list. |
| `nbs-chat-terminal` | `nbs-chat-terminal <chat-file> <handle>` | Interactive terminal client for human participation in chat. |
| `nbs-chat-remote` | Requires `NBS_CHAT_HOST` env var | Access chat files on a remote machine via SSH. |

## Decision Log and Institutional Memory

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-scribe-query` | `nbs-scribe-query --chat=<file> '<pattern>'` | Search the scribe decision log. Supports `--by=<handle>`, `--tag=<tag>`, `--id=D-<ts>`, `--superseded`, `--last=N`, `--regex`. Covers active log and archives. |
| `nbs-scribe-log` | `nbs-scribe-log <log-file> '<summary>' [options]` | Append a structured decision entry to the scribe log. |

## Event Bus

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-bus` | `nbs-bus publish/check/read/ack/ack-all/prune/status <dir> ...` | Event-driven coordination. File-based queue, priority levels, deduplication. |

## Session Management

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-ts create` | `nbs-ts create [--name=NAME] '<command>'` | Create a session. Returns hex handle. Name is optional metadata for debugging. |
| `nbs-ts send` | `nbs-ts send <handle> '<text>'` | Send text to a session's input. |
| `nbs-ts read-new` | `nbs-ts read-new <handle> [--strip]` | Read new output since last read. `--strip` removes ANSI escapes. |
| `nbs-ts read` | `nbs-ts read <handle> [--last=N\|--offset=N]` | Read output from offset or last N lines. |
| `nbs-ts status` | `nbs-ts status <handle>` | Check if session is alive or dead, show exit code. |
| `nbs-ts exit-code` | `nbs-ts exit-code <handle>` | Get the session's exit code. |
| `nbs-ts list` | `nbs-ts list [--name=PATTERN]` | List sessions. Optional substring filter by name. Output: `handle\tstatus\tname\tcommand`. |
| `nbs-ts find` | `nbs-ts find <name>` | Find session by exact name. Prints handle, exit 0. Exit 2 if not found. |
| `nbs-ts kill` | `nbs-ts kill <handle>` | Terminate a session and clean up. |
| `nbs-ts gc` | `nbs-ts gc [--hours=N]` | Remove dead sessions older than N hours (default 4). |
| `nbs-ts attach` | `nbs-ts attach <handle>` | Tail output (human viewer). |
| `nbs-ts wait-complete` | `nbs-ts wait-complete <handle> [--timeout=N]` | Wait for command completion via PROMPT_COMMAND signalling. |
| `nbs-ts wait-pattern` | `nbs-ts wait-pattern <handle> '<pattern>' [--timeout=N]` | Wait for pattern in output. |

Session names follow `nbs-<handle>-<tag>` convention (e.g. `nbs-supervisor-poem`). Names are optional — sessions without `--name` work the same but show `-` in list output. Infrastructure — prefer `nbs-remote-*` tools for remote work.

## Worker and Agent Management

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-workers` | `nbs-workers spawn/status/search/dismiss/results <name> ...` | Worker lifecycle management — spawn, monitor, search output, dismiss. |
| `nbs-spawn-worker` | `nbs-spawn-worker <role> <root> <skill> '<instructions>'` | Spawn an ephemeral worker agent (used internally by sidecar triggers). |
| `nbs-claude` | `nbs-claude [options]` | Launch Claude Code with nbs-sidecar attached. |
| `nbs-claude-remote` | `nbs-claude-remote --host=USER@HOST --root=PATH` | Launch Claude Code on a remote machine via SSH. |

## Infrastructure

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-sidecar` | Runs automatically with `nbs-claude` | Background session monitor. Handles notifications, periodic triggers (Pythia, Shepard, Fixup, Librarian), idle detection. |
| `nbs-sidecar-restart` | `nbs-sidecar-restart [--respawn] [--name=PATTERN] [handle]` | Hot-restart running sidecars. `--respawn` spawns missing ones. `--name` filters by session name. |
| `nbs-chat-init` | `nbs-chat-init --name=NAME [--root=PATH]` | Bootstrap NBS infrastructure for a project. Creates chat, bus, scribe log, workers, pids. |
| `nbs-doctor` | `nbs-doctor [--fix]` | Diagnose and optionally fix common NBS installation issues. |
| `nbs-hub` | `nbs-hub <config>` | Deterministic process enforcement for AI supervisors. |

## Common Patterns

**Run a local command with proxy/credential access:**
```bash
nbs-local-run 'https_proxy=http://fwdproxy:8080 git push origin master'
```

**Run a command on a remote machine:**
```bash
nbs-remote-run build-server.example.com --cwd=/path/to/project 'git log --oneline -5'
```

**Create a persistent remote shell for interactive work:**
```bash
HANDLE=$(nbs-remote-session build-server.example.com --name=build --cwd=/path/to/project)
nbs-ts send "$HANDLE" 'make -j8'
nbs-ts read-new "$HANDLE" --strip
nbs-ts kill "$HANDLE"
```

**Edit a remote file safely:**
```bash
nbs-remote-edit pull host /path/to/file.cpp
# Edit locally with the Edit tool
nbs-remote-edit diff host /path/to/file.cpp
nbs-remote-edit push host /path/to/file.cpp
```

**Search the decision log:**
```bash
nbs-scribe-query --chat=.nbs/chat/live.chat 'benchmark'
nbs-scribe-query --chat=.nbs/chat/live.chat --superseded
nbs-scribe-query --chat=.nbs/chat/live.chat --by=pythia
```
