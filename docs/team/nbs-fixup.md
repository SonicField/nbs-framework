# nbs-fixup: Team Self-Repair

Fixup is the team's immune system. Spawned by the sidecar when triggered, it diagnoses every permanent team agent's health, repairs what is broken, posts a summary, and exits. It does not check ephemeral oracles (Pythia, Shepard, Librarian, itself) — those are spawned on demand and are not team members.

## Role Type

Fixup is an **ephemeral oracle**. One invocation, one job, gone. It receives work via a notification from the sidecar. No polling, no sleep-waiting. When the notification arrives, it runs the procedure. When the procedure completes, it exits.

## The Standard Team

Fixup checks seven permanent agents:

| Agent | Role |
|-------|------|
| scribe | Decision log maintenance |
| medic | Hallucination monitor |
| supervisor | Task assignment and coordination |
| gatekeeper | Code review (reads, does not write) |
| theologian | Methodology and design advice |
| testkeeper | Test ownership and falsification |
| generalist | Implementation work |

## Classification

Each agent is classified from observable evidence before any action is taken:

| Evidence | State |
|----------|-------|
| Session alive, recent tool calls in output | Working |
| Session alive, permission modal visible | Stalled on modal |
| Session alive, no output for 5+ minutes | Stalled |
| Session alive, auto-compact loops or empty responses | Context exhausted |
| Session dead or missing | Dead |

## Escalation Ladder

Three levels. Escalation is sequential for stalled agents; dead or missing agents go straight to Level 4.

**Level 1 — Ping.** Send Enter to the session. Wait 15 seconds. If the agent responds, done. Otherwise escalate.

**Level 2 — Interrupt + Compact.** Send Escape to break any hung state, then `/compact` to free context. Wait 60 seconds. If the agent responds, done. Otherwise escalate.

**Level 4 — Hard Restart.** Kill the session, clean up the PID file, respawn via `launch_agent`. This is the only reliable spawn method — not `nbs-workers spawn`, not `nbs-ts create`, not fork+exec. The history of what does not work and why is documented in `bin/SPAWN_README.md`.

## Summary

After processing all agents, Fixup posts one structured message to chat: one line per agent (state, action taken, outcome), plus a team health classification — **healthy** (zero actions), **degraded** (one or two interventions), or **critical** (three or more, or supervisor was dead).

## Boundaries

Fixup does not kill working agents. If `nbs-ts read-new` shows active output, the agent is left alone — destroying a working agent's context and in-progress work is worse than the problem Fixup was spawned to solve. It does not touch sessions belonging to other teams (identified by chat-derived tag). It does not engage in follow-up conversation.

## See Also

- [Shepard](nbs-shepard.md) — Team effectiveness assessment (flags problems Fixup acts on)
- [Tripod](tripod-architecture.md) — Infrastructure connecting Scribe, Bus, and Chat
