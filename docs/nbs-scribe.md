# nbs-scribe: Institutional Memory

Scribe is the persistent memory layer for NBS teams. It distils decisions from chat into a structured log that survives compaction, session restarts, and agent rotation.

## The Problem

Chat is ephemeral by design. Conversations accumulate, context windows fill, compaction discards everything but a summary. Decisions made at hour two are lost by hour six. An agent restarting mid-project reads "the team decided X" in a summary — but not why, not what alternatives were rejected, not what risks were accepted.

The Scribe solves this by maintaining a structured decision log separate from chat. Every decision is recorded once, with provenance, and persists indefinitely.

## How It Works

Scribe is a persistent Claude instance with a long context window (up to 1M tokens). It reads all chat channels continuously and distils decisions into `.nbs/scribe/log.md`. It does not participate in conversation — it observes, records, and makes the record available.

### What Scribe Records

Scribe tracks **decisions** — moments where the team chose a direction. Not every message, not status updates, not greetings. Decisions.

Signals that a decision has occurred:
- Explicit agreement: "let's do X", "agreed", "go with option 2"
- Task assignment: "you handle X, I'll handle Y"
- Architecture choice: "we'll use file-based events, not sockets"
- Risk acceptance: "we know X could break, proceeding anyway"
- Course correction: "actually, let's switch to Y instead"

Scribe does not record:
- Routine status updates ("tests passing", "build complete")
- Social messages ("hello", "thanks")
- Intermediate discussion that did not result in a decision
- Implementation details (those belong in commit messages)

### Activation

Scribe runs as a persistent instance with the `/nbs-scribe` skill loaded. It polls chat channels using `nbs-chat read` and monitors the bus for relevant events. When it identifies a decision, it appends a structured entry to the log.

### Pythia Integration

