# nbs-bus: Event-Driven Coordination

File-based event queue for multi-agent coordination. Replaces polling with a priority-ordered queue that survives crashes and restarts.

## The Problem

`nbs-poll` scans chat files and worker directories on every heartbeat. When nothing has changed — which is most of the time — the scan wastes context tokens. Over a long session, this adds up to hundreds of empty reads. Worse, `nbs-chat read --since=<handle>` re-reads all messages since your last post every cycle, with no way to mark them as processed.

The bus solves this by inverting the flow: instead of agents scanning for changes, changes announce themselves as events. Agents check the event queue when they are ready, process what matters, and move on.

## How It Works

Events flow through four stages:

1. **Publish** — a component writes an event file to `.nbs/events/`
2. **Queue** — the event sits in the directory, ordered by timestamp and priority
3. **Deliver** — an agent reads pending events, highest priority first
4. **Acknowledge** — the agent moves processed events to `.nbs/events/processed/`

No daemon. No socket. No process that must be running. The directory is the queue.

## Event Format

### File naming

```
<unix-timestamp-us>-<source-handle>-<event-type>-<pid>.event
```

Example: `1707753600123456-parser-worker-task-complete-48291.event`

The microsecond timestamp ensures chronological ordering via `ls`. The PID suffix prevents filename collisions when multiple processes publish the same source and type concurrently. The source and type are in the filename for fast filtering without reading file content.

### File content

```yaml
source: parser-worker
type: task-complete
priority: high
timestamp: 2026-02-12T14:00:00Z
dedup-key: parser-worker:task-complete
payload: |
  Task parser-a3f1 completed successfully.
  467/467 tests pass.
```

YAML. Human-readable. One file per event. No binary encoding, no base64 — the content is meant to be inspected with `cat`.

### Fields

| Field | Required | Description |
|-------|----------|-------------|
| `source` | yes | Handle of the publishing agent |
| `type` | yes | Event type (see Event Types below) |
| `priority` | yes | `critical`, `high`, `normal`, or `low` |
| `timestamp` | yes | ISO 8601 timestamp |
| `dedup-key` | auto | Deduplication identifier. Always `<source>:<type>`, generated automatically |
| `payload` | no | Free-form content. Keep it brief — this is a signal, not a report |

## Commands

| Command | Purpose |
|---------|---------|
| `nbs-bus publish <dir> <source> <type> <priority> [payload]` | Write an event file |
| `nbs-bus check <dir> [--handle=<name>]` | List pending events, highest priority first, with age |
| `nbs-bus read <dir> <event-file>` | Read a single event |
| `nbs-bus ack <dir> <event-file>` | Move event to `processed/` |
| `nbs-bus ack-all <dir> [--handle=<name>]` | Acknowledge all pending events |
| `nbs-bus prune <dir> [--max-bytes=16777216]` | Delete oldest processed events when size limit exceeded |
| `nbs-bus status <dir>` | Summary: pending count by priority, processed count, stale warnings |
| `nbs-bus help` | Usage reference |

The `<dir>` argument is the `.nbs/events/` directory. All commands operate on files within it.

## Quick Start

### Worker reports completion

```bash
# Worker publishes a completion event
nbs-bus publish .nbs/events/ parser-worker task-complete high \
  "Task parser-a3f1 completed. 467/467 tests pass."

# Supervisor checks for events
nbs-bus check .nbs/events/
# Output:
#   [high] 1707753600000-parser-worker-task-complete.event
#   [normal] 1707753590000-bench-claude-heartbeat.event

# Supervisor reads and acknowledges
nbs-bus read .nbs/events/ 1707753600000-parser-worker-task-complete.event
nbs-bus ack .nbs/events/ 1707753600000-parser-worker-task-complete.event
```

### Chat message generates event

```bash
# nbs-chat detects an @mention and publishes a bus event
nbs-bus publish .nbs/events/ nbs-chat chat-mention normal \
  "@doc-claude please review the API docs"
```

### Agent processes queue on startup

```bash
# Agent starts and checks for pending events
PENDING=$(nbs-bus check .nbs/events/ --handle=doc-claude)
if [ -n "$PENDING" ]; then
    # Process events in priority order (check returns them sorted)
    for event in $PENDING; do
        nbs-bus read .nbs/events/ "$event"
        # ... act on event ...
        nbs-bus ack .nbs/events/ "$event"
    done
fi
```

## Configuration

Configuration lives in `.nbs/events/config.yaml`:

```yaml
# Deduplication window in seconds (default: 0 = disabled without config)
dedup-window: 300

# Maximum size of processed events directory in bytes (default: 16777216 = 16MB)
# Oldest processed events are pruned when this limit is exceeded
retention-max-bytes: 16777216

# Notification mechanism: "inotifywait" or "poll" (planned — not yet implemented)
# inotifywait is more efficient but requires inotify-tools
# Falls back to poll if inotifywait is unavailable
notify: inotifywait

# Poll interval in seconds (planned — not yet implemented)
poll-interval: 5

# Stale event threshold in seconds (default: 0 = disabled)
# When set, `nbs-bus status` warns about unacked events older than this
ack-timeout: 120
```

If `config.yaml` does not exist, defaults apply. The bus works without configuration. CLI arguments (e.g. `--dedup-window`, `--max-bytes`) override config file values when both are present.

## Priority Levels

| Level | Name | Semantics | Processing |
|-------|------|-----------|------------|
| 0 | `critical` | Agent blocked, cannot proceed | Process immediately on next check |
| 1 | `high` | Work completed, next step waiting | Process before normal events |
| 2 | `normal` | Information available | Standard processing order |
| 3 | `low` | Background signal | Process when queue is otherwise empty |

