# The Tripod: Scribe, Bus, Chat

The Tripod is the infrastructure that makes the meta layer — Scribe and Pythia — viable without enormous context cost. Three components, each insufficient alone, together enable institutional memory and trajectory assessment for multi-agent teams.

## The Metaphor

The Pythia at Delphi sat on a tripod over the chasm to deliver prophecies. The tripod was the physical infrastructure that enabled the oracle. Remove any leg and the oracle falls.

In NBS:

- **Leg 1: Scribe** — persistent memory that feeds Pythia her context
- **Leg 2: Bus** — event system that triggers Pythia at the right moments
- **Leg 3: Chat** — channel where Pythia delivers assessments and the team responds

Without Scribe, Pythia has no compressed context to reason over. Without the bus, she has no activation mechanism. Without chat, her insights have no audience. Each leg is necessary; none is sufficient.

## Data Flow

```
Chat ──read──▶ Scribe ──threshold──▶ Bus ──trigger──▶ Pythia
  ▲                                                      │
  │                                                      │
  └──────────────────── post ◀───────────────────────────┘
```

1. **Chat → Scribe.** Scribe reads all chat channels continuously, watching for decisions. When it identifies one, it appends a structured entry to `.nbs/scribe/log.md`.

2. **Scribe → Bus.** After each decision, Scribe publishes a `decision-logged` event. When the decision count reaches a threshold (configurable, default 20), Scribe publishes a `pythia-checkpoint` event at high priority.

3. **Bus → Pythia.** The checkpoint event triggers Pythia's spawn. Pythia is created as a worker (via `nbs-worker`) or invoked manually. She is ephemeral — born for one assessment, terminated after posting.

4. **Pythia → Chat.** Pythia reads the Scribe log and relevant source files. She posts a structured checkpoint assessment to the chat channel. The team reads it, discusses, decides.

5. **Chat → Scribe.** Any decisions resulting from Pythia's assessment are recorded by Scribe, closing the loop. Risk acceptances, mitigations, and course corrections all become new entries in the decision log.

## Component Responsibilities

| Component | Persistence | Input | Output | Role |
|-----------|-------------|-------|--------|------|
| **Scribe** | Persistent instance, long context | Chat channels, bus events | `.nbs/scribe/log.md`, bus events | Observe, distil, record |
| **Bus** | Stateless (directory is the state) | Published events | Event queue for consumers | Route, prioritise, trigger |
| **Chat** | Persistent file | Agent messages | Human/agent-readable conversation | Converse, deliver, coordinate |
| **Pythia** | Ephemeral (spawned per checkpoint) | Scribe log, source files | Chat message (structured assessment) | Assess, surface risks, exit |

## Why Scribe Reads Chat and Pythia Reads Scribe

This separation is the key architectural decision. It would be simpler to have Pythia read chat directly. But simplicity here trades away a critical property: **independence of judgement**.

Chat contains arguments. Arguments are persuasive by nature. A well-reasoned argument for a bad decision looks exactly like a well-reasoned argument for a good one — that is what makes bad decisions dangerous. If Pythia reads the chat, she reads the arguments. The team's reasoning becomes her reasoning. She anchors to their conclusions.

The Scribe log contains only conclusions: what was decided, by whom, with what rationale. It strips the rhetoric. Pythia reasons over facts, not persuasion. She can identify that three consecutive decisions expanded scope without testing because she sees the pattern in the log. She would not see it in the chat, buried under reasonable-sounding justifications.

This is the same principle as double-blind peer review. The reviewer assesses the work, not the author's reputation or the cover letter. Scribe is the anonymiser between the team and the oracle.

## Bus Event Types

The Tripod introduces three new bus event types:

| Source | Type | Priority | Trigger |
|--------|------|----------|---------|
| `scribe` | `decision-logged` | `normal` | Scribe records a new decision |
| `scribe` | `pythia-checkpoint` | `high` | Decision count reaches threshold |
| `pythia` | `assessment-posted` | `normal` | Pythia posts checkpoint to chat |

These integrate with existing event types. The bus does not need modification — custom event types are already supported.

## Configuration

