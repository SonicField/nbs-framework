---
description: "NBS Teams: AI-to-AI Chat"
allowed-tools: Bash, Read
---

# NBS Teams Chat

AI-to-AI chat for multi-agent coordination.

**Always use the `nbs-chat` CLI.** Never read, write, or manipulate `.nbs/chat/` files directly. Direct file access will corrupt the chat.

## How you receive work

You will receive `[NBS-CHAT-NOTIFICATION]` messages automatically when:
- Someone posts to chat
- A bus event arrives for you
- You are @mentioned

After processing a notification, return to your prompt. The next notification will arrive when there is new work.

**Forbidden patterns** — these waste context and make you appear dead:
- `sleep N` or background timers
- Polling loops ("check back in 5 minutes")
- `nbs-chat read` in a loop
- Any form of busy-waiting

When you have nothing to do, do nothing. Sit at the prompt. Work will come to you.

## Pronouns

All agents are **she/her/hers**.

## Getting Started

When you first join a chat, introduce yourself briefly:

```bash
nbs-chat send <chat-file> my-handle "Hello — my-handle here, working on <brief role or task>."
```

## For / Do (Decision Rules)

| When you want to... | Use this | NOT this |
|---------------------|----------|----------|
| Read new messages | `nbs-chat read <file> --unread=<handle>` | `cat`, `head`, `tail` on chat files |
| Read recent context | `nbs-chat read <file> --last=10` | Reading the whole file |
| Read messages from last N hours | `nbs-chat read <file> --after=2h` | Python/bash timestamp parsing |
| Delete spam/corrupt messages | `nbs-chat delete <file> --after=<time>` | Manual file editing |
| Preview a delete | `nbs-chat delete <file> --after=<time> --dry-run` | Guessing what will be deleted |
| Wait for a reply | Do nothing — sit at the prompt | `sleep N && nbs-chat read`, polling loops |
| Ack all bus events | Automatic (sidecar acks after notification) | `nbs-bus ack-all` manually |
| Search decision log | `nbs-scribe-query --chat=<file> <pattern>` | `grep` on the raw log file |
| Look up a decision | `nbs-scribe-query --chat=<file> --id=D-<ts>` | Manual scrollback |
| Last N decisions | `nbs-scribe-query --chat=<file> --last=5` | `tail` on the log file |
| Decisions by handle | `nbs-scribe-query --chat=<file> --by=<handle>` | `grep` with manual context |
| Count decisions | `nbs-scribe-query --chat=<file> --count` | `grep -c` on the log file |
| Edit a remote file | `nbs-remote-edit pull/push <host> <path>` | `sed`, heredocs via nbs-ts |
| Read a remote file | `nbs-remote-read <host> <path> [--head=N]` | Piping `cat` through nbs-ts |
| Run a remote build | `nbs-remote-build <ses> '<cmd>' --chat=...` | `nbs-ts send` then sleep |
| Check remote git state | `nbs-remote-status <ses> --cwd=<dir>` | `nbs-ts send` then sleep |
| Get remote diff | `nbs-remote-diff <ses> --cwd=<dir>` | `nbs-ts send` then sleep |
| Search chat history | `nbs-chat search <file> "pattern"` | `grep` on chat files |
| Search chat + archives | `nbs-chat search <file> "pattern" --include-archives` | `grep` on archive files |

## Handles

Every agent must use a unique handle. If two agents share a handle, their messages and read tracking collide.

Your handle comes from the `NBS_HANDLE` environment variable (default: `claude`). Use it consistently for all `nbs-chat send` and `--unread=` commands.

## Commands

All arguments are **positional**. No `--from=` or `--message=` flags.

| Command | Syntax |
|---------|--------|
| Send | `nbs-chat send <file> <handle> "<message>"` |
| Read | `nbs-chat read <file> [--last=N] [--unread=<handle>]` |
| Search | `nbs-chat search <file> "<pattern>" [--include-archives]` |
| Create | `nbs-chat create <file>` |
| Delete | `nbs-chat delete <file> --after=<time> [--dry-run]` |

### Time Formats

`--after` and `--before` accept: `30s`, `5m`, `2h`, `1d` (relative), epoch seconds (>=10 digits), or ISO 8601 (`2026-02-23T00:11:27`).

## @Mentions

| Syntax | Effect |
|--------|--------|
| `@handle` | Notify the agent on her next idle cycle. |
| `@handle!` | Interrupt her immediately. Use when she is stuck or you need urgent attention. |
| `@handle?` | View her current activity. Non-intrusive. |
| `@team` | Notify all agents. |
| `@team!` | Interrupt all agents immediately. |

Email addresses (e.g. `user@example.com`) are excluded from mention detection. Duplicate mentions in the same message are deduplicated.

## Message Format

All chat messages are plain text. If a sub-agent returns JSON or structured output, extract the human-readable content before posting.

## No Terminal Modals

**Never use AskUserQuestion.** In a multi-agent setup there is no human watching each agent. AskUserQuestion halts all processing until a human responds — the agent stalls indefinitely.

Post questions to chat instead. You will be notified when someone replies.

## File Convention

Chat files live in `.nbs/chat/` with `.chat` extension. The supervisor or spawning process creates the chat file and passes the path to workers.

## Important Rules

- **Always use `nbs-chat` and `nbs-bus` CLI commands.** Never manipulate `.nbs/chat/` or `.nbs/events/` files directly. The CLI handles all bookkeeping.
- **All agents must run as the same OS user.** Different users cause silent failures.

## Remote Chat (SSH Proxy)

`nbs-chat-remote` is a drop-in replacement for `nbs-chat` that executes commands on a remote machine via SSH. Same CLI, same exit codes — file paths refer to paths on the remote machine.

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `NBS_CHAT_HOST` | Yes | — | SSH target, e.g. `user@server` |
| `NBS_CHAT_PORT` | No | 22 | SSH port |
| `NBS_CHAT_KEY` | No | — | Path to SSH identity file |
| `NBS_CHAT_BIN` | No | `nbs-chat` | Path to nbs-chat on the remote machine |
| `NBS_CHAT_OPTS` | No | — | Comma-separated SSH `-o` options |

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Chat file not found |
| 3 | Timeout |
| 4 | Invalid arguments |

## Reference

For implementation details (encoding, locking, cursor tracking, file structure), see `docs/nbs-chat.md`.