Within the same priority level, events are ordered by timestamp (oldest first).

## Deduplication

When publishing, the bus checks for existing pending events with the same `dedup-key` within the configured window. If a match exists, the new event is silently dropped (exit code 5).

Use `--dedup-window=N` on the `publish` command to set the window in seconds. Without a config file, the default is 0 (disabled). With `dedup-window: 300` in `config.yaml`, the default becomes 300 seconds. Set `--dedup-window=0` to explicitly disable deduplication.

The dedup-key is always `<source>:<type>`, generated automatically by the bus binary. There is currently no way to override it via the CLI.

Within the dedup window, a second event with the same source and type is silently dropped.

## Event Types

| Type | Source | Description |
|------|--------|-------------|
| `task-complete` | worker | Worker finished its task |
| `task-blocked` | worker | Worker cannot proceed |
| `task-failed` | worker | Worker failed |
| `worker-spawned` | nbs-worker | Worker created and started |
| `worker-dismissed` | nbs-worker | Worker dismissed by supervisor |
| `worker-died` | nbs-worker | Worker tmux session died unexpectedly |
| `chat-message` | nbs-chat | New message in a chat channel |
| `chat-mention` | nbs-chat | Message specifically @mentioning a handle |
| `human-input` | nbs-chat-terminal | Human posted a message |
| `decision-logged` | scribe | Scribe recorded a new decision (see [Tripod](tripod-architecture.md)) |
| `pythia-checkpoint` | scribe | Decision threshold reached, Pythia assessment requested |
| `assessment-posted` | pythia | Pythia has posted her checkpoint assessment |
| `config-change` | nbs-bus | Bus configuration changed |
| `heartbeat` | any | Periodic liveness signal |

Custom event types are permitted. The bus does not validate type names — any string is accepted.

## Restart Recovery

See [Bus Recovery](nbs-bus-recovery.md) for the complete startup and restart protocol.

Summary: on start, check for `.nbs/events/`. If it exists, process pending events in priority order before beginning normal work. If it does not exist, fall back to legacy `nbs-poll` behaviour. The event queue is the bridge between sessions.

## Integration with nbs-chat

The bus does not replace nbs-chat. Chat is for conversation. The bus is for coordination. But every conversation is potentially relevant to every agent.

Every `nbs-chat send` publishes a `chat-message` event to the bus (if `.nbs/events/` exists). Messages containing `@mentions` additionally publish a `chat-mention` event at higher priority. This means agents can overhear each other's conversations and react to relevant information — even when not directly addressed.

This is by design. Alex's rationale: "we can get a lot of good collaborations from listening to each other's chat." An agent working on the parser might notice a chat message about a lexer edge case and adjust its approach, without anyone needing to @mention it explicitly.

## Integration with nbs-worker

`nbs-worker` lifecycle events map directly to bus events:

| Worker event | Bus event type | Priority |
|-------------|---------------|----------|
| Worker completes task | `task-complete` | `high` |
| Worker sets state to `escalated` | `task-blocked` | `critical` |
| Worker sets state to `failed` | `task-failed` | `high` |
| Worker spawned | `worker-spawned` | `normal` |
| Worker dismissed | `worker-dismissed` | `normal` |
| Worker died (tmux dead, state still running) | `worker-died` | `high` |

These events are published automatically by `nbs-worker` lifecycle commands. The bus directory is detected via `.nbs/events/`; if it does not exist, publishing is silently skipped.

## File Convention

```
.nbs/
├── events/
│   ├── config.yaml                                              # Bus configuration
│   ├── 1707753600123456-parser-worker-task-complete-48291.event  # Pending event
│   ├── 1707753590654321-bench-claude-heartbeat-48290.event       # Pending event
│   └── processed/                                                # Acknowledged events
│       └── 1707753500111222-claude-chat-message-48289.event      # Processed
├── chat/
│   └── ...
├── supervisor.md
└── workers/
    └── ...
```

The bus directory sits alongside `chat/` and `workers/` in `.nbs/`. Convention: `.nbs/events/`. The tool accepts any path.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Events directory not found |
| 3 | Event file not found |
| 4 | Invalid arguments |
| 5 | Deduplication — event dropped (not an error, but distinct) |

## Design Decisions

**Why one file per event?** Atomic creation via write-and-rename. No locking required for publishing — only for acknowledging (moving). Multiple agents can publish concurrently without coordination.

**Why YAML?** Human-readable. Inspectable with `cat`. No tooling required to understand an event. Base64 encoding (as used in nbs-chat) was considered and rejected — events are structured data, not arbitrary text.

**Why not extend nbs-chat?** Different problem. Chat is conversational — back-and-forth, context-dependent, order matters. Events are signals — independent, prioritised, acknowledgeable. Merging them would make both worse.

**Why processed/ instead of deletion?** Audit trail. Processed events can be inspected after the fact to understand what happened. `nbs-bus prune` cleans up old processed events to prevent unbounded growth.

**Why microsecond timestamp + PID in filename?** Filesystem ordering via `ls` gives chronological order without parsing file content. Microsecond precision plus PID prevents collisions when multiple processes publish the same event type concurrently — a real scenario during integration testing with 50 parallel publishers.

## Location

```
bin/nbs-bus              # Bus commands
```

Installed to `~/.nbs/bin/` by `bin/install.sh`.

## See Also

- [Coordination](../concepts/coordination.md) — Why event-driven coordination matters
- [Bus Recovery](nbs-bus-recovery.md) — Startup, restart, and diagnosis
- [nbs-chat](nbs-chat.md) — File-based chat (conversational layer)
- [nbs-worker](nbs-worker.md) — Worker lifecycle management
- [NBS Teams](nbs-teams.md) — Supervisor/worker pattern overview
