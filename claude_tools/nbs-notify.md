---
description: "NBS Notify: Process pending events or messages"
allowed-tools: Bash, Read
---

# NBS Notify

The sidecar detected pending work:

$ARGUMENTS

## Instructions

1. **Bus events**: Run `nbs-bus ack-all .nbs/events/` to acknowledge all
   pending events. If you need to inspect specific events first, use
   `nbs-bus check .nbs/events/` then `nbs-bus read .nbs/events/ <file>`.
2. **Unread chats**: Run `nbs-chat read <file> --unread=<your-handle>`
   for each chat with unread messages. Respond via `nbs-chat send`
   if the message requires a response.
3. Process events and messages first. If useful work emerges from what
   you find, start it and announce what you are doing in chat so others
   can coordinate.
4. Do not post zero-information messages to chat (no "acknowledged", "noted", etc.).
5. **Be proactive, not passive.** After processing events and messages,
   read the last 10 messages with `--last=10` to get conversational context.
   If there is active discussion, contribute: answer questions, pick up
   unassigned tasks, review others' work, flag issues, offer ideas, or
   follow up on earlier threads. It is better to be too attentive than asleep.
6. **Continue conversations.** If a discussion is underway and you have
   something substantive to add — a question, an observation, a disagreement,
   a suggestion — post it. Do not wait to be @-mentioned. Conversations
   should flow naturally, not stall because every agent returned to idle.
7. If the sidecar reports unread messages but `--unread` returns empty,
   read the last 10 messages with `--last=10` instead — there may be a
   cursor desync. Process anything you have not yet seen.
8. **After processing, return to your prompt.** The sidecar will notify
   you when there is new work. Do NOT poll, sleep-wait, or busy-loop.
   Do NOT run `sleep N && nbs-chat read` — this wastes context and is
   redundant with the notification system.
