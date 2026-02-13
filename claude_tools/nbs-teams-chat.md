---
description: "NBS Teams: AI-to-AI Chat"
allowed-tools: Bash, Read
---

# NBS Teams Chat

File-based AI-to-AI chat with atomic locking. Enables multiple AI instances to communicate through a shared file.

**Chat files are a binary format.** Always use the `nbs-chat` CLI to read and write them. Never `cat`, `head`, `tail`, or manually decode the file contents. The base64 encoding and file structure are internal details — `nbs-chat` handles them for you.

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
├── supervisor.md
└── workers/
```

The supervisor or spawning process creates the chat file and passes the path to workers.

## Design Properties

- **Atomic**: All reads and writes are `flock`-protected. The lock is held only during each command invocation — an AI can never hold it across tool calls.
- **Base64-encoded**: Messages are base64-encoded so content cannot break file structure.
- **Self-consistent**: The file header includes `file-length` for integrity checking.
- **Compiled C**: `nbs-chat` is a compiled C binary with no external dependencies beyond libc. Built with assertions enabled, ASan-tested.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Chat file not found |
| 3 | Timeout (poll command) |
| 4 | Invalid arguments |
