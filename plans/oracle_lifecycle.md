# Plan: Oracle Lifecycle Module

## Context

Oracle workers (pythia, librarian, shepard, fixup) are ephemeral. They spawn, assess, post to chat, and should exit. Currently they don't — the sidecar keeps injecting notifications so they never go idle, and the monitor subshell in nbs-spawn-worker can't find the session handle. Oracles accumulate as zombies.

The detection method: the oracle posted to chat. That's how a human knows it's done. The module does the same.

## Design

### New module: `src/nbs-common/oracle_lifecycle.{h,c}`

```c
int oracle_register(const char *role, const char *chat_dir, const char *project_root);
void oracle_tick(void);
int oracle_active_count(void);
```

Internal: array of up to 8 entries. Each tracks role, chat_dir, project_root, spawn_time, state (ACTIVE → DRAINING → reaped).

**`oracle_tick()` per entry:**

| Condition | Action |
|-----------|--------|
| Elapsed > 600s | Kill session, remove (timeout) |
| Elapsed < 30s | Skip (grace period) |
| State DRAINING and 10s elapsed | Kill session, remove |
| Session not alive (`nbs-ts list --name=<role>`) | Remove (natural death) |
| Chat post found (`nbs-chat search <file> "" --handle=<role> --after=<epoch>`) | Set DRAINING |

Session kill: `nbs-ts list --name=<role>` → parse handle → `nbs-ts kill <handle>`.

### Prerequisite: exec_util shared

Move `src/nbs-sidecar/exec_util.{h,c}` → `src/nbs-common/`. Both sidecar, workers, and terminal compile it as local .o (existing pattern).

### --no-monitor for nbs-workers spawn

New flag. `nbs-workers spawn --no-monitor` creates task file, launches worker, prints name, returns immediately. Callers pass this and let oracle_lifecycle handle reaping.

## Files to modify

| Phase | File | Change |
|-------|------|--------|
| 1 | `src/nbs-sidecar/exec_util.{h,c}` | Move to `src/nbs-common/` |
| 1 | Sidecar, workers, chat Makefiles | Update paths |
| 1 | `src/nbs-workers/worker.c` | Delete duplicate exec functions, use shared |
| 2 | `src/nbs-workers/main.c` + `worker.c` | --no-monitor flag |
| 3 | `src/nbs-common/oracle_lifecycle.{h,c}` | New module |
| 4 | `src/nbs-sidecar/triggers.c` | exec_capture + oracle_register (not fire_and_forget) |
| 4 | `src/nbs-sidecar/sidecar.c` | oracle_tick every 10s in wall-clock section |
| 4 | `src/nbs-chat/terminal.c` | exec_capture in spawn_trigger_worker, oracle_tick in main loop |
| 5 | `bin/nbs-spawn-worker` | Delete monitor subshell |

## Sequencing

```
Phase 1 (exec_util) → Phase 2 (--no-monitor) ─┐
                                                ├→ Phase 4 (integration) → Phase 5 (cleanup)
                      Phase 3 (oracle_lifecycle) ┘
```

## What does NOT change

- `bin/nbs-launch-agent` — untouched
- `bin/nbs-spawn-worker` launch logic — untouched (only monitor deleted)
- The setsid/bash launch pattern — untouched
- Trigger lock/dedup — untouched

## Verification

1. `/pythia` → pythia posts to chat → session killed within 30s of posting
2. Sidecar trigger fires → same
3. `oracle_active_count()` returns 0 after reaping
4. No orphan oracles after 15 minutes
5. `make clean && make` in all src/ directories
6. Existing tests pass
