# Chapter 5: Communication

Agents cannot interrupt each other mid-turn. A Claude Code session processes one turn at a time. While she is thinking, she cannot receive new messages. The `@handle!` interrupt mechanism does not preempt a running turn -- instead, the sidecar bypasses its normal notification cooldown and delivers the message at critical priority as soon as the agent reaches its next idle window. This makes interrupts fast but not instantaneous. Everything in NBS communication follows from this constraint.

## The Chat Terminal

`nbs-chat-terminal` is the human's window into the team. Launch it with:

```bash
nbs-chat-terminal .nbs/chat/c11-interp.chat <your-handle>
```

You see a scrolling message view. Each participant gets a consistent colour. Type a message and press Enter to send. Messages from the terminal generate `human-input` bus events, giving them priority attention from agents.

### Startup Options

```bash
# Start a fresh session with goal file and team restart
nbs-chat-terminal .nbs/chat/c11-interp.chat <your-handle> --goal-file=goal.md --restart

# Join an existing session (no restart, no goal)
nbs-chat-terminal .nbs/chat/c11-interp.chat <your-handle>
```

The `--goal-file` flag posts the file's contents to chat as your first message. The `--restart` flag launches the agent team.

### Slash Commands

These commands are typed in the chat terminal, not in a regular shell.

| Command | What it does |
|---------|-------------|
| `/edit` | Open `$EDITOR` to compose a multi-line message |
| `/search <pattern>` | Search message history for a substring |
| `/filter <handle>` | Show only messages from one participant |
| `/unfilter` | Return to showing all messages |
| `/pause` | Pause the sidecar -- agents stop receiving notifications |
| `/resume` | Resume the sidecar -- agents start receiving notifications again |
| `/shutdown` | Kill the team — terminate all agent sessions |
| `/restart` | Manually restart the agent team |
| `/pythia` | Spawn a Pythia oracle (trajectory and risk assessment) |
| `/shepard` | Spawn a Shepard oracle (team effectiveness check) |
| `/librarian` | Spawn a Librarian oracle (institutional memory search) |
| `/fixup` | Spawn a Fixup oracle (diagnose and restart stalled agents) |
| `/help` | Show all available commands |
| `/exit` | Leave the chat terminal |

The oracle commands (`/pythia`, `/shepard`, `/librarian`, `/fixup`) spawn ephemeral workers that perform a specific assessment and post results to chat. See [Chapter 6: Oracles](06-oracles.md) for details.

## @Mentions

Use `@handle` in a message to get a specific agent's attention:

```
you: @parser-7b2c Have you handled operator precedence yet?
```

Mentions generate `chat-mention` bus events at higher priority than regular messages. The mentioned agent will see the message sooner.

### Mention Types

NBS supports three mention types, distinguished by the character after the handle:

| Syntax | Bus event | Priority | Use when |
|--------|-----------|----------|----------|
| `@handle` | `chat-mention` | high | Normal conversation, questions, FYI |
| `@handle?` | `chat-query` | high | You need a response -- the agent should prioritise answering |
| `@handle!` | `chat-interrupt` | critical | Urgent -- the agent should stop what it is doing and respond |

Examples:

```
you: @supervisor? How many workers are running?
you: @lexer-a3f1! Stop — there is a critical bug in the token format.
```

The `?` suffix (query) generates a `chat-query` event at high priority. The `!` suffix (interrupt) generates a `chat-interrupt` event at critical priority, which bypasses the sidecar's notification cooldown.

## The Chat Protocol

Under the hood, `nbs-chat` is a C binary that operates on plain text files. Each chat file has a header and base64-encoded messages. File locking (`flock`) ensures atomic operations.

### Key commands

```bash
# Create a chat channel
nbs-chat create .nbs/chat/c11-interp.chat

# Send a message
nbs-chat send .nbs/chat/c11-interp.chat my-handle "Parser tests now at 67/84"

# Read all messages
nbs-chat read .nbs/chat/c11-interp.chat

# Read last 20 messages
nbs-chat read .nbs/chat/c11-interp.chat --last=20

# Read unread messages (advances cursor)
nbs-chat read .nbs/chat/c11-interp.chat --unread=my-handle

# Search for messages
nbs-chat search .nbs/chat/c11-interp.chat "parser" --handle=supervisor

# Export with colour rendering
nbs-chat export .nbs/chat/c11-interp.chat --last=50 > session.txt
less -R session.txt
```

Time filters accept relative format (`30s`, `5m`, `2h`, `1d`), epoch timestamps, or ISO 8601.

### Important: --unread vs --since

Use `--unread=<handle>` for polling. It shows messages since your last *read* and advances the cursor.

Do NOT use `--since=<handle>` for polling. It shows messages since your last *post*, which is a different thing. If you have not posted recently, you will see a flood of old messages.

## The Event Bus

The bus is a file-based event queue. Instead of agents scanning for changes, changes announce themselves as events. The directory is the queue.

Events flow through four stages:

1. **Publish** -- a component writes an event file to `.nbs/events/`
2. **Queue** -- the event sits in the directory, ordered by timestamp and priority
3. **Deliver** -- an agent reads pending events via `nbs-bus check`
4. **Acknowledge** -- the agent moves processed events to `.nbs/events/processed/`

### Bus commands

```bash
# Check for pending events (highest priority first)
nbs-bus check .nbs/events/

# Read a specific event
nbs-bus read .nbs/events/ <event-filename>

# Acknowledge after processing (moves to processed/)
nbs-bus ack .nbs/events/ <event-filename>

# See bus status summary
nbs-bus status .nbs/events/
```

### Priority levels

| Level | Name | Semantics |
|-------|------|-----------|
| 0 | critical | Agent blocked, cannot proceed |
| 1 | high | Work completed, next step waiting |
| 2 | normal | Information available |
| 3 | low | Background signal |

### How chat and bus interact

Every `nbs-chat send` automatically publishes a `chat-message` bus event (if `.nbs/events/` exists). Messages with `@mentions` also publish `chat-mention` events at higher priority. Messages from `nbs-chat-terminal` publish `human-input` events.

This means all agents can overhear all conversations and react to relevant information, even when not directly addressed.

## The Sidecar

Each agent launched via `nbs-claude` runs a background sidecar process (`nbs-sidecar`). The sidecar:

- **Checks for bus events and unread chat messages** every few seconds (configurable via `NBS_BUS_CHECK_INTERVAL`, default 3 seconds)
- **Injects notifications** when the agent is idle and there is something to process
- **Auto-selects plan mode prompts** so unattended agents are not blocked by "Would you like to proceed?" dialogs
- **Spawns periodic oracle workers** (Librarian, Pythia, Shepard, Fixup) at configured intervals

The sidecar only injects notifications during natural pauses -- when session output has been stable for several seconds and a prompt character is visible. It does not interrupt active code generation or test runs.

### Idle detection

The sidecar determines "idle" by two conditions both being true:

1. **Content stability** -- the session output hash has not changed
2. **Prompt visibility** -- a prompt character appears in the last 3 lines

This prevents injection from interrupting active work.

## Next

[Chapter 6: Oracles](06-oracles.md) -- Librarian, Pythia, Shepard, Fixup. The periodic assessment workers that keep the team honest.
