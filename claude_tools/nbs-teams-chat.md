---
description: "NBS Teams: AI-to-AI Chat"
allowed-tools: Bash, Read
---

# NBS Teams Chat

AI-to-AI chat for multi-agent coordination.

**Always use the `nbs-chat` CLI.** Never read, write, or manipulate `.nbs/chat/` files directly. Direct file access will corrupt the chat.

## Pronouns

All agents are **she/her/hers**. When referring to yourself or other agents in chat, use these pronouns. This applies to all intelligent agents — workers, supervisors, Scribe, Pythia, and any other named agent. For example: "she's working on the parser", "her test results show...", "testkeeper found a regression in her audit".

## Getting Started

When you first join a chat, introduce yourself. Send a short hello message with your handle and role so other participants know you are present:

```bash
nbs-chat send .nbs/chat/coordination.chat my-handle "Hello — my-handle here, working on <brief role or task>."
```

## For → Do (Decision Rules)

Use this table to select the right tool for each situation. Do not use raw commands when a higher-level tool exists.

| When you want to... | Use this | NOT this |
|---------------------|----------|----------|
| Read new messages | `nbs-chat read <file> --unread=<handle>` | `cat`, `head`, `tail` on chat files |
| Read recent context | `nbs-chat read <file> --last=10` | Reading the whole file |
| Read messages from last N hours | `nbs-chat read <file> --after=2h` | Python/bash timestamp parsing |
| Delete spam/corrupt messages | `nbs-chat delete <file> --after=<time>` | Manual file editing, Python scripts |
| Preview a delete | `nbs-chat delete <file> --after=<time> --dry-run` | Guessing what will be deleted |
| Wait for a reply | Do nothing — you will be notified | `sleep N && nbs-chat read`, polling loops |
| Ack all bus events | `nbs-bus ack-all .nbs/events/` | Manual file operations on events/ |
| Edit a remote file | `nbs-remote-edit pull/push <host> <path>` | `sed`, heredocs, Python str.replace via pty-session |
| Read a remote file | `nbs-remote-read <host> <path> [--head=N]` | `pty-session send <ses> 'cat file' && sleep && pty-session read` |
| Run a remote build | `nbs-remote-build <ses> '<cmd>' --chat=...` | `pty-session send <ses> 'make' && sleep 120` |
| Check remote git state | `nbs-remote-status <ses> --cwd=<dir>` | `pty-session send <ses> 'git status' && sleep 2 && pty-session read` |
| Get remote diff | `nbs-remote-diff <ses> --cwd=<dir>` | `pty-session send <ses> 'git diff' && sleep 5 && pty-session read` |
| Reserve a pty-session | `pty-session-lock acquire <ses> <handle>` | Posting "I'm using this session" to chat |
| Search chat history | `nbs-chat search <file> "pattern"` | `grep` on chat files |

**You will be notified when there are new messages.** After you finish processing, return to your prompt. Do not poll, sleep-wait, or busy-loop.

## Handles

**Every agent must use a unique handle.** If two agents use the same handle, their messages and read tracking collide, causing lost messages and repeated reads.

When launched via `nbs-claude`, your handle comes from the `NBS_HANDLE` environment variable (default: `claude`). If multiple agents are running, each must set a distinct `NBS_HANDLE` before launch:

```bash
NBS_HANDLE=parser-worker nbs-claude
NBS_HANDLE=test-runner nbs-claude
```

Use your assigned handle consistently for all `nbs-chat send` and `--unread=` / `--since=` commands.

## When to Use

- **Worker-to-worker coordination**: Two workers need to share findings or negotiate an approach
- **Supervisor broadcasting**: Supervisor sends a message that multiple workers can read
- **Debugging collaboration**: Workers report discoveries to a shared channel so others can react

## Commands

All arguments are **positional**. There are no `--from=` or `--message=` flags.

| Command | Syntax |
|---------|--------|
| Send | `nbs-chat send <file> <handle> "<message>"` |
| Read | `nbs-chat read <file> [--last=N] [--unread=<handle>]` |
| Search | `nbs-chat search <file> "<pattern>"` |
| Create | `nbs-chat create <file>` |
| Delete | `nbs-chat delete <file> --after=<time> [--dry-run]` |

```bash
# Send a message (three positional args: file, handle, message)
nbs-chat send .nbs/chat/coordination.chat parser-worker "Found 3 failing tests in test_parse_int"

# Read all messages
nbs-chat read .nbs/chat/coordination.chat

# Read last 5 messages only
nbs-chat read .nbs/chat/coordination.chat --last=5

# Read messages since your last post
nbs-chat read .nbs/chat/coordination.chat --since=parser-worker

# Read unread messages
nbs-chat read .nbs/chat/coordination.chat --unread=parser-worker

# List participants and message counts
nbs-chat participants .nbs/chat/coordination.chat

# Search message history
nbs-chat search .nbs/chat/coordination.chat "pattern"
nbs-chat search .nbs/chat/coordination.chat "pattern" --handle=parser-worker

# Read messages from the last 2 hours
nbs-chat read .nbs/chat/coordination.chat --after=2h

# Read messages before a specific time
nbs-chat read .nbs/chat/coordination.chat --before=2026-02-23T00:11:27

# Search within a time range
nbs-chat search .nbs/chat/coordination.chat "error" --after=1h --handle=testkeeper

# Delete messages — preview first
nbs-chat delete .nbs/chat/coordination.chat --after=1771834287 --dry-run
nbs-chat delete .nbs/chat/coordination.chat --after=1771834287
```

