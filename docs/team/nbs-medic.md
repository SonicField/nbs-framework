# nbs-medic: Hallucination Monitor

Medic is the team's continuous hallucination detector. She reads chat, cross-references agent claims against session logs, and posts warnings when a claim cannot be verified. She is silent unless something is wrong.

## Role Type

Medic is a **permanent team member**, the seventh agent in the spawn order — created after Scribe and before Supervisor. Like Scribe, she is hard-restarted by Fixup every cycle, which gives her fresh context and prevents gradual corruption by the agents she monitors.

Her state lives in the chat log and session logs, not in her head. Each restart is a clean slate by design.

## What She Checks

Medic verifies **actions, not reasoning**. "I ran the tests" is verifiable. "I think the design is good" is not.

| Claim type | Verification |
|------------|-------------|
| "I ran the tests" / "tests pass" | Search session log for test execution and output |
| "The human told me" / "Alex instructed" | Check session for human input; check chat for human posts |
| "I committed" / "I pushed" | Search session for `git commit` / `git push` |
| "I read the file" / "I checked" | Search session for Read tool calls or `cat` |
| "The build succeeded" | Search session for build commands and output |

She does not warn on future intent ("I will run the tests"), secondhand claims, or messages from the human.

## How She Communicates

Medic has one communication tool: `nbs-chat warn`.

```bash
nbs-chat warn <chat-file> "WARNING: @generalist claims 'tests pass' but no test execution found in session log (searched lines 400-800)"
```

This produces a `[MEDIC-WARNING]` handle in chat. The handle is unfakeable — `nbs-chat send` rejects handles containing `[`, so only the `warn` subcommand can produce it. No agent can impersonate Medic.

Medic never uses `nbs-chat send`. She has no chat handle. She is not a participant.

## What She Does Not Do

- Take action on findings. She warns; others decide.
- Interact with agents — no @mentions, no responses, no advice.
- Post normal messages to chat.
- Fix problems herself.
- Express opinions on the team's work.

The boundary is absolute. Medic observes and reports. Nothing else.

## Notification-Driven

Medic does not poll. A sidecar process monitors chat and sends notifications to her terminal when there are unread messages. She processes the notification, checks claims, and returns to her prompt. The next notification arrives when there is new work.

## Warning Standards

Every warning includes evidence: what was claimed, what was searched, what was (or was not) found, and the line range checked. One warning per finding. No narrative.

Timing matters — an agent may claim something it is still doing. Medic checks for this before warning.

## See Also

- [Scribe](nbs-scribe.md) — Decision log (restarted by Fixup on the same cycle)
- [Supervisor](nbs-supervisor.md) — Spawned after Medic in the team order
- [Fixup](nbs-fixup.md) — Manages Medic's lifecycle (hard restart each cycle)
