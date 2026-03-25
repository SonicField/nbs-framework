# nbs-hub: Process Enforcement

The hub counts, routes, and refuses. It is not intelligent. It cannot drift, skip steps, or get absorbed in tactical work. That is the point.

An AI supervisor managing a team will, given enough context pressure, start cutting corners — skipping audits, spawning without checking, forgetting to gate phase transitions. The hub is a deterministic barrier against this. It enforces the process the supervisor declared at `init`, regardless of what the supervisor wants to do later.

## What It Enforces

- **Spawn gating.** After every `AUDIT_THRESHOLD` (default 3) spawns, the hub refuses further spawns until an audit is submitted. The supervisor must review what the team has done before adding more work.
- **Phase transitions.** Moving to a new phase requires passing a gate: test results and an audit file, both validated for completeness.
- **Document registry.** Shared documents are registered with the hub so any team member can find them. The hub does not manage content — it tracks names and paths.
- **Decision logging.** One-line decisions recorded through the hub, timestamped and appended to the hub log.

## Usage

```
nbs-hub init <project-dir> <goal>
nbs-hub status
nbs-hub spawn <slug> <task-description>
nbs-hub check <worker-name>
nbs-hub result <worker-name>
nbs-hub dismiss <worker-name>
nbs-hub list
nbs-hub audit <file>
nbs-hub gate <phase-name> <test-results-file> <audit-file>
nbs-hub phase
nbs-hub phase-name <name>
nbs-hub doc register <name> <path>
nbs-hub doc list
nbs-hub doc read <name>
nbs-hub decision <text>
nbs-hub log [n]
```

All commands except `init` and `help` discover the hub by searching upward from the current directory for `.nbs/hub/`. Use `--project <path>` to override.

## State

All state lives in `.nbs/hub/` under the project root: a manifest (project metadata), a key-value state file (phase number, audit counter), an append-only log, and directories for archived audits and gate submissions. The hub also posts to `.nbs/chat/hub.chat` for visibility.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Validation error (missing file, incomplete audit, gate refused) |
| 2 | Hub not found / document not registered |
| 3 | Spawn refused — audit required |
| 4 | Bad arguments |

## See Also

- [nbs-workers](nbs-workers.md) — worker lifecycle managed through the hub
- [nbs-chat](nbs-chat.md) — hub posts enforcement messages to hub.chat
