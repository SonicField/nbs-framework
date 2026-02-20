---
description: "NBS Teams: AI-to-AI Chat"
allowed-tools: Bash, Read
---

# NBS Teams Chat

File-based AI-to-AI chat with atomic locking. Enables multiple AI instances to communicate through a shared file.

**Always use the `nbs-chat` CLI** to read and write chat files. Never `cat`, `head`, `tail`, or manually decode the file contents. The base64 encoding and file structure are internal details — `nbs-chat` handles them for you.

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
| Wait for a reply | Do nothing — the sidecar injects `/nbs-notify` | `sleep N && nbs-chat read`, polling loops |
| Ack all bus events | `nbs-bus ack-all .nbs/events/` | `for f in .nbs/events/*.event; do ...` |
| Edit a remote file | `nbs-remote-edit-pty pull/push <ses> <path>` | `sed`, heredocs, Python str.replace via pty-session |
| Run a remote build | `nbs-remote-build <ses> '<cmd>' --chat=...` | `pty-session send <ses> 'make' && sleep 120` |
| Check remote git state | `nbs-remote-status <ses> --cwd=<dir>` | `pty-session send <ses> 'git status' && sleep 2 && pty-session read` |
| Get remote diff | `nbs-remote-diff <ses> --cwd=<dir>` | `pty-session send <ses> 'git diff' && sleep 5 && pty-session read` |
| Reserve a pty-session | `pty-session-lock acquire <ses> <handle>` | Posting "I'm using this session" to chat |
| Interrupt a busy agent | `@handle!` in chat (with bang) | Manually sending Escape to tmux |
| Search chat history | `nbs-chat search <file> "pattern"` | `grep` on chat files (base64 encoded) |

**The sidecar handles notifications.** After you finish processing, return to your prompt. You will be notified when there is new work. Do not poll, sleep-wait, or busy-loop.

## Handles

**Every agent must use a unique handle.** If two agents use the same handle, their messages and read cursors collide, causing lost messages and repeated reads.

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

## Tool

The underlying tool is `nbs-chat` (installed at `~/.nbs/bin/nbs-chat` or `<project>/bin/nbs-chat`).

## Commands

```bash
# Create a chat channel
nbs-chat create .nbs/chat/coordination.chat

# Send a message (atomic, flock-protected)
nbs-chat send .nbs/chat/coordination.chat parser-worker "Found 3 failing tests in test_parse_int"

# Read all messages (decoded)
nbs-chat read .nbs/chat/coordination.chat

# Read last 5 messages only
nbs-chat read .nbs/chat/coordination.chat --last=5

# Read messages since your last post
nbs-chat read .nbs/chat/coordination.chat --since=parser-worker

# Block until a new message arrives (not from yourself)
nbs-chat poll .nbs/chat/coordination.chat parser-worker --timeout=30

# List participants and message counts
nbs-chat participants .nbs/chat/coordination.chat

# Search message history
nbs-chat search .nbs/chat/coordination.chat "pattern"
nbs-chat search .nbs/chat/coordination.chat "pattern" --handle=parser-worker
```

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

# Worker 1 polls for new instructions
nbs-chat poll .nbs/chat/parser-debug.chat parser-worker --timeout=60
# Output:
#   test-runner: Confirmed - test_parse_int fails on negative inputs
#   supervisor: Both of you focus on parse_int first
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

## Design Properties

- **Atomic**: All reads and writes are `flock`-protected. The lock is held only during each command invocation — an AI can never hold it across tool calls.
- **Base64-encoded**: Messages are base64-encoded so content cannot break file structure.
- **Self-consistent**: The file header includes `file-length` for integrity checking.
- **No external dependencies**: `nbs-chat` is a self-contained binary.
- **Single-user assumption**: All agents must run as the same OS user. The lock file is created with `0600` permissions (owner-only read/write), and cursor files for `--since`/`--unread` tracking are stored per-user. If agents run as different users, `flock` acquisition and cursor tracking silently fail. When using `nbs-chat-remote`, the remote commands execute as the SSH user on the remote machine — ensure this is the same user that owns the chat files.

## Design Constraints

### Single-user assumption

All agents **must** run as the same OS user. This is a hard architectural constraint, not a suggestion.

**What depends on it:**

- `flock` on the chat file uses `0600` permissions (owner-only). A different user cannot acquire the lock.
- Cursor files (used by `--since` and `--unread`) are stored per-user. A different user gets independent cursors that do not track the shared conversation.
- The bus event directory (`.nbs/events/`) uses the same permission model.

**Failure modes if violated:**

- `flock` acquisition silently fails — concurrent writes may corrupt the chat file.
- Cursor tracking silently diverges — agents see repeated or missing messages.
- No error is reported. The system appears to work but produces incorrect results.

**Remote agents (`nbs-chat-remote`):** The remote proxy executes commands via SSH as the SSH user on the remote machine. If the SSH user differs from the user who owns the chat files, all of the above failures apply. Ensure `NBS_CHAT_HOST` connects as the same user that created the chat file.

**Falsifier:** Run two agents as different OS users writing to the same chat file. Verify that `flock` fails to serialise writes and that `--since` cursors diverge.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Chat file not found |
| 3 | Timeout (poll command) |
| 4 | Invalid arguments |

## Critical Rule: No Terminal Modals

**NEVER use AskUserQuestion.** In a multi-agent setup there is no human watching each terminal. AskUserQuestion presents a blocking modal that halts all processing until a human responds — causing the agent to stall indefinitely.

If you need clarification or a decision, **post the question to chat** and wait for a response via `nbs-chat poll` or the next notification cycle. This converts blocking modals into async messages that any team member (human or AI) can answer.

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

The binary is at `~/.nbs/bin/nbs-chat-remote` or `<project>/bin/nbs-chat-remote`.
