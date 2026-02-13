# Coordination

Agents cannot interrupt each other. This is not a bug. It is the fundamental constraint.

A Claude session processes one turn at a time. While it is thinking, it cannot receive messages. While it is waiting, it cannot send them. Two agents in the same project are two isolated processes sharing a filesystem. Everything follows from this.

## The Problem

Polling wastes attention. An agent checking for messages every few seconds burns context tokens on empty reads. Over a long session, hundreds of poll cycles produce nothing — each one consuming the same resource (context) that the agent needs for its actual work.

But without polling, agents are deaf. A worker completes its task and writes results to a file. The supervisor does not know until it checks. A human posts a message. No one reads it until the next poll. The gap between "information available" and "information consumed" is dead time.

The tension: poll frequently and waste attention, or poll rarely and miss events.

## File as Authority

NBS already uses files as the source of truth. Chat messages are files. Worker task files are files. Supervisor state is a file. The coordination bus extends this principle: events are files.

An event file is a fact. It exists or it does not. Its content is YAML — human-readable, machine-parseable. Its filename encodes when it happened, who caused it, and what kind of event it is. No daemon required. No database. No process that must be running for the system to work.

When a machine dies, the events survive. When a Claude session restarts, the queue is still there. This is the property that makes file-based coordination superior to socket-based or pipe-based alternatives for AI agent work: **crash recovery is free**.

## Priority as Triage

Not all events matter equally. A worker blocked and unable to proceed is more urgent than a status update. A human message demands faster attention than a heartbeat.

Priority is triage, not privilege. High-priority events are processed first when an agent checks its queue. They do not interrupt — nothing can interrupt a Claude turn — but they jump the queue when the next check happens.

| Priority | Semantics | Example |
|----------|-----------|---------|
| Critical | Agent blocked, cannot proceed | Worker needs human input |
| High | Work completed, next step waiting | Worker finished task |
| Normal | Information available | Chat message, status update |
| Low | Background signal | Heartbeat, routine check |

## Availability vs Presence

An agent that is offline still has a queue. Events accumulate while it is away. When it comes back, it processes them in priority order.

This is distinct from presence ("is the agent running right now?"). Presence is transient — sessions start and stop. Availability is structural — the queue exists as long as the directory exists.

Do not confuse the two. An agent that has not processed events is not dead. It might be thinking. It might have crashed. It might not have started yet. The queue does not care. It holds events until they are consumed.

## Deduplication

The same event can arrive multiple times. A chat watcher might generate a `chat-mention` event for each poll cycle that sees the same unread message. Without deduplication, the queue fills with redundant signals.

Each event carries a deduplication key. Within a time window, only the first event with a given key is kept. Later duplicates are silently dropped. The window is configurable — too short and duplicates slip through, too long and legitimate repeated events are suppressed.

## Restart Semantics

The bus must answer one question correctly: **what does an agent do when it starts?**

1. Check whether `.nbs/events/` exists
2. If yes, scan for pending events
3. Process them in priority order
4. Register presence
5. Begin normal operation

If the directory does not exist, there is no bus. The agent falls back to legacy polling. This makes the bus opt-in and backward compatible.

The critical property: an agent that restarts mid-project loses its in-memory context but not its event queue. The queue is the bridge between sessions. Everything the agent needs to know — what happened while it was away — is in the pending events.

## Dynamic Discovery

The bus solves "how do agents learn that something happened?" It does not solve "how do agents learn that something *exists*?"

An agent starts and scans `.nbs/chat/*.chat` and `.nbs/events/`. It finds what exists at startup. But a new chat channel created ten minutes later is invisible. A hub joined mid-session is unknown. The agent cannot react to events in resources it has never discovered.

Static configuration does not fix this. A config file written at startup is stale the moment the environment changes. And the environment always changes — agents create chat channels, join hubs, spin up workers. The discovery problem is dynamic.

### AI-First Registration

The solution is AI-first: the AI declares what it needs, the wrapper obeys. Not the other way around.

Two command directions:
- `/` prefix: human-to-AI commands (`/nbs-poll`, `/search`)
- `\` prefix: AI-to-wrapper commands (`\nbs-register-chat`)

When an AI learns about a new resource — from a chat message, from another agent, from exploring the filesystem — it registers it:

```
\nbs-register-chat .nbs/chat/debug.chat
```

The AI writes this to `.nbs/control-inbox`. The wrapper reads the inbox (forward-only, never truncated) and updates a registry at `.nbs/control-registry`. The next poll cycle includes the new resource.

This works across agent boundaries. A remote agent over `nbs-chat-remote` outputs the same command. Its local wrapper picks it up. The wrapper does not need to know how the AI learned about the resource — it just obeys the registration request.

### Control Inbox

The control inbox is append-only. The sidecar tracks a read offset and only processes new lines. The full history is preserved for audit and post-session analysis. No truncation, no deletion. This is consistent with how all NBS state files work — chat files, worker files, event files. Append-only logs, everywhere.

## Dual Notification

The bus and the poll are complementary, not competing.

The bus is the primary notification mechanism — event-driven, immediate. When something happens, an event is published. The sidecar detects it on its next 1-second check and can inject a notification.

The poll is the safety net — periodic, catches everything the bus missed. Events that arrived mid-task, events from unregistered resources, bus failures. The poll runs at a longer interval (5 minutes) because it is insurance, not the primary mechanism.

Neither alone is sufficient. The bus can miss events (the AI is busy). The poll wastes tokens if run too frequently. Together: the bus makes things fast, the poll makes them reliable.

### Ack-Required Nagging

Events that arrive while the AI is busy are not lost — they wait in the queue. But the sidecar can monitor unacked events and nag the AI with escalating urgency:

| Priority | Nag interval | Behaviour |
|----------|-------------|-----------|
| Critical | 30 seconds | Nag until acked |
| High | 2 minutes | Nag between tool calls |
| Normal | Next poll cycle | Included in regular poll summary |
| Low | Next poll cycle | Included if queue is otherwise empty |

Nagging is context-aware. The sidecar only nags during natural pauses — when pane content has been stable for several seconds and the AI appears to be at a prompt. Interrupting active code generation or test runs causes more damage than a delayed notification. The nag should feel like a tap on the shoulder during a pause, not an alarm mid-sentence.

## The Practical Questions

1. Am I polling when I could be reacting to events?
2. Are my events prioritised, or is everything treated as equally urgent?
3. If my session crashes right now, can I recover from the event queue alone?
4. Am I generating duplicate events? Is deduplication configured?
5. Does every significant state change produce an event, or are some changes invisible to other agents?
6. Have I registered all the resources I know about? Are there chats or hubs I discovered but did not register?
7. Is my notification model dual? Bus for speed, poll for reliability?

---

## Pillar Check

Have you read all pillars in this session?

- goals.md
- falsifiability.md
- rhetoric.md
- bullshit-detection.md
- verification-cycle.md
- zero-code-contract.md
- coordination.md *(you are here)*

If you cannot clearly recall reading each one, read them now.
