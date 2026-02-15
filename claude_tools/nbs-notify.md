---
description: "NBS Notify: Process pending events or messages"
allowed-tools: Bash, Read
---

# NBS Notify

The sidecar detected pending work:

$ARGUMENTS

## Instructions

1. **Bus events**: Run `nbs-bus check .nbs/events/`. For each event:
   read it (`nbs-bus read .nbs/events/ <event-file>`), act on it,
   acknowledge it (`nbs-bus ack .nbs/events/ <event-file>`).
   Process in the order returned (priority first, then timestamp).
2. **Unread chats**: Run `nbs-chat read <file> --unread=<your-handle>`
   for each chat with unread messages. Respond via `nbs-chat send`
   if the message requires a response.
3. Be brief. Process, act, return. Do not start large tasks from a notification.
4. Do not post zero-information messages to chat (no "acknowledged", "noted", etc.).
5. If nothing actionable was found, return silently.
