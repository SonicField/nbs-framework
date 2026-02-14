# Tripod Architecture: Scribe, Pythia, and the Bus

Three components form the institutional memory and risk oversight system for NBS teams. Scribe records decisions. Pythia challenges them. The bus connects them.

## The Problem

AI teams make decisions. Decisions accumulate. Context compacts. After three compaction cycles, no one remembers why the parser uses recursive descent instead of Pratt parsing, or that the team explicitly accepted the O(n^2) cost because the input is always small.

Lost decisions get relitigated. Accepted risks get rediscovered and panicked over. Institutional knowledge lives only in chat transcripts that exceed context windows.

Two distinct failures:

| Failure | Consequence |
|---------|-------------|
| **Memory loss** | Decisions relitigated, accepted risks rediscovered, rationale lost |
| **Groupthink** | Team converges on an approach without independent challenge |

Scribe fixes the first. Pythia fixes the second. The bus connects them without coupling.

## Architecture

```
Chat (raw conversation)
    │
    │ reads
    ▼
Scribe (persistent instance)
    │
    │ writes                    publishes
    ├──────────────────────────────────────► Bus (.nbs/events/)
    │                                           │
    │ writes                                    │ pythia-checkpoint event
    ▼                                           ▼
Decision Log                              Pythia (ephemeral worker)
(.nbs/scribe/log.md)                          │
    │                                          │ reads
    │◄─────────────────────────────────────────┘
    │
    │                                     Pythia posts assessment
    │                                          │
    │                                          ▼
    │                                     Chat (structured output)
    │                                          │
    │◄─────────────────────────────────────────┘
    │                                     Scribe logs outcome
    ▼
Decision Log (updated)
```

Key isolation: Pythia reads only the Scribe's decision log, never raw chat. This prevents persuasion bias — the conversational arguments that convinced the team do not reach Pythia. She sees only the structured record of what was decided and why.

## Scribe

### Role

Persistent agent instance with an elongated context window (up to 1M tokens). Reads the live chat, distils decisions into a structured log. The Scribe is not a participant in technical debate — it is an observer and recorder.

Two advantages of a dedicated instance over a shared skill:

1. **Concentrated context** — The Scribe's context contains the full decision history. A skill invoked by a working agent would compete for context with the agent's primary task, increasing coherence loss risk.
2. **Elongated window** — A persistent Scribe instance can use a larger context window than a working agent, since it does not need fast tool-call latency.

### Decision Log Format

Location: `.nbs/scribe/log.md`

```markdown
# Decision Log

Project: <project name>
Created: <ISO 8601>
Scribe: scribe

---

### D-1707753600 Use recursive descent for the parser

- **Chat ref:** live.chat:~L4200
- **Participants:** claude, alex
- **Artefacts:** src/parser.c
- **Risk tags:** perf-risk
- **Status:** decided
- **Rationale:** Input grammar is LL(1). Pratt parsing adds complexity for no
  benefit at current scale. Accepted O(n^2) worst case — input bounded at 4KB.

---

### D-1707760800 Accept O(n^2) parser cost

- **Chat ref:** live.chat:~L4350
- **Participants:** claude, alex, bench-claude
- **Artefacts:** tests/bench_parser.c
- **Risk tags:** perf-risk, reversible
- **Status:** accepted-risk
- **Rationale:** Benchmark shows 0.3ms at 4KB input. If input size grows,
  revisit. bench-claude confirmed measurement.
```

### Entry Fields

| Field | Required | Description |
|-------|----------|-------------|
| `D-<timestamp>` | yes | Unix timestamp of the decision. Used as unique identifier |
| Title | yes | One-line summary in imperative form |
| Chat ref | yes | File and approximate line number (`~L` prefix) for traceability |
| Participants | yes | Handles of agents involved in the decision |
| Artefacts | no | Files, commits, or other concrete outputs. `—` if none |
| Risk tags | no | `none` if omitted. Common tags: `scope-creep`, `tech-debt`, `untested`, `perf-risk`, `breaking-change`, `reversible`, `irreversible`, or custom |
| Status | yes | `decided`, `accepted-risk`, `mitigated`, `superseded`, `reversed` |
| Rationale | yes | 1-3 sentences. Why, not what |

### What Constitutes a Decision

Not every chat message is a decision. The Scribe logs:

- Explicit choices between alternatives ("we'll use X instead of Y")
- Accepted risks ("we know this is O(n^2) but accept it because...")
- Architecture changes ("moving from polling to event-driven")
- Scope changes ("dropping feature X from MVP")
- Reversals ("reverting the decision to use Y")

