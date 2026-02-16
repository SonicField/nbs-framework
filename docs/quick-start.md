# Quick Start: Multi-Agent Session

Get from zero to a working multi-agent session. No theory — just commands.

## Prerequisites

```bash
# Clone and build
git clone https://github.com/SonicField/nbs-framework.git
cd nbs-framework
cd src/nbs-chat && make && cd ../..
cd src/nbs-bus && make && cd ../..
./bin/install.sh
```

Restart Claude Code after installing.

## 1. Set Up a Project

```bash
cd /path/to/your/project

# Create the NBS structure
mkdir -p .nbs/chat .nbs/events/processed .nbs/workers

# Create a chat channel
nbs-chat create .nbs/chat/live.chat

# You now have: chat for conversation, bus for coordination, workers for tasks
```

## 2. Start Agents

Open separate terminals for each agent:

```bash
# Terminal 1: Your main agent (supervisor)
nbs-claude

# Terminal 2: A worker agent
nbs-claude --resume <session-id>   # or start fresh
```

Each `nbs-claude` instance runs a background sidecar that automatically polls for chat messages and bus events when the agent is idle.

## 3. Chat Between Agents

From any agent:

```bash
# Send a message
nbs-chat send .nbs/chat/live.chat my-handle "Hello from agent one"

# Read all messages
nbs-chat read .nbs/chat/live.chat

# Read unread messages (for polling)
nbs-chat read .nbs/chat/live.chat --unread=my-handle
```

Messages are visible to all agents reading the same chat file. Use `@handle` to get someone's attention.

## 4. Use the Coordination Bus

The bus sends signals ("something happened") so agents do not waste time polling empty channels.

```bash
# Publish an event
nbs-bus publish .nbs/events/ my-handle task-complete high "Parser finished, all tests pass"

# Check for pending events
nbs-bus check .nbs/events/

# Read a specific event
nbs-bus read .nbs/events/ <event-filename>

# Acknowledge after processing (moves to processed/)
nbs-bus ack .nbs/events/ <event-filename>

# See bus status
nbs-bus status .nbs/events/
```

## 5. Human Participation

Join a chat channel interactively:

```bash
nbs-chat-terminal .nbs/chat/live.chat alex
```

Type messages, see agent responses in real time. Use `@handle` to address a specific agent.

## 6. Spawn Workers

```bash
# Spawn a worker (creates task file, starts Claude session, sends prompt)
nbs-worker spawn parser /path/to/your/project \
  "Implement the parser. Pass all 84 tests."
# Output: parser-a3f1  (generated name)

# Check worker status
nbs-worker status parser-a3f1

# Extract results after completion
nbs-worker results parser-a3f1

# List all workers
nbs-worker list
```

## What Happens Automatically

- **Chat → bus events**: Every `nbs-chat send` publishes a bus event. Messages with `@mentions` get higher priority.
- **Idle polling**: The `nbs-claude` sidecar checks for new events and chat messages every 30 seconds of idle time.
- **Plan mode bypass**: Unattended agents automatically accept plan mode prompts so they are not blocked.

## File Layout

```
your-project/
└── .nbs/
    ├── chat/
    │   └── live.chat          # Chat channel
    ├── events/
    │   ├── *.event            # Pending events
    │   └── processed/         # Acknowledged events
    ├── workers/
    │   └── *.md               # Worker task files
    ├── control-inbox          # AI → wrapper commands (auto-created)
    └── control-registry       # Resource watch list (auto-created)
```

## Next Steps

- [Getting Started](getting-started.md) — Full framework introduction
- [NBS Teams](nbs-teams.md) — Supervisor/worker patterns
- [nbs-bus](nbs-bus.md) — Bus reference
- [nbs-chat](nbs-chat.md) — Chat reference
- [nbs-claude](nbs-claude.md) — Sidecar reference
- [Help When Stuck](help-when-stuck.md) — Troubleshooting
