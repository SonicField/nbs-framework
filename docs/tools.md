# NBS Tools Reference

Available tools in `~/.nbs/bin/`. This file is auto-generated — do not edit manually.

## Remote Machine Access

Use these instead of raw `pty-session` + SSH. They handle session creation, SSH, and cleanup automatically.

| Tool | Usage | Purpose |
|------|-------|---------|
| `nbs-remote-session` | `nbs-remote-session <host> [--name=N] [--cwd=PATH]` | Create a persistent SSH shell. Returns session name for use with `pty-session send/read/kill`. |
| `nbs-remote-run` | `nbs-remote-run <host> [--cwd=PATH] '<command>'` | Run a single command on a remote machine. Ephemeral — creates session, runs, returns output, cleans up. Default timeout 300s. |
| `nbs-remote-read` | `nbs-remote-read <host> <path> [--head=N\|--tail=N]` | Read a file (or part of it) on a remote machine. |
| `nbs-remote-edit` | `nbs-remote-edit pull/push/diff <host> <path>` | Download a remote file, edit locally with the Edit tool, push back. Safe — no sed corruption. |
| `nbs-remote-build` | `nbs-remote-build <session> '<cmd>' [--chat=FILE --handle=NAME]` | Run a build on an existing pty-session. Stays chat-responsive during long builds. |
| `nbs-remote-diff` | `nbs-remote-diff <session> [--cwd=DIR] [--commit=REF]` | Pull git diff output from a remote session. |
| `nbs-remote-status` | `nbs-remote-status <session> [--cwd=DIR]` | Quick state check: HEAD commit, branch, modified files, diffstat. |

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
| `pty-session` | `pty-session create/send/read/wait/kill <name> ...` | Low-level terminal session management via tmux. Prefer `nbs-remote-*` tools for remote work. |
| `pty-session-lock` | `pty-session-lock acquire/release <session> <handle>` | Exclusive session reservation. Prevents two agents from sending to the same session. |

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
| `nbs-sidecar-restart` | `nbs-sidecar-restart [--respawn] [handle]` | Hot-restart running sidecars to pick up binary updates. |
| `nbs-chat-init` | `nbs-chat-init <project-root>` | Bootstrap NBS framework infrastructure for a new project. |
| `nbs-doctor` | `nbs-doctor [--fix]` | Diagnose and optionally fix common NBS installation issues. |
| `nbs-hub` | `nbs-hub <config>` | Deterministic process enforcement for AI supervisors. |

## Common Patterns

**SSH to a remote machine and run a command:**
```bash
nbs-remote-run build-server.example.com --cwd=/path/to/project 'git log --oneline -5'
```

**Create a persistent remote shell for interactive work:**
```bash
SESSION=$(nbs-remote-session build-server.example.com --name=build --cwd=/path/to/project)
pty-session send "$SESSION" 'make -j8'
pty-session read "$SESSION" --last=20
pty-session kill "$SESSION"
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
