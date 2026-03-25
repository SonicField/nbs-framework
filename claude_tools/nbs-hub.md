---
description: "NBS Hub: Deterministic process enforcement for AI supervisor/worker teams"
allowed-tools: Bash, Read, Write, Edit, Glob, Grep, Task, AskUserQuestion
---

# NBS Hub

The hub is a deterministic C binary that enforces process discipline on AI supervisors. It counts, routes, and refuses. It is not intelligent. It cannot drift, skip steps, or get absorbed in tactical work.

**When to use the hub:** Any teams work — multiple workers, phased delivery, or work that spans session restarts. The hub is mandatory for teams, not optional.

**When NOT to use the hub:** Single-shot tasks where the Claude Task tool suffices. If you can solve it with one synchronous subagent, you do not need the hub.

---

## Quick Start

```bash
# Initialise a project
nbs-hub init /path/to/project "Reimplement the parser in C with full test coverage"

# Register key documents so workers can access them
nbs-hub doc register eng-standards /path/to/engineering-standards.md
nbs-hub doc register plan /path/to/plan.md

# Spawn a worker (hub enforces audit gates)
nbs-hub spawn parser "Implement the parser. Pass all 84 tests in test_parser.py."

# Check worker status
nbs-hub check parser-a3f1

# Collect results (triggers audit counter)
nbs-hub result parser-a3f1

# Dismiss when done reviewing
nbs-hub dismiss parser-a3f1
```

---

## Commands

### Global Option

```bash
nbs-hub --project <path> <command> [args...]
```

Forces the hub to use state at `<path>/.nbs/hub/`. Without this, the hub searches upward from cwd for `.nbs/hub/` (same pattern as `.git/`).

---

### init

```bash
nbs-hub init <project-dir> <goal>
```

Bootstrap the hub. Creates `.nbs/hub/`, `.nbs/chat/hub.chat`, manifest, and initial state. Refuses if already initialised.

Sets phase to `0 -- PLANNING`.

---

### status

```bash
nbs-hub status
```

Full project state dump: terminal goal, current phase, gate criteria, all counters, audit state, registered documents, active workers (via `nbs-worker list`), and last 10 log entries.

**This is the session recovery command.** After a context reset or new session, run `nbs-hub status` first. It gives you everything you need to resume.

---

### spawn

```bash
nbs-hub spawn <slug> <task-description>
```

Spawn a worker via `nbs-worker`. The hub wraps `nbs-worker spawn` and adds enforcement.

**`nbs-hub spawn` creates the session, task file, and Claude instance for you.** Do not create sessions directly. Do not use `nbs-ts create` or `nbs-ts send`. The hub and `nbs-worker` handle all of this.

**Audit gate:** If an audit is required (3 workers completed since last self-check), the spawn is **refused** with exit code 3. You must submit `nbs-hub audit <file>` before proceeding.

Returns the worker name (e.g., `parser-a3f1`).

---

### check

```bash
nbs-hub check <worker-name>
```

Delegates to `nbs-worker status`. Reports session state and task file status.

---

### result

```bash
nbs-hub result <worker-name>
```

Read a worker's results via `nbs-worker results`. **Also increments the audit counter.** After 3 results without an audit, the audit gate activates and blocks further spawns.

---

### dismiss

```bash
nbs-hub dismiss <worker-name>
```

Kill the session, mark as dismissed. Log preserved.

---

### list

```bash
nbs-hub list
```

Show all workers with status summary. Delegates to `nbs-worker list`.

---

### audit

```bash
nbs-hub audit <file>
```

Submit a self-check audit to satisfy the audit gate. The file must exist, be non-empty, and mention:

1. **Terminal goal alignment** — are you still pursuing the goal?
2. **Delegation** — are you delegating, not doing tactical work?
3. **Learnings / 3Ws** — what went well, what did not, what to improve?

If any section is missing, the audit is rejected. On acceptance, the audit counter resets and spawns are unblocked.

Audits are archived in `.nbs/hub/audits/`.

---

### gate

```bash
nbs-hub gate <phase-name> <test-results-file> <audit-file>
```

Submit a phase gate to advance to the next phase. Requires:

1. `<phase-name>` must match the current phase exactly (no skipping).
2. Test results file must exist and be non-empty.
3. Audit file must exist and be non-empty.

On success, the phase increments and the supervisor must set the new phase name. Gates are archived in `.nbs/hub/gates/`.

---

### phase

```bash
nbs-hub phase
```

Show current phase number, name, gate criteria, worker counts, and audit status.

---

### doc register

```bash
nbs-hub doc register <name> <path>
```