Scribe counts decision entries. When the count reaches a configurable threshold (`pythia-interval` in `.nbs/events/config.yaml`, default 20), Scribe publishes a `pythia-checkpoint` bus event. This triggers a Pythia assessment — see [nbs-pythia](#see-also).

Scribe reads from chat. Pythia reads from Scribe. This separation prevents persuasion bias: Pythia reasons over distilled facts, not persuasive arguments.

## Log Format

The decision log lives at `.nbs/scribe/log.md`. It is a markdown file with structured entries.

### File Structure

```markdown
# Decision Log

Project: <project name>
Created: <ISO 8601 timestamp>
Scribe: <scribe instance handle>

---

### D-1707753600 Coordination bus replaces polling
- **Chat ref:** live.chat:~L342
- **Participants:** alex, claude
- **Artefacts:** docs/nbs-bus.md, bin/nbs-bus
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Polling wastes context tokens on empty reads. File-based events
  are crash-recoverable and require no daemon.

---

### D-1707840000 MVP-first for bus implementation
- **Chat ref:** live.chat:~L869
- **Participants:** alex, claude, bench-claude
- **Artefacts:** —
- **Risk tags:** scope-creep
- **Status:** decided
- **Rationale:** Build publish/subscribe/acknowledge/list/prune first. Dedup
  and inotifywait deferred to post-review phase. Adversarial tests required.

---
```

### Entry Schema

Each entry begins with `### D-<unix-timestamp> <one-line summary>` and contains the following fields:

| Field | Required | Description |
|-------|----------|-------------|
| `Chat ref` | yes | Source chat file and approximate line number (`file:~Lnnn`) |
| `Participants` | yes | Handles of agents/humans involved in the decision |
| `Artefacts` | no | Commit hashes, file paths, or other concrete outputs linked to the decision |
| `Risk tags` | yes | `none` if no risks identified, otherwise a comma-separated list |
| `Status` | yes | One of: `decided`, `accepted-risk`, `mitigated`, `superseded`, `reversed` |
| `Rationale` | yes | 1–3 sentences explaining why this decision was made |

### Field Details

**D-<unix-timestamp>**: Unix timestamp (seconds) of when the decision was logged. Not when it was made — when Scribe recorded it. The timestamp is used for sorting and deduplication. The `D-` prefix distinguishes decision entries from other headings in the file.

**Chat ref**: Approximate location in the chat file. The `~L` prefix indicates this is approximate (chat line numbers shift as messages are added). Format: `<filename>:~L<number>`. Multiple refs separated by commas if the decision spans messages.

**Participants**: Comma-separated handles. Only agents/humans who actively contributed to the decision, not everyone in the channel.

**Artefacts**: Concrete outputs. Commit hashes (`fa788ce`), file paths (`docs/nbs-bus.md`), task IDs, diff numbers. Use `—` (em-dash) if no artefacts yet.

**Risk tags**: Free-form labels for risks associated with this decision. Common tags:
- `scope-creep` — decision expands scope beyond original plan
- `tech-debt` — decision accepts known technical debt
- `untested` — decision involves unverified assumptions
- `perf-risk` — performance implications not measured
- `breaking-change` — affects existing interfaces
- `reversible` / `irreversible` — ease of reversal

**Status lifecycle**:
- `decided` → initial state when decision is logged
- `accepted-risk` → team acknowledged a risk and proceeded
- `mitigated` → a previously identified risk was addressed
- `superseded` → a later decision replaced this one (add ref to superseding entry)
- `reversed` → decision was explicitly undone (add ref to reversal entry)

### Querying the Log

The log is plain markdown. Query with standard tools:

```bash
# All decisions
grep "^### D-" .nbs/scribe/log.md

# Decisions with risk tags
grep -A6 "^### D-" .nbs/scribe/log.md | grep -B1 "Risk tags:" | grep -v "none"

# Decisions involving a specific participant
grep -A6 "^### D-" .nbs/scribe/log.md | grep -B2 "alex"

# Decisions by status
grep -A6 "^### D-" .nbs/scribe/log.md | grep "Status: superseded"

# Count decisions (for Pythia threshold)
grep -c "^### D-" .nbs/scribe/log.md
```

## Bus Integration

When `.nbs/events/` exists, Scribe publishes bus events:

| Trigger | Event type | Priority | Payload |
|---------|-----------|----------|---------|
| Decision logged | `scribe decision-logged` | `normal` | Decision summary |
| Pythia threshold reached | `scribe pythia-checkpoint` | `high` | Decision count, last N summaries |
| Log format error detected | `scribe log-error` | `high` | Error description |

Scribe also subscribes to:
| Event type | Action |
|------------|--------|
| `chat-message` | Read chat for potential decisions |
| `chat-mention` | Read chat, prioritise if @scribe mentioned |
| `task-complete` | Check if completion represents a decision outcome |
| `pythia assessment-posted` | Log any risk-acceptance decisions from Pythia's assessment |

## File Convention

```
.nbs/
├── scribe/
│   └── log.md          # Decision log (append-only)
├── events/
│   └── ...             # Bus events
├── chat/
│   └── ...             # Chat channels
└── ...
```

The `.nbs/scribe/` directory is created by the Scribe instance on first run. The log file is append-only — entries are never modified in place. Status changes are recorded as new entries referencing the original.

## Initialisation

```bash
# Create the Scribe directory
mkdir -p .nbs/scribe

# Create the initial log file
cat > .nbs/scribe/log.md << 'EOF'
# Decision Log

Project: <project-name>
Created: <timestamp>
Scribe: scribe

---
EOF
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Log file not found |
| 3 | Malformed entry |

## Design Decisions

**Why a separate file, not supervisor.md?** Supervisor state is per-session and per-role. The decision log is project-wide and role-independent. A decision made by bench-claude and alex is as relevant to doc-claude as to claude. The log must outlive any individual session.

**Why markdown, not YAML?** The log is meant to be read by humans scanning a file. Markdown headings are visually scannable. YAML would require tooling to read comfortably. The structured fields within each entry use a consistent format that is both human-readable and grep-parseable.

**Why approximate line numbers?** Chat files grow continuously. Exact line numbers would be stale within minutes. The `~L` prefix signals "approximately line N at time of recording". Good enough for context, not precise enough to be misleading.

**Why append-only?** The decision log is an audit trail. Modifying past entries would undermine trust in the record. Status changes are new entries, not edits. This matches the append-only pattern used throughout NBS (chat files, event files, control inbox).

## See Also

- [Pythia](nbs-pythia.md) — Trajectory and risk assessment (reads from Scribe)
- [Tripod](tripod.md) — Architecture connecting Scribe, Bus, and Chat
- [nbs-bus](nbs-bus.md) — Event-driven coordination
- [nbs-chat](nbs-chat.md) — File-based chat
- [Coordination](../concepts/coordination.md) — Why event-driven coordination matters
