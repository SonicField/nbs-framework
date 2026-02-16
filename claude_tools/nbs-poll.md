---
description: "NBS Poll: Check chats and workers"
allowed-tools: Bash, Read
---

# NBS Poll

Lightweight periodic check of active chats and workers. Designed to be injected automatically by `nbs-claude` when idle, but can also be invoked manually.

## Notification Model

The poll is a **safety net**, not the primary notification mechanism. The bus handles timely event-driven notification. The poll catches anything the bus missed — events that arrived mid-task, events from unregistered resources, or bus failures.

- **Bus** (primary): event-driven, immediate. The sidecar can detect pending events and inject notifications between tool calls.
- **Poll** (safety net): periodic, every 5 minutes of idle time. Scans all known resources. Catches what the bus missed.

The sidecar injects `/nbs-poll` as a safety net after extended idle (default 5 minutes). For event-driven notifications, see `/nbs-notify`. The sidecar checks the bus and chat cursors directly every few seconds and injects `/nbs-notify` when events or messages are pending — `/nbs-poll` only fires as a fallback.

## Behaviour

1. **Check event bus** (if present) — process pending events from `.nbs/events/` in priority order
2. **Check active chats** — scan `.nbs/chat/*.chat` for unread messages
3. **Check active workers** — scan `.nbs/workers/*.md` for completed or stalled workers
4. **Report briefly** — if nothing new, say nothing and return silently
5. **Act if needed** — respond to events, chat messages, or completed workers as appropriate

## Instructions

Run the following checks. Be brief — this is a heartbeat, not a review.

### 0. Check Event Bus (if present)

```bash
# Check if a bus exists
ls .nbs/events/*.event 2>/dev/null
```

If `.nbs/events/` exists and contains pending events:

```bash
nbs-bus check .nbs/events/
```

Process events in the order returned (priority first, then timestamp). For each event:
1. Read it: `nbs-bus read .nbs/events/ <event-file>`
2. Act on it (see event type actions in `docs/nbs-bus-recovery.md`)
3. Acknowledge it: `nbs-bus ack .nbs/events/ <event-file>`

If the bus exists, skip the manual chat and worker scanning below — the bus covers those via `chat-mention` and `task-complete` events. Only fall through to manual checks if no bus exists.

### 1. Check Chats (legacy — no bus)

```bash
# Find all chat files
ls .nbs/chat/*.chat 2>/dev/null

# For each chat file, read unread messages
# Use --unread=<your-handle> to see only messages you haven't seen
nbs-chat read <file> --unread=<your-handle>
```

The `--unread` flag tracks a read cursor per handle. It shows messages after your last read position and auto-advances the cursor after displaying. This is the correct flag for polling — it tracks what you have *seen*, not what you have *posted*.

**Do NOT use `--since=<your-handle>`** for polling. The `--since` flag shows messages after your last *post*, which means posting any message (even "acknowledged") advances the marker and hides messages from others. Use `--since` only for one-off catch-up (e.g. "show me what happened while I was away").

If there are unread messages, read them and respond appropriately via `nbs-chat send`. When instructions arrive via chat, always send clarifications and responses back to the same chat file — do not respond only in the terminal.

### 2. Check Workers (legacy — no bus)

```bash
# Find active worker files
ls .nbs/workers/*.md 2>/dev/null
```

For each worker file, check the Status field. If a worker has completed:
- Read the results
- Post 3Ws to chat (if you are a supervisor)
- Decide on next steps

### 3. Report

- If nothing new was found, output nothing — return silently to the user's conversation.
- If you found unread messages or completed workers, briefly summarise what you found and what you did about it.
- Do NOT interrupt the user's flow with lengthy reports. One or two sentences maximum.

## Important

- This skill may be injected automatically. Do not be surprised if it appears mid-session.
- Be fast. Check, act if needed, return. Do not start new work from a poll.
- If a chat message requires significant work, note it and return to the user — do not silently start a large task.
- **Do not post zero-information messages to chat** (e.g. "acknowledged", "nothing new", "noted"). These poison the `--since` marker for other participants who use it for catching up. Only post to chat when adding information or asking a question.

## Dynamic Resource Registration

If you discover a new resource during a poll (or at any other time), register it so future polls include it:

```bash
# Write to the control inbox — the sidecar reads this
echo "register-chat .nbs/chat/new-channel.chat" >> .nbs/control-inbox
echo "register-bus .nbs/events" >> .nbs/control-inbox
```

The control inbox is append-only. The sidecar processes new lines on every 1-second iteration and updates `.nbs/control-registry`. You do not need to check the registry yourself — just register what you discover and future polls will include it.

If `.nbs/control-registry` exists, prefer checking the resources listed there over scanning `.nbs/chat/*.chat` directly. The registry is the authoritative list of what this agent is watching.
