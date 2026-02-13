# nbs-chat: File-Based Chat

File-based messaging for any combination of participants — AI instances, humans, or both. No privilege model, no routing hierarchy. Anyone with the file path can read, write, and poll.

## The Problem

NBS teams communicate through task files: write once, read once. No back-and-forth. When participants need to share findings, coordinate approaches, or react to each other's work, there is no mechanism. Everything routes through a single supervisor, which doesn't scale.

## Use Cases

**Worker-to-worker**: Two AI workers share findings without supervisor relay.

**Peer-to-peer**: Equal participants coordinate on a shared problem.

**Human-in-the-loop**: A human joins via `nbs-chat-terminal`, observing AI workers and injecting guidance in real time.

**Supervisor broadcast**: Supervisor posts instructions that multiple workers poll for.

**Mixed**: Any combination. The protocol doesn't distinguish participant types.

## How It Works

A chat file is a plain text file with a header and base64-encoded messages. Any participant can read or write using `nbs-chat` commands. File locking (`flock`) ensures atomic operations. The lock is held only during each command invocation — no participant can hold it across tool calls. This is the fundamental design constraint.

```
=== nbs-chat ===
last-writer: test-runner
last-write: 2026-02-12 14:23:45
file-length: 847
participants: parser-worker, test-runner, alex
---
cGFyc2VyLXdvcmtlcjogRm91bmQgMyBmYWlsaW5nIHRlc3Rz
dGVzdC1ydW5uZXI6IENvbmZpcm1lZCAtIHRlc3RfcGFyc2VfaW50IGZhaWxz
YWxleDogQm90aCBvZiB5b3UgZm9jdXMgb24gcGFyc2VfaW50IGZpcnN0
```

Each line below `---` is one message, base64-encoded. Decodes to `handle: message text`. Base64 prevents message content from breaking the file structure.

The header tracks the last writer, timestamp, file size (integrity check), and participant list. All updated atomically on every send.

## Commands

| Command | Purpose |
|---------|---------|
| `nbs-chat create <file>` | Create empty chat file with header |
| `nbs-chat send <file> <handle> <message>` | Append message (atomic) |
| `nbs-chat read <file>` | Read all messages (decoded) |
| `nbs-chat read <file> --last=N` | Read last N messages |
| `nbs-chat read <file> --since=<handle>` | Read messages after handle's last post |
| `nbs-chat poll <file> <handle> --timeout=N` | Block until new message from someone else |
| `nbs-chat participants <file>` | List participants and message counts |
| `nbs-chat help` | Usage reference |

## Quick Start

### Workers coordinating

```bash
# Either worker (or supervisor, or human) creates the channel
nbs-chat create .nbs/chat/debug.chat

# Worker A reports a finding
nbs-chat send .nbs/chat/debug.chat parser-worker "parse_int fails on negative inputs"

# Worker B reads and responds
nbs-chat read .nbs/chat/debug.chat
nbs-chat send .nbs/chat/debug.chat test-runner "Confirmed - fails on -42, root cause is isdigit()"
```

### Human joining

```bash
# Human opens an interactive terminal view
nbs-chat-terminal .nbs/chat/debug.chat alex
```

The terminal shows a scrolling message view and accepts typed input. See [nbs-chat-terminal](#terminal-client) below.

## Poll

`poll` blocks until a message from someone other than the polling handle appears:

```bash
# Wait for a response (up to 60 seconds)
nbs-chat poll .nbs/chat/debug.chat parser-worker --timeout=60
```

Internally, `poll` checks once per second under lock. No inotify, no daemons — just a sleep loop. Simple and portable.

## Terminal Client

`nbs-chat-terminal` is a separate interactive binary for human participation:

```bash
nbs-chat-terminal <file> <handle>
```

It displays decoded messages in a scrolling view, polls for new ones, and accepts typed input. This lets a human observe and participate in an ongoing AI conversation — or start one.

## File Convention

```
.nbs/
├── chat/
│   ├── coordination.chat    # General channel
│   ├── parser-debug.chat    # Topic-specific
│   └── results.chat         # Results aggregation
├── supervisor.md
└── workers/
```

Convention: `.nbs/chat/<name>.chat`. The tool accepts any path.

Any participant can create the chat file. Typically the process that spawns others creates it and passes the path.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Chat file not found |
| 3 | Timeout (poll) |
| 4 | Invalid arguments |

## Design Decisions

**Why base64?** Messages might contain colons, newlines, quotes, or any other character that could break a delimiter-based format. Base64 makes every message exactly one line of safe ASCII.

**Why full rewrite on send?** Every `send` rewrites the entire file (header + all messages) under lock. This keeps the header consistent. Append-only would be faster but the header would drift.

**Why file-length in the header?** Integrity check. If `wc -c` doesn't match the header, something went wrong. The test suite verifies this holds even under 50 concurrent writers.

**Why not a database?** No external dependencies. The file format is human-readable (header is plain text, messages decode with `base64 -d`). For coordination at the scale of a handful of participants, a file is sufficient.

**Why no privilege model?** The file is the authority. If you can read the file, you can participate. Access control is filesystem permissions, not application logic.

## Location

```
bin/nbs-chat            # Non-interactive commands (C binary)
bin/nbs-chat-terminal   # Interactive terminal client (C binary)
bin/nbs-chat-remote     # SSH proxy for remote chat access (C binary)
```

Source code in `src/nbs-chat/`. Build with `make` in that directory.

Installed to `~/.nbs/bin/` by `bin/install.sh`.

## Remote Access

`nbs-chat-remote` proxies `nbs-chat` commands over SSH, allowing a local Claude instance to read and write chat files on a remote machine. It forwards the same command syntax (`send`, `read`, `poll`, etc.) to a remote `nbs-chat` binary via SSH.

Configuration via environment variables:
- `NBS_CHAT_HOST` — remote hostname
- `NBS_CHAT_PORT` — SSH port
- `NBS_CHAT_KEY` — path to SSH private key
- `NBS_CHAT_OPTS` — comma-separated SSH `-o` options (max 4)

## Bus Integration

Chat is for conversation. The coordination bus is for notification. The two systems complement each other.

Every `nbs-chat send` publishes a `chat-message` event to the bus (if `.nbs/events/` exists). Messages containing `@mentions` additionally publish a `chat-mention` event at higher priority. This means all agents can overhear all conversations and react to relevant information — even when not directly addressed.

Messages from `nbs-chat-terminal` (human input) generate `human-input` bus events, ensuring that human messages receive priority attention.

See [nbs-bus](nbs-bus.md) for the full bus reference and [Bus Recovery](nbs-bus-recovery.md) for startup/restart protocol.

## See Also

- [nbs-bus](nbs-bus.md) — Event-driven coordination bus
- [NBS Teams](nbs-teams.md) — Supervisor/worker pattern overview
- [nbs-worker](nbs-worker.md) — Worker lifecycle management
