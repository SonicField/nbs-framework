# Plan: Oracle Lifecycle

## Context

Oracle workers (pythia, librarian, shepard, fixup) are ephemeral. They spawn, assess, post to chat, and should exit. Currently they don't — the sidecar keeps injecting notifications so they never go idle. Oracles accumulate as zombies.

Detection: the oracle posted to chat. That's how a human knows it's done.

## Design: bash reaper script

A standalone bash script `bin/nbs-oracle-reaper` that checks active oracles and kills completed ones. Called via `system()` or `fork+exec` from both the sidecar (C) and the chat terminal (C). No C lifecycle module — bash is the contract for all process management.

### `bin/nbs-oracle-reaper`

```
nbs-oracle-reaper check <project-root>
nbs-oracle-reaper register <role> <project-root>
```

**State file:** `.nbs/oracle-active.txt` — one line per active oracle:
```
pythia 1774436207
librarian 1774436300
```
Format: `<role> <spawn_epoch>`. Simple, no JSON.

**`register`:** Appends a line to the state file.

**`check`:** For each line in the state file:
1. Timeout (>600s elapsed) → kill session, remove line
2. Grace (<30s elapsed) → skip
3. Session not alive (`nbs-ts list --name=<role> | grep alive`) → remove line
4. Chat post found (`nbs-chat search <chat-file> "" --handle=<role> --after=<epoch>`) → kill session after 10s settle, remove line
5. Otherwise → keep

Session kill: `nbs-ts list --name=<role> | grep alive | head -1 | cut -f1` → `nbs-ts kill <handle>`.

### Callers

**sidecar (triggers.c):** After `exec_fire_and_forget(nbs-workers spawn ...)`, call `exec_fire_and_forget("nbs-oracle-reaper", "register", role, nbs_root)`. Then in the wall-clock section, every 10s: `exec_fire_and_forget("nbs-oracle-reaper", "check", nbs_root)`.

**terminal.c:** After `spawn_trigger_worker()`, call `system("nbs-oracle-reaper register <role> <root>")`. In the main loop, every 10s: `system("nbs-oracle-reaper check <root>")`.

### nbs-spawn-worker

Delete the monitor subshell. The reaper handles lifecycle.

## Files to modify

| File | Change |
|------|--------|
| `bin/nbs-oracle-reaper` | New bash script |
| `src/nbs-sidecar/triggers.c` | Add register call after spawn |
| `src/nbs-sidecar/sidecar.c` | Add check call every 10s |
| `src/nbs-chat/terminal.c` | Add register after /pythia etc, check in main loop |
| `bin/nbs-spawn-worker` | Delete monitor subshell |

## What does NOT change

- `bin/nbs-launch-agent` — untouched
- `bin/nbs-spawn-worker` launch logic — untouched
- Trigger lock/dedup — untouched
- No C lifecycle module — all bash

## Verification

1. `/pythia` → pythia posts to chat → session killed within 30s
2. Sidecar trigger → same
3. `.nbs/oracle-active.txt` empty after all oracles reaped
4. No orphan oracles after 15 minutes
5. `make clean && make` succeeds
6. Existing tests pass
