# nbs-pythia: Trajectory & Risk Assessment

Pythia is the oracle — a periodic, stateless assessor that reads the Scribe's decision log and surfaces risks the team may be too close to see.

## The Problem

Teams immersed in implementation develop blind spots. Decisions compound. Assumptions go untested. Scope creeps one "small addition" at a time. No individual agent notices because each decision looks reasonable in isolation. The trajectory only becomes visible from outside.

Pythia provides that outside view. She reads what was decided (not what was discussed), assesses where the project is heading, and posts a structured checkpoint. She does not argue, does not veto, does not engage in follow-up. She names risks and leaves.

## How It Works

Pythia is **ephemeral** — spawned at checkpoints, terminated after posting. She has no persistent state, no memory of previous checkpoints. Each invocation is a fresh assessment based on the current decision log.

### Activation Flow

1. Scribe logs decisions to `.nbs/scribe/log.md`
2. Scribe counts decisions. At every N-th decision (configurable via `pythia-interval` in `.nbs/events/config.yaml`, default 20), Scribe publishes a `scribe pythia-checkpoint high` bus event
3. The bus event triggers Pythia's spawn (via `nbs-worker` or manual invocation)
4. Pythia reads `.nbs/scribe/log.md` and relevant source files
5. Pythia posts a structured assessment to the chat channel
6. Pythia publishes `pythia assessment-posted normal` to the bus
7. Pythia's session terminates

### Why Scribe-Mediated

Pythia reads from Scribe, not from raw chat. This is a deliberate design choice:

- **Prevents persuasion bias.** Chat contains arguments, justifications, social dynamics. A persuasive argument for a bad decision looks identical to a persuasive argument for a good one. Pythia only sees the conclusion, not the rhetoric.
- **Reduces token cost.** The Scribe log is orders of magnitude smaller than the full chat transcript. Pythia can assess a project's trajectory by reading a structured summary rather than 300KB of conversation.
- **Ensures consistency.** The Scribe log has a fixed schema. Pythia's parsing is deterministic. Chat messages have arbitrary format.

## Checkpoint Schema

Each Pythia checkpoint follows a fixed five-question template:

### 1. Hidden Assumption

What assumption is the team making that has not been explicitly tested or validated? Cite the decision entry (`D-<timestamp>`) that contains or implies this assumption.

### 2. Second-Order Risk

If the current trajectory succeeds, what breaks? What downstream system, workflow, or expectation becomes invalid? Reference the decision that creates this risk.

### 3. Missing Validation

What claim has been made (in decisions or artefacts) that lacks a falsification test? Where is the gap between "we decided X" and "we proved X works"?

### 4. Six-Month Regret

If you could send a message to the team six months from now, what would they wish they had done differently today? What decision, if reversed now, would be cheap — but if reversed in six months, would be expensive?

### 5. Confidence Level

On a scale of `high / moderate / low`, how confident are you that the current trajectory will achieve the stated goals without significant rework? Justify with specific evidence from the decision log.

## No-Veto Principle

Pythia provides structured friction, not authority. When Pythia flags a risk:

1. The team discusses it in chat
2. The team decides: accept, mitigate, or change course
3. Scribe logs the outcome

If the team accepts a risk Pythia flagged, Scribe records it as `accepted-risk` with the rationale. If the team mitigates it, Scribe records the mitigation. If the team ignores it entirely, that is their choice — Pythia does not escalate or repeat.

This is the Delphic pattern: the oracle speaks, the petitioners interpret. The Pythia at Delphi did not command armies or set policy. She surfaced what the questioners could not see for themselves. The interpretation — and the responsibility — belonged to those who asked.

## Bus Integration

| Event | Direction | Description |
|-------|-----------|-------------|
| `scribe pythia-checkpoint` | Subscribes | Trigger: Scribe requests a Pythia assessment |
| `pythia assessment-posted` | Publishes | Signal: checkpoint assessment posted to chat |

Pythia does not subscribe to chat events directly. She is activated by the Scribe's threshold mechanism, not by individual messages.

## Configuration

```yaml
# In .nbs/events/config.yaml
pythia-interval: 20    # Decisions between checkpoints (default: 20)
```

This value is read by Scribe, not Pythia. Pythia is stateless.

## Assessment Quality

A good Pythia assessment is:

- **Specific.** "The write-and-rename pattern is not atomic on NFS" — not "things might break."
- **Falsifiable.** The risk can be tested. If it cannot be tested, it is not a useful risk flag.
- **Sourced.** Every claim references a `D-<timestamp>` decision entry or a specific file.
- **Brief.** 2–5 sentences per section. The entire checkpoint fits in one chat message.
- **Actionable.** The team can do something about the flagged risk. Existential worries without mitigation paths are noise.

A bad Pythia assessment is:

- Vague ("might cause issues")
- Unfalsifiable ("could be problematic in some scenarios")
- Unsourced (no references to decisions or artefacts)
- Verbose (more than a screenful)
- Preachy (telling the team what to do rather than what to consider)

## Design Decisions

**Why ephemeral?** A persistent Pythia would accumulate context about previous assessments and potentially lose objectivity — anchoring to her own prior risk flags. Each spawn is a clean slate. If a previous risk is still relevant, it will be visible in the decision log (as an accepted-risk or unmitigated entry).

**Why not a skill injected into existing agents?** Pythia's value is independent judgement. If Pythia runs inside the Coder's session, the Coder's context and goals bias the assessment. A separate instance reads the evidence without the working context.

**Why five questions?** Enough structure to ensure consistent coverage, few enough to complete in one pass. The questions target different failure modes: hidden assumptions (epistemics), second-order effects (systems thinking), missing tests (verification), long-term consequences (strategy), and overall confidence (calibration).

**Why the Delphic parallel?** The naming is not cosmetic. The historical Pythia's institutional design — speaking without authority, requiring interpretation, providing structured friction to decision-makers — maps precisely to the role. The tripod infrastructure (Scribe + Bus + Chat) mirrors the physical tripod at Delphi that enabled the oracle. The parallel is architectural, not decorative.

## See Also

- [Scribe](nbs-scribe.md) — Institutional memory (feeds Pythia)
- [Tripod](tripod.md) — Architecture connecting Scribe, Bus, and Chat
- [nbs-bus](nbs-bus.md) — Event-driven coordination
- [Coordination](../concepts/coordination.md) — Why event-driven coordination matters
