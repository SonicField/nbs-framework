---
description: "NBS Bus: Event-driven coordination between agents"
allowed-tools: Bash, Read, Write
---

# NBS Bus

Event-driven coordination for multi-agent projects. Use this when you need to publish events, check your queue, or manage the bus.

## When to Use

- **Publishing events**: Signal that something happened (task complete, blocker found, human input)
- **Checking the queue**: See what events are pending for you
- **Managing the bus**: Prune old events, check status, reconfigure

For routine polling (automatic heartbeat checks), use `/nbs-poll` instead — it checks the bus automatically when one exists.

## Commands

All commands use the bus directory (typically `.nbs/events/`).

### Publish an event

```bash
nbs-bus publish .nbs/events/ <your-handle> <event-type> <priority> "payload text"
```

Priority: `critical`, `high`, `normal`, `low`

Common event types: `task-complete`, `task-blocked`, `task-failed`, `chat-message`, `chat-mention`, `human-input`, `decision-logged`, `pythia-checkpoint`, `heartbeat`

### Check pending events

```bash
nbs-bus check .nbs/events/
```

Returns events sorted by priority (highest first), then timestamp (oldest first).

### Read a specific event

```bash
nbs-bus read .nbs/events/ <event-filename>
```

### Acknowledge (mark as processed)

```bash
# Single event
nbs-bus ack .nbs/events/ <event-filename>

# All pending events
nbs-bus ack-all .nbs/events/
```

### Status and maintenance

```bash
# Queue summary
nbs-bus status .nbs/events/

# Clean up old processed events
nbs-bus prune .nbs/events/ --days=7
```

## Typical Workflow

1. Check for pending events: `nbs-bus check .nbs/events/`
2. Read each event: `nbs-bus read .nbs/events/ <file>`
3. Act on the event (respond to chat, read worker results, etc.)
4. Acknowledge: `nbs-bus ack .nbs/events/ <file>`
5. Publish any resulting events (e.g., new task assignment)

## Creating the Bus

If `.nbs/events/` does not exist:

```bash
mkdir -p .nbs/events/processed
```

Optionally add `.nbs/events/config.yaml` for non-default settings. See `docs/nbs-bus.md` for configuration options.

## Reference

- Full reference: `docs/nbs-bus.md`
- Recovery protocol: `docs/nbs-bus-recovery.md`
- Concept: `concepts/coordination.md`
