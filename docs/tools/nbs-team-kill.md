# nbs-team-kill: Clean-Slate Team Process Termination

Kill ALL processes for a team: sidecar-loops, sidecars, and sessions. Clean PID files.

## Purpose

When duplicate processes, orphaned sidecars, or corrupt state make targeted fixes impractical, `nbs-team-kill` provides a clean slate. It kills everything for a team in the correct order — sidecar-loops first (to prevent respawn), then sidecars, then sessions — and cleans up PID marker files.

## Usage

```bash
nbs-team-kill <tag> <project-root> [--force]
```

**Arguments:**

| Argument | Description |
|----------|-------------|
| `tag` | Team tag (e.g., `harden`, `phoenix`) |
| `project-root` | Path to the project root |
| `--force` | Skip confirmation prompt (use in scripts) |

## Kill Order

The kill order is critical. Killing in the wrong order causes respawn races:

1. **Sidecar-loops** — the bash respawn wrappers in `/tmp/nbs-sidecar-loop.*.sh`. If killed after sidecars, they immediately respawn what was just killed.
2. **Sidecars** — the C sidecar processes. Safe to kill after loops are dead.
3. **Sessions** — the nbs-ts terminal sessions. Killed last so agents can see shutdown messages.

After killing, the tool removes PID marker files from `.nbs/pids/sidecar-*.pid`.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All processes killed successfully |
| 1 | Some processes could not be killed |

## When to Use

- Before a full team restart — ensures no orphaned processes survive
- After detecting 2x expected sidecar count in `nbs-team-status`
- When `nbs-sidecar-restart` cannot resolve duplicates
- Emergency shutdown of a misbehaving team

## Caution

This is destructive — all agents lose their in-flight context. Use `nbs-team-status` first to understand what is running. Prefer `nbs-sidecar-restart` for targeted sidecar fixes.

## See Also

- [nbs-team-status](nbs-team-status.md) — audit process state before killing
- [nbs-sidecar-restart](../sidecar.md) — targeted sidecar restart with deduplication
