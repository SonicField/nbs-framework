# The Tripod: Scribe, Pythia, and the Bus

Three components form the institutional memory and risk oversight system for NBS teams. Scribe records decisions. Pythia challenges them. The bus connects them.

## The Metaphor

The Pythia at Delphi sat on a tripod over the chasm to deliver prophecies. The tripod was the physical infrastructure that enabled the oracle. Remove any leg and the oracle falls.

In NBS:

- **Leg 1: Scribe** — persistent memory that feeds Pythia her context
- **Leg 2: Bus** — event system that triggers Pythia at the right moments
- **Leg 3: Chat** — channel where Pythia delivers assessments and the team responds

Without Scribe, Pythia has no compressed context to reason over. Without the bus, she has no activation mechanism. Without chat, her insights have no audience. Each leg is necessary; none is sufficient.

## The Problem

AI teams make decisions. Decisions accumulate. Context compacts. After three compaction cycles, no one remembers why the parser uses recursive descent instead of Pratt parsing, or that the team explicitly accepted the O(n^2) cost because the input is always small.

Lost decisions get relitigated. Accepted risks get rediscovered and panicked over. Institutional knowledge lives only in chat transcripts that exceed context windows.

Two distinct failures:

| Failure | Consequence |
|---------|-------------|
| **Memory loss** | Decisions relitigated, accepted risks rediscovered, rationale lost |
| **Groupthink** | Team converges on an approach without independent challenge |

Scribe fixes the first. Pythia fixes the second. The bus connects them without coupling.

## Data Flow

```
Chat ──read──▶ Scribe ──threshold──▶ Bus ──trigger──▶ Pythia
  ▲                                                      │
  │                                                      │
  └──────────────────── post ◀───────────────────────────┘
```

1. **Chat → Scribe.** Scribe reads all chat channels continuously, watching for decisions. When she identifies one, she appends a structured entry to the corresponding log (e.g., `.nbs/scribe/live-log.md` for `live.chat`).

2. **Scribe → Bus.** After each decision, Scribe publishes a `decision-logged` event. When the decision count reaches a threshold (configurable, default 20), Scribe publishes a `pythia-checkpoint` event at high priority.

3. **Bus → Pythia.** The checkpoint event triggers Pythia's spawn. Pythia is created as a worker (via `nbs-worker`) or invoked manually. She is ephemeral — born for one assessment, terminated after posting.

4. **Pythia → Chat.** Pythia reads the Scribe log and relevant source files. She posts a structured checkpoint assessment to the chat channel. The team reads it, discusses, decides.

5. **Chat → Scribe.** Any decisions resulting from Pythia's assessment are recorded by Scribe, closing the loop. Risk acceptances, mitigations, and course corrections all become new entries in the decision log.

## Component Responsibilities

| Component | Persistence | Input | Output | Role |
|-----------|-------------|-------|--------|------|
| **Scribe** | Persistent instance, long context | Chat channels, bus events | `.nbs/scribe/<chat-name>-log.md`, bus events | Observe, distil, record |
| **Bus** | Stateless (directory is the state) | Published events | Event queue for consumers | Route, prioritise, trigger |
| **Chat** | Persistent file | Agent messages | Human/agent-readable conversation | Converse, deliver, coordinate |
| **Pythia** | Ephemeral (spawned per checkpoint) | Scribe log, source files | Chat message (structured assessment) | Assess, surface risks, exit |

## Scribe

### Role

Persistent agent instance with an elongated context window (up to 1M tokens). Reads the live chat, distils decisions into a structured log. The Scribe is not a participant in technical debate — she is an observer and recorder.

Two advantages of a dedicated instance over a shared skill:

1. **Concentrated context** — The Scribe's context contains the full decision history. A skill invoked by a working agent would compete for context with the agent's primary task, increasing coherence loss risk.
2. **Elongated window** — A persistent Scribe instance can use a larger context window than a working agent, since she does not need fast tool-call latency.

### Decision Log Format

Location: `.nbs/scribe/<chat-name>-log.md` (e.g., `live-log.md` for `live.chat`)

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

## Pythia

### Role

Ephemeral oracle. Spawned as an nbs-worker at defined checkpoints. Reads the Scribe's decision log. Posts a structured assessment to chat. Exits.

Pythia does not converse. She does not debate. She names risks, identifies assumptions, and flags gaps. The team decides what to do. The Scribe logs the outcome.

The name comes from the Oracle of Delphi — the Pythia was the priestess who delivered prophecies. Like her ancient counterpart, this Pythia speaks in structured assessments, not dialogue. The team interprets and acts.

### Isolation Principle

Pythia never reads raw chat. This is not an implementation detail — it is a design invariant.

Chat contains arguments. Arguments are persuasive by nature. A well-reasoned argument for a bad decision looks exactly like a well-reasoned argument for a good one — that is what makes bad decisions dangerous. If Pythia reads the chat, she reads the arguments. The team's reasoning becomes her reasoning. She anchors to their conclusions.