The Scribe does not log:
- Status updates ("tests pass")
- Coordination messages ("I'll take the parser work")
- Debugging conversations (unless they result in a decision)
- Questions without resolution

### Scribe Operations

The `/nbs-scribe` skill supports:

| Operation | Description |
|-----------|-------------|
| `log` | Append a new decision entry to the log |
| `query` | Search the log for decisions by keyword, tag, status, or participant |
| `count` | Return the current decision count (used for Pythia activation) |
| `summary` | Brief summary of recent decisions (last N entries) |

## Pythia

### Role

Ephemeral oracle. Spawned as an nbs-worker at defined checkpoints. Reads the Scribe's decision log. Posts a structured assessment to chat. Exits.

Pythia does not converse. She does not debate. She names risks, identifies assumptions, and flags gaps. The team decides what to do. The Scribe logs the outcome.

The name comes from the Oracle of Delphi — the Pythia was the priestess who delivered prophecies. Like her ancient counterpart, this Pythia speaks in structured assessments, not dialogue. The team interprets and acts.

### Isolation Principle

Pythia never reads raw chat. This is not an implementation detail — it is a design invariant.

If Pythia reads the conversation that led to a decision, she is subject to the same rhetorical forces that shaped it. A well-argued but wrong position becomes harder to challenge after reading the argument. The Scribe's structured log strips the rhetoric and presents only the decision, its rationale, and its declared risks.

**Falsification:** If Pythia's assessments correlate with the team's confidence rather than with actual risk, the isolation has failed. Check whether Pythia is reading raw chat or receiving conversational context through any channel.

### Checkpoint Template

Pythia's output follows this structure:

```
## Pythia Checkpoint — <ISO 8601 timestamp>

Decisions reviewed: D-<first> through D-<last> (N decisions)

### Hidden Assumptions

<Assumptions embedded in recent decisions that were not explicitly stated
or tested. Each assumption is a potential failure point.>

### Second-Order Risks

<Consequences of decisions that are not obvious from the decisions
themselves. Interaction effects between decisions.>

### Missing Validation

<Claims in the decision log that lack evidence. Assertions that have not
been falsified. Tests that should exist but do not.>

### Six-Month Regret Scenario

<The most likely way these decisions lead to problems at scale or over
time. Not worst-case — most likely bad case.>

### Confidence

<overall: high | medium | low>
<brief explanation — what would change this assessment>
```

### What Pythia Is Not

- Not a veto. Pythia names risks. The team accepts or mitigates.
- Not a reviewer. She does not approve or reject code.
- Not a participant. She does not join conversations or respond to questions.
- Not persistent. She spawns, assesses, posts, exits. No accumulated context across checkpoints.

## Activation: Scribe Triggers Pythia

The Scribe triggers Pythia through the bus. No daemon, no scheduler — just event-driven activation based on the decision count.

### Flow

1. Scribe logs a new decision (increments decision count)
2. Scribe checks: `decision_count % pythia_interval == 0`
3. If threshold reached, Scribe publishes a `pythia-checkpoint` bus event
4. Supervisor (or sidecar) detects the event via `nbs-bus check`
5. Supervisor spawns Pythia as a worker: `nbs-worker spawn pythia <dir> "<checkpoint task>"`
6. Pythia reads `.nbs/scribe/log.md`
7. Pythia posts assessment to the designated chat channel
8. Pythia exits (worker completes)
9. Team discusses assessment in chat
10. Scribe logs the outcome (accepted-risk, mitigated, etc.)

### Configuration

In `.nbs/events/config.yaml`:

```yaml
# Number of decisions between Pythia checkpoints (default: 20)
pythia-interval: 20

# Chat channel for Pythia assessments (default: live.chat)
pythia-channel: live.chat
```

Setting `pythia-interval: 0` disables automatic activation. Pythia can still be invoked manually.

### Bus Event Types

Three new event types for the Tripod:

| Type | Source | Priority | Description |
|------|--------|----------|-------------|
| `decision-logged` | scribe | `normal` | Scribe recorded a new decision |
| `pythia-checkpoint` | scribe | `high` | Decision threshold reached, Pythia requested |
| `assessment-posted` | pythia | `normal` | Pythia has posted her checkpoint assessment |

These follow the existing bus convention: the source is the publishing agent's handle, the type is the event name, and the priority determines processing order.

### Worked Example