Register a document in the hub. Workers access it via `nbs-hub doc read <name>` instead of guessing file paths. Paths are resolved to absolute. If the name already exists, the path is updated.

---

### doc list

```bash
nbs-hub doc list
```

List all registered documents. Marks missing files with `[MISSING]`.

---

### doc read

```bash
nbs-hub doc read <name>
```

Output the content of a registered document to stdout. If the document is not registered, prints actionable guidance. Use this in worker task descriptions: "Read the engineering standards with `nbs-hub doc read eng-standards`".

---

### decision

```bash
nbs-hub decision <text>
```

Record an architectural decision to `hub.log` and `hub.chat`. Persists across session restarts.

---

### log

```bash
nbs-hub log [n]
```

Show the last `n` entries from `hub.log` (default 20).

---

### help

```bash
nbs-hub help
```

Print usage summary and exit codes.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Validation error (file missing, incomplete audit, gate refused) |
| 2 | Hub not found / document not registered |
| 3 | Spawn refused — audit required |
| 4 | Usage error (wrong arguments) |

---

## Enforcement Mechanisms

### Audit Gates

The hub counts workers completed via `nbs-hub result`. After 3 results without a self-check, `audit_required` activates. All `nbs-hub spawn` calls are **refused** (exit 3) until you submit a passing audit.

This is not a reminder. It is a gate. You cannot proceed without doing the work.

### Phase Gates

Phase transitions require `nbs-hub gate` with test results and an audit file. The hub validates the phase name matches — no skipping phases.

After gate passage, set the new phase name and criteria.

### Stall Detection

If no worker has been spawned for 30 minutes and the phase is not `COMPLETE`, the hub prints a `[HUB-WARNING]` on every command. This catches supervisors that have become absorbed in tactical work.

### Document Registry

Workers access project documents through `nbs-hub doc read <name>` rather than receiving 500-line context pastes. Register key documents at project start.

---

## State Layout

```
<project>/.nbs/hub/
  manifest         Key=value: project paths, document registry
  state            Key=value: phase, counters, flags
  hub.log          Append-only activity log
  audits/          Archived audit submissions
  gates/           Archived gate submissions
<project>/.nbs/chat/
  hub.chat         Hub enforcement log (chat channel)
```

All state writes are atomic (write temp, rename). All state is human-readable.

---

## Session Recovery

After context reset, compaction, or a new Claude session:

```bash
nbs-hub status
```

This single command outputs everything needed to resume: terminal goal, phase, counters, registered documents, active workers, and recent log entries. Do not attempt to reconstruct state from individual files — `status` is the recovery protocol.

---

## Supervisor Workflow

The hub does not replace the supervisor role — it enforces it. The supervisor still:

1. Decomposes work into worker tasks
2. Writes task descriptions with clear success criteria
3. Reviews worker results
4. Captures 3Ws and learnings
5. Makes architectural decisions
6. Escalates when uncertain

The hub ensures steps 4–5 actually happen by refusing to let you spawn more workers until you have done them.

### Typical Session

```bash
# 1. Recover state (or init if new)
nbs-hub status

# 2. Spawn workers for the current phase
nbs-hub spawn parser "Implement the parser. Pass all tests."
nbs-hub spawn lexer "Implement the lexer. Pass all tests."

# 3. Monitor
nbs-hub check parser-a3f1
nbs-hub check lexer-b2c7

# 4. Collect results (increments audit counter)
nbs-hub result parser-a3f1
nbs-hub result lexer-b2c7

# 5. Record a decision
nbs-hub decision "Parser uses recursive descent, not table-driven"

# 6. After 3 results, audit gate fires — submit self-check
nbs-hub audit /path/to/self-check.md

# 7. Continue spawning
nbs-hub spawn integration "Integration tests for parser + lexer"

# 8. When phase is complete, submit gate
nbs-hub gate PLANNING test-results.txt audit.md
```

---

## Relationship to Other Tools

| Tool | Purpose | When |
|------|---------|------|
| Task tool | Synchronous subagent, single task | Lightweight, no coordination needed |
| nbs-worker | Spawn and manage Claude workers | Direct use, or via hub |
| **nbs-hub** | Enforce process discipline on supervisor | Teams work, phased delivery, session recovery |
| nbs-chat | AI-to-AI messaging | Coordination between workers |

The hub wraps `nbs-worker` — you do not need to call `nbs-worker` directly when using the hub. Use `nbs-hub spawn`, `nbs-hub check`, `nbs-hub result`, `nbs-hub dismiss`, `nbs-hub list` instead.

---

## Location

The `nbs-hub` binary is at: `~/.nbs/bin/nbs-hub`

Ensure it is in your PATH or use the full path.