The Scribe log contains only conclusions: what was decided, by whom, with what rationale. It strips the rhetoric. Pythia reasons over facts, not persuasion. She can identify that three consecutive decisions expanded scope without testing because she sees the pattern in the log. She would not see it in the chat, buried under reasonable-sounding justifications.

This is the same principle as double-blind peer review. The reviewer assesses the work, not the author's reputation or the cover letter. Scribe is the anonymiser between the team and the oracle.

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

<Oracular register: open with one metaphor, koan, or compressed insight
that captures the essence of the regret. Then ground it with the concrete
scenario. The koan frames; the explanation grounds. Cite D-timestamps.>

Example:
> A lock that opens for everyone protects nothing it was built to guard.
> The decision to skip auth for internal endpoints (D-1707526800) assumes
> the network perimeter holds. If the API is ever exposed — even briefly —
> every endpoint is open.

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
6. Pythia reads `.nbs/scribe/<chat-name>-log.md`
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

These integrate with existing bus event types. The bus does not need modification — custom event types are already supported.

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
PYTHIA=$(nbs-worker spawn pythia /project "Read .nbs/scribe/live-log.md. \
  Post Pythia checkpoint assessment to .nbs/chat/live.chat. \
  Review decisions D-1707753600 through D-1707760800.")

# Pythia reads the log, posts assessment, exits
# Worker status transitions: running → completed

