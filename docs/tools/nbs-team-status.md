# nbs-team-status: Team Process Audit

Show every process (session, sidecar, sidecar-loop) for a team, with duplicate and orphan detection.

## Purpose

When agents desync or behave erratically, the first diagnostic step is verifying the process landscape. `nbs-team-status` answers: how many sidecars are running, do any sessions have duplicates, are there orphaned processes?

## Usage

```bash
nbs-team-status <tag> <project-root>
```

**Arguments:**

| Argument | Description |
|----------|-------------|
| `tag` | Team tag (e.g., `harden`, `phoenix`) — used to identify sessions |
| `project-root` | Path to the project root (e.g., `/home/user/my-project`) |

## Output

```
supervisor:  session=8c73007c  sidecar=PID:737309  OK
generalist:  session=a37b0504  sidecar=PID:123456  OK
generalist:  (none)            sidecar=PID:789012  DUPLICATE — no session
testkeeper:  session=541029aa  sidecar=PID:234567  OK
                               sidecar=PID:345678  DUPLICATE — same handle
```

Each line shows one process association. Flags:
- **OK** — one session, one sidecar, correctly matched
- **DUPLICATE** — multiple sidecars for the same handle
- **ORPHAN** — sidecar running for a handle not in the expected agent list
- **MISSING** — expected agent has no session or no sidecar

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All processes healthy, no duplicates or orphans |
| 1 | Problems detected (duplicates, orphans, or missing processes) |

## When to Use

- Before restarting sidecars — verify the current process state
- After a restart — verify the restart produced the expected result
- When an agent stops responding — check for missing or duplicate sidecars
- As part of fixup diagnostics — called by `nbs-fixup-auto` for health checks

## See Also

- [nbs-team-kill](nbs-team-kill.md) — clean-slate kill of all team processes
- [nbs-sidecar-restart](../sidecar.md) — restart sidecars with deduplication
