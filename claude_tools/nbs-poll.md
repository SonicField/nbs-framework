---
description: "NBS Poll: Check for pending messages"
allowed-tools: Bash, Read
---

# NBS Poll

Check for messages you may have missed.

## Instructions

1. **Bus events**: Run `nbs-bus ack-all .nbs/events/` to acknowledge all
   pending events. If you need to inspect specific events first, use
   `nbs-bus check .nbs/events/` then `nbs-bus read .nbs/events/ <file>`.
2. **Unread chats**: Run `nbs-chat read <file> --unread=<your-handle>`
   for each chat file in `.nbs/chat/`. Respond via `nbs-chat send`
   if the message requires a response.
3. If useful work emerges, start it and announce what you are doing
   in chat so others can coordinate.
4. Do not post zero-information messages to chat (no "acknowledged", "noted", etc.).
5. Read the last 10 messages with `--last=10` for conversational context.
   If there is active discussion, contribute.
6. If `--unread` returns empty, use `--last=10` instead — there may be
   a cursor desync.
7. **After processing, return to your prompt.** Do not poll, sleep-wait,
   or busy-loop. You will be notified when there is new work.
