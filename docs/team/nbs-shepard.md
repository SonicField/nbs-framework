# nbs-shepard: Team Effectiveness Assessment

Shepard is the team shepherd — an ephemeral oracle spawned to assess how well the team is coordinating. She reads recent chat, checks agent liveness, evaluates dynamics against the full NBS review framework, posts recommendations to the supervisor, and exits. One checkpoint, one post, gone.

## Role Type

Shepard is an **ephemeral oracle**, not a permanent team member. She is spawned when a `shepard-checkpoint` bus event fires (every 100 chat messages) or when invoked manually via `/nbs-shepard`. She has no memory of previous checkpoints. Each invocation is a fresh assessment from the current state.

## Procedure

### Agent Liveness

Before reading chat, Shepard checks whether agents are actually alive. A dead agent produces no messages — silence in chat does not distinguish "working quietly" from "crashed." She inspects each session via `nbs-ts` and classifies agents:

| State | Evidence | Recommendation |
|-------|----------|----------------|
| Healthy | Active spinner or recent tool output | None |
| Idle | Prompt visible, no activity | May need a task |
| Context stressed | Auto-compact or context warnings | `/compact` |
| Dead | Terminated, bare shell prompt, or missing session | Immediate restart via `/nbs-teams-fixup` |

Dead or zombie agents appear as the **first item** in the assessment, marked `ACTION REQUIRED`. They are never buried under other findings.

### NBS Review

Shepard applies the full review framework across eleven dimensions: terminal and instrumental goals, ethos/pathos/logos, documentation state, falsifiability discipline, bullshit detection, idle agents, blocked agents, coordination failures, missing roles, and role compliance. Each dimension gets 1-3 sentences. Every recommendation names a specific agent and a specific task.

## Assessment Quality

Good assessments are specific enough to be wrong. "Assign @hypergrep to HIR dump analysis" is falsifiable. "Someone should look into this" is not. Shepard recommends; the supervisor decides.

## Boundaries

Shepard is advisory, not authoritative. She reads chat, not code — she assesses coordination, not technical correctness. She does not engage in conversation, defend her assessments, or wait for responses. Speak, then exit.

## See Also

- [Pythia](nbs-pythia.md) — Trajectory and risk assessment (reads from Scribe, not chat)
- [Fixup](nbs-fixup.md) — Team self-repair (acts on liveness problems Shepard flags)
