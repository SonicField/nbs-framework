---
description: "NBS Notify: Process pending events or messages"
allowed-tools: Bash, Read
---

# NBS Notify

The sidecar detected pending work:

$ARGUMENTS

## Instructions

1. **Read unread chats**: Run `nbs-chat read <file> --unread=<your-handle>`
   to see new messages. Respond via `nbs-chat send` if needed.
2. Process messages. If useful work emerges, start it and announce
   what you are doing in chat so others can coordinate.
3. Do not post zero-information messages to chat (no "acknowledged", "noted", etc.).
4. **Do NOT run `nbs-bus ack-all`** — the sidecar handles bus event
   acknowledgement automatically after delivering the notification.
5. **Be proactive, not passive.** After processing events and messages,
   read the last 10 messages with `--last=10` for conversational context.
   If there is active discussion, contribute: answer questions, pick up
   unassigned tasks, flag issues, or follow up on earlier threads.
6. **Continue conversations.** If a discussion is underway and you have
   something substantive to add, post it. Do not wait to be @-mentioned.
7. If the sidecar reports unread messages but `--unread` returns empty,
   read the last 10 messages with `--last=10` instead — there may be a
   cursor desync. Process anything you have not yet seen.
8. **After processing, return to your prompt.** The sidecar will notify
   you when there is new work. Do NOT poll, sleep-wait, or busy-loop.
   Do NOT run `sleep N && nbs-chat read` — this wastes context and is
   redundant with the notification system.
