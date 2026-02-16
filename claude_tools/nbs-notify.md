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