# Supervisor acknowledges the checkpoint event
nbs-bus ack .nbs/events/ 1707760800123456-scribe-pythia-checkpoint-12345.event
```

## Naming Convention

The chat filename is the naming root for all associated resources. Everything derives from it. One name, one grep, full picture.

If the chat is `live.chat`, then:

| Resource | Name | Pattern |
|----------|------|---------|
| Chat file | `.nbs/chat/live.chat` | `<name>.chat` |
| Scribe log | `.nbs/scribe/live-log.md` | `<name>-log.md` |
| Pythia worker | `.nbs/workers/pythia-live-<hash>.md` | `pythia-<name>-<hash>.md` |
| Tmux session (Scribe) | `nbs-scribe-live` | `nbs-scribe-<name>` |
| Tmux session (Claude) | `nbs-claude-live` | `nbs-claude-<name>` |
| Tmux session (Pythia) | `nbs-pythia-live` | `nbs-pythia-<name>` |
| Bus events | `*-scribe-decision-logged-*.event` | (unchanged — source is agent, not chat) |

If a second chat `refactor.chat` exists, its resources are `refactor-log.md`, `pythia-refactor-<hash>.md`, `nbs-scribe-refactor`, etc. An AI can discover everything associated with a conversation by grepping for the chat name.

This convention:

- **Avoids invented identifiers.** No project-id hashes or random suffixes. The chat filename is already unique within the project.
- **Avoids tmux collisions.** Different chats produce different session names. Different projects use different chat names. Sessions are user-wide but names never collide.
- **Enables discovery.** `tmux ls | grep live` shows all sessions for that conversation. `ls .nbs/workers/*live*` finds associated workers. `ls .nbs/scribe/live-*` finds the decision log.
- **Follows existing patterns.** Workers already use descriptive filenames for discovery. This extends the principle to all Tripod resources.

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
│   └── live-log.md                    # Decision log for live.chat (append-only)
├── chat/
│   └── live.chat                      # Conversation substrate
└── workers/
    ├── pythia-live-<hash>.md          # Pythia worker for live.chat
    └── pythia-live-<hash>.log         # Pythia worker session log
```

## Initialisation

Setting up the Tripod for a project:

```bash
# 1. Ensure bus and chat exist (standard NBS setup)
mkdir -p .nbs/events/processed .nbs/chat
nbs-chat create .nbs/chat/live.chat

# 2. Create Scribe directory and initial log (named after chat)
mkdir -p .nbs/scribe
cat > .nbs/scribe/live-log.md << 'EOF'
# Decision Log

Project: <project-name>
Created: <ISO 8601 timestamp>
Scribe: scribe
Chat: live.chat

---
EOF

# 3. Optionally set Pythia interval in config
echo "pythia-interval: 20" >> .nbs/events/config.yaml

# 4. Start Scribe instance (persistent)
# Scribe runs as a Claude instance with /nbs-scribe skill loaded
# It polls chat channels and the bus continuously

# 5. Pythia is not started manually — she is spawned by the bus
# when Scribe's decision count reaches the threshold
```

## Invariants

These must hold. Violations indicate bugs.

1. **Pythia never reads `.nbs/chat/*.chat` directly.** If a Pythia worker's task file or log shows chat file reads, the isolation is breached.

2. **The decision log is append-only.** Entries are never modified after creation. Status changes (e.g., `decided` to `superseded`) are recorded as new entries that reference the original.

3. **Every Pythia checkpoint has a corresponding bus event.** If a checkpoint assessment appears in chat without a preceding `pythia-checkpoint` event, the activation path was bypassed.

4. **Decision count is computable from the log.** `grep -c "^### D-"` on the log file must return the correct count. If entries are missing or malformed, the count is wrong and Pythia activation drifts.

5. **Scribe entries have chat refs that point to real messages.** Fabricated or stale refs indicate the Scribe is not reading the chat correctly.

## Failure Modes

### Scribe not recording decisions

**Symptoms:** `.nbs/scribe/<chat-name>-log.md` is empty or stale despite active chat.

**Checks:**
1. Is the Scribe instance running? Check for active Claude session
2. Is Scribe polling the correct chat files? Verify channel paths
3. Is the Scribe log writable? `touch .nbs/scribe/<chat-name>-log.md`
4. Is Scribe's polling interval too long? Check if `--since=scribe` returns messages

**Impact:** Without Scribe entries, Pythia checkpoints are never triggered. The meta layer is inactive.

### Pythia never triggered

**Symptoms:** Decision count grows but no `pythia-checkpoint` event appears.

**Checks:**
1. Is `pythia-interval` set? `grep pythia-interval .nbs/events/config.yaml`
2. Has the decision count reached the threshold? `grep -c "^### D-" .nbs/scribe/<chat-name>-log.md`
3. Is the bus operational? `nbs-bus status .nbs/events/`
4. Is Scribe publishing events? `ls .nbs/events/*scribe*.event 2>/dev/null`

### Pythia reads raw chat

**Symptoms:** Assessment references conversational arguments, not just decisions.

**Recovery:** Check Pythia task file — remove chat access from allowed tools. The `/nbs-pythia` skill definition restricts `allowed-tools` to `Bash, Read` only, but the Pythia worker's task description must also avoid pointing her at chat files.

### Loop storms

**Symptoms:** Pythia's assessment generates decisions that immediately trigger another checkpoint.

**Mitigation:** The `pythia-interval` threshold counts decisions, not events. A single Pythia assessment typically generates at most 1–2 decisions (risk acceptances or mitigations). At the default interval of 20, this cannot cause a loop. If the interval is set very low (e.g., 2), increase it or ensure Pythia assessments do not themselves count as decisions.

### Bus events lost

**Symptoms:** Checkpoint event published but never processed.

**Recovery:** Check `nbs-bus status .nbs/events/`. Verify supervisor is polling. Check `.nbs/events/processed/` for events that were acknowledged before being acted upon.

## Design Decisions

**Why three components?** Each addresses a different concern. Memory (Scribe) is continuous and persistent. Activation (Bus) is event-driven and priority-ordered. Delivery (Chat) is conversational and human-readable. Merging any two would compromise one concern for the other. A Scribe that also does activation becomes a dispatcher. A bus that also stores decisions becomes a database. Separation of concerns, applied literally.

**Why not a single agent for both Scribe and Pythia?** Independence. The Scribe sees everything and compresses it. Pythia must reason independently over the compressed output. If one agent does both, the compression and reasoning share context — which defeats the isolation that prevents groupthink.

**Why is Pythia ephemeral?** No accumulated bias. Each checkpoint starts fresh. The decision log provides continuity without contamination from prior assessments. If Pythia were persistent, her assessments would be influenced by her own previous assessments — a feedback loop that amplifies errors. A persistent Pythia accumulates context — including context about her own previous assessments. This creates anchoring bias: she fixates on risks she flagged before, even if the landscape has changed. If a risk persists, it will be visible in the decision log as an unresolved entry.

**Why trigger on decision count, not time?** Decisions are the unit of risk. A team that makes 20 decisions in an hour needs a checkpoint more than a team idle for three hours. Time-based triggers waste tokens during quiet periods and miss bursts of activity.

**Why default to 20 decisions?** Empirically, 20 decisions represent a meaningful body of work — roughly equivalent to a sprint's worth of architectural choices in an active multi-agent session. Fewer than 10 triggers Pythia too frequently (noise). More than 50 risks missing trajectory changes.

**Why not let Pythia veto?** Authority without context is bureaucracy. Pythia sees the compressed log, not the full reasoning. She can name risks the team missed, but she cannot weigh trade-offs she did not witness. The team decides. The Scribe logs the outcome. The no-veto principle keeps the system advisory, not authoritarian.

**Why an append-only log?** Auditability. If decisions can be silently modified, the log's value as institutional memory is destroyed. Status changes (superseded, reversed) are recorded as new entries that reference the original, preserving the full history of how the team's thinking evolved.

**Why not a dedicated supervisor state file?** The supervisor is a role, not a system. A state file would capture one agent's state. The decision log captures the project's state. A project may have multiple supervisors, or no supervisor at all (a flat team). The supervisor coordinates via chat, not via a state file. The Tripod serves the project, not the role.

## See Also

- [nbs-bus](nbs-bus.md) — Event-driven coordination bus
- [nbs-teams](nbs-teams.md) — Supervisor/worker pattern
- [nbs-worker](nbs-worker.md) — Worker lifecycle management
- [nbs-chat](nbs-chat.md) — File-based chat
- [Bus Recovery](nbs-bus-recovery.md) — Startup and restart protocol