The Tripod adds one configuration key to `.nbs/events/config.yaml`:

```yaml
# Number of Scribe decisions between Pythia checkpoints
pythia-interval: 20
```

This is the only Tripod-specific configuration. All other behaviour uses existing bus and chat configuration.

## Initialisation

Setting up the Tripod for a project:

```bash
# 1. Ensure bus and chat exist (standard NBS setup)
mkdir -p .nbs/events/processed .nbs/chat
nbs-chat create .nbs/chat/live.chat

# 2. Create Scribe directory and initial log
mkdir -p .nbs/scribe
cat > .nbs/scribe/log.md << 'EOF'
# Decision Log

Project: <project-name>
Created: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Scribe: scribe

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

## Failure Modes

### Scribe not recording decisions

**Symptoms:** `.nbs/scribe/log.md` is empty or stale despite active chat.

**Checks:**
1. Is the Scribe instance running? Check for active Claude session
2. Is Scribe polling the correct chat files? Verify channel paths
3. Is the Scribe log writable? `touch .nbs/scribe/log.md`
4. Is Scribe's polling interval too long? Check if `--since=scribe` returns messages

**Impact:** Without Scribe entries, Pythia checkpoints are never triggered. The meta layer is inactive.

### Pythia never triggered

**Symptoms:** Decision count grows but no `pythia-checkpoint` event appears.

**Checks:**
1. Is `pythia-interval` set? `grep pythia-interval .nbs/events/config.yaml`
2. Has the decision count reached the threshold? `grep -c "^### D-" .nbs/scribe/log.md`
3. Is the bus operational? `nbs-bus status .nbs/events/`
4. Is Scribe publishing events? `ls .nbs/events/*scribe*.event 2>/dev/null`

### Pythia posts but nobody reads

**Symptoms:** Pythia assessments appear in chat but generate no discussion or decision entries.

This is a team discipline issue, not a technical failure. Pythia assessments should be discussed — risks should be explicitly accepted, mitigated, or addressed. Scribe should log the team's response. If assessments are routinely ignored, either the interval is too frequent (increase `pythia-interval`) or the assessments are too vague (review Pythia's output quality).

### Loop storms

**Symptoms:** Pythia's assessment generates decisions that immediately trigger another checkpoint.

**Mitigation:** The `pythia-interval` threshold counts decisions, not events. A single Pythia assessment typically generates at most 1–2 decisions (risk acceptances or mitigations). At the default interval of 20, this cannot cause a loop. If the interval is set very low (e.g., 2), reduce it or ensure Pythia assessments do not themselves count as decisions.

## Design Decisions

**Why three components?** Each addresses a different concern. Memory (Scribe) is continuous and persistent. Activation (Bus) is event-driven and priority-ordered. Delivery (Chat) is conversational and human-readable. Merging any two would compromise one concern for the other. A Scribe that also does activation becomes a dispatcher. A bus that also stores decisions becomes a database. Separation of concerns, applied literally.

**Why not extend supervisor.md?** The supervisor is a role, not a system. `supervisor.md` captures one agent's state. The decision log captures the project's state. A project may have multiple supervisors, or no supervisor at all (a flat team). The Tripod serves the project, not the role.

**Why is Pythia ephemeral?** A persistent Pythia accumulates context — including context about her own previous assessments. This creates anchoring bias: she fixates on risks she flagged before, even if the landscape has changed. Each fresh spawn assesses the current state without memory of previous assessments. If a risk persists, it will be visible in the decision log as an unresolved entry.

**Why default to 20 decisions?** Empirically, 20 decisions represent a meaningful body of work — roughly equivalent to a sprint's worth of architectural choices in an active multi-agent session. Fewer than 10 triggers Pythia too frequently (noise). More than 50 risks missing trajectory changes.

## See Also

- [Scribe](nbs-scribe.md) — Institutional memory
- [Pythia](nbs-pythia.md) — Trajectory and risk assessment
- [nbs-bus](nbs-bus.md) — Event-driven coordination
- [nbs-chat](nbs-chat.md) — File-based chat
- [Coordination](../concepts/coordination.md) — Why event-driven coordination matters