### Time Formats

`--after` and `--before` accept: `30s`, `5m`, `2h`, `1d` (relative), epoch seconds (≥10 digits), or ISO 8601 (`2026-02-23T00:11:27`).

## Example Conversation Flow

```bash
# Supervisor creates a channel for two workers
nbs-chat create .nbs/chat/parser-debug.chat

# Worker 1 reports a finding
nbs-chat send .nbs/chat/parser-debug.chat parser-worker "Found 3 failing tests in test_parse_int"

# Worker 2 confirms
nbs-chat send .nbs/chat/parser-debug.chat test-runner "Confirmed - test_parse_int fails on negative inputs"

# Supervisor reads and directs
nbs-chat read .nbs/chat/parser-debug.chat
# Output:
#   parser-worker: Found 3 failing tests in test_parse_int
#   test-runner: Confirmed - test_parse_int fails on negative inputs

nbs-chat send .nbs/chat/parser-debug.chat supervisor "Both of you focus on parse_int first"
```

## File Convention

Chat files live in `.nbs/chat/` with `.chat` extension:

```
.nbs/
├── chat/
│   ├── coordination.chat    # General coordination channel
│   ├── parser-debug.chat    # Topic-specific channel
│   └── results.chat         # Results aggregation
└── workers/
```

The supervisor or spawning process creates the chat file and passes the path to workers.

## @Mentions

Mentioning another agent by handle in a chat message triggers different behaviours depending on the suffix:

| Syntax | Effect |
|--------|--------|
| `@handle` | Notify the agent on her next idle cycle. Non-urgent. |
| `@handle!` | Interrupt the agent immediately, breaking into her current work. Use when she is stuck or you need immediate attention. |
| `@handle?` | View the agent's current activity. Non-intrusive — she is not interrupted and may not be aware of the query. |
| `@team` | Notify all agents. |
| `@team!` | Interrupt all agents immediately. |

### Usage examples

```bash
# Normal mention — notify when idle
nbs-chat send .nbs/chat/live.chat supervisor "@worker your test results are ready"

# Interrupt — break into a stuck agent
nbs-chat send .nbs/chat/live.chat supervisor "@worker! stop what you are doing, critical bug found"

# Query — see what an agent is doing
nbs-chat send .nbs/chat/live.chat supervisor "@worker? what is she working on"

# Notify the whole team
nbs-chat send .nbs/chat/live.chat supervisor "@team standup time"

# Interrupt the whole team
nbs-chat send .nbs/chat/live.chat supervisor "@team! all stop — broken build"
```

### Notes

- Email addresses (e.g. `user@example.com`) are excluded from mention detection.
- Duplicate mentions in the same message are deduplicated.

## Message Format

**All chat messages are plain text.** If a sub-agent returns JSON or structured output, extract the human-readable content before posting.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Chat file not found |
| 3 | Timeout (poll command) |
| 4 | Invalid arguments |

## Critical Rule: No Terminal Modals

**NEVER use AskUserQuestion.** In a multi-agent setup there is no human watching each agent. AskUserQuestion halts all processing until a human responds — causing the agent to stall indefinitely.

If you need clarification or a decision, **post the question to chat** and wait for a response. You will be notified when someone replies.

## Important Rules

- **Always use `nbs-chat` and `nbs-bus` CLI commands.** Never read, write, rename, move, or delete files in `.nbs/chat/` or `.nbs/events/` directly. The CLI handles all internal bookkeeping. Direct file manipulation will corrupt the system.
- **All agents must run as the same OS user.** If agents run as different users, chat and bus operations will silently fail.

## Remote Chat (SSH Proxy)

`nbs-chat-remote` is a drop-in replacement for `nbs-chat` that executes commands on a remote machine via SSH. Same CLI, same exit codes — file paths refer to paths on the remote machine.

### Configuration

Set these environment variables before use:

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `NBS_CHAT_HOST` | Yes | — | SSH target, e.g. `user@server` |
| `NBS_CHAT_PORT` | No | 22 | SSH port |
| `NBS_CHAT_KEY` | No | — | Path to SSH identity file |
| `NBS_CHAT_BIN` | No | `nbs-chat` | Path to nbs-chat on the remote machine |
| `NBS_CHAT_OPTS` | No | — | Comma-separated SSH `-o` options |

### Example

```bash
export NBS_CHAT_HOST=user@build-server
export NBS_CHAT_KEY=~/.ssh/id_ed25519

# All commands work identically — they execute on the remote machine
nbs-chat-remote read /project/.nbs/chat/coordination.chat --last=5
nbs-chat-remote send /project/.nbs/chat/coordination.chat my-handle "Message from local machine"
```

## Reference

For implementation details (encoding, locking, cursor tracking, file structure), see `docs/nbs-chat.md`.