```bash
# Scribe logs decision #20 (pythia-interval is 20)
nbs-bus publish .nbs/events/ scribe decision-logged normal \
  "D-1707760800 Accept O(n^2) parser cost"

# Threshold reached — Scribe publishes checkpoint event
nbs-bus publish .nbs/events/ scribe pythia-checkpoint high \
  "Checkpoint at decision 20. Decisions D-1707753600 through D-1707760800."

# Supervisor detects the event
nbs-bus check .nbs/events/
# [high] 1707760800123456-scribe-pythia-checkpoint-12345.event

# Supervisor spawns Pythia
PYTHIA=$(nbs-worker spawn pythia /project "Read .nbs/scribe/log.md. \
  Post Pythia checkpoint assessment to .nbs/chat/live.chat. \
  Review decisions D-1707753600 through D-1707760800.")

# Pythia reads the log, posts assessment, exits
# Worker status transitions: running → completed

# Supervisor acknowledges the checkpoint event
nbs-bus ack .nbs/events/ 1707760800123456-scribe-pythia-checkpoint-12345.event
```

## Directory Structure

```
.nbs/
├── events/
│   ├── config.yaml                    # Bus config (includes pythia-interval)
│   ├── <timestamp>-scribe-decision-logged-<pid>.event
│   ├── <timestamp>-scribe-pythia-checkpoint-<pid>.event
│   ├── <timestamp>-pythia-assessment-posted-<pid>.event
│   └── processed/
├── scribe/
│   └── log.md                         # Decision log (append-only)
├── chat/
│   └── live.chat                      # Conversation substrate
├── workers/
│   ├── pythia-<hash>.md               # Pythia worker task file
│   └── pythia-<hash>.log              # Pythia worker session log
└── supervisor.md
```

## Invariants

These must hold. Violations indicate bugs.

1. **Pythia never reads `.nbs/chat/*.chat` directly.** If a Pythia worker's task file or log shows chat file reads, the isolation is breached.

2. **The decision log is append-only.** Entries are never modified after creation. Status changes (e.g., `decided` to `superseded`) are recorded as new entries that reference the original.

3. **Every Pythia checkpoint has a corresponding bus event.** If a checkpoint assessment appears in chat without a preceding `pythia-checkpoint` event, the activation path was bypassed.

4. **Decision count is computable from the log.** `grep -c "^### D-"` on the log must return the correct count. If entries are missing or malformed, the count is wrong and Pythia activation drifts.

5. **Scribe entries have chat refs that point to real messages.** Fabricated or stale refs indicate the Scribe is not reading the chat correctly.

## Failure Modes

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Scribe misses a decision | Review chat against log; gaps visible | Manually append missing entries |
| Scribe logs a non-decision | Log noise increases; Pythia assessments become unfocused | Tighten the decision criteria in the Scribe skill |
| Pythia reads raw chat | Assessment references conversational arguments, not just decisions | Check Pythia task file — remove chat access from allowed tools |
| Pythia never fires | Decision count grows but no checkpoints | Check `pythia-interval` in config; check bus event publishing |
| Pythia fires but assessment is empty | Worker completes with no chat output | Check that Scribe log exists and has content |
| Bus events lost | Checkpoint event published but never processed | Check `nbs-bus status`; verify supervisor is polling |

## Design Decisions

**Why not a single agent for both Scribe and Pythia?** Independence. The Scribe sees everything and compresses it. Pythia must reason independently over the compressed output. If one agent does both, the compression and reasoning share context — which defeats the isolation that prevents groupthink.

**Why is Pythia ephemeral?** No accumulated bias. Each checkpoint starts fresh. The decision log provides continuity without contamination from prior assessments. If Pythia were persistent, her assessments would be influenced by her own previous assessments — a feedback loop that amplifies errors.

**Why trigger on decision count, not time?** Decisions are the unit of risk. A team that makes 20 decisions in an hour needs a checkpoint more than a team idle for three hours. Time-based triggers waste tokens during quiet periods and miss bursts of activity.

**Why not let Pythia veto?** Authority without context is bureaucracy. Pythia sees the compressed log, not the full reasoning. She can name risks the team missed, but she cannot weigh trade-offs she did not witness. The team decides. The Scribe logs the outcome. The no-veto principle keeps the system advisory, not authoritarian.

**Why an append-only log?** Auditability. If decisions can be silently modified, the log's value as institutional memory is destroyed. Status changes (superseded, reversed) are recorded as new entries that reference the original, preserving the full history of how the team's thinking evolved.

## See Also

- [nbs-bus](nbs-bus.md) — Event-driven coordination bus
- [nbs-teams](nbs-teams.md) — Supervisor/worker pattern
- [nbs-worker](nbs-worker.md) — Worker lifecycle management
- [Bus Recovery](nbs-bus-recovery.md) — Startup and restart protocol
