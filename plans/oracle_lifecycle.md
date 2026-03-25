# Plan: Oracle Lifecycle

## Context

Oracle workers (pythia, librarian, shepard, fixup) are ephemeral. They spawn, assess, post to chat, and should exit. Currently they don't — the sidecar keeps injecting notifications so they never go idle. Oracles accumulate as zombies.

Detection: the oracle posted to chat. That's how a human knows it's done.

## Design: bash reaper script

A standalone bash script `bin/nbs-oracle-reaper` that checks active oracles and kills completed ones. Called via `system()` or `fork+exec` from both the sidecar (C) and the chat terminal (C). No C lifecycle module — bash is the contract for all process management.

### `bin/nbs-oracle-reaper`

```
nbs-oracle-reaper check <project-root>
```

**Stateless.** The reaper has no state file. It discovers active oracles directly from nbs-ts sessions using session names (e.g. `nbs-pythia-XXXX-poem`) and reads start timestamps from session meta files.

**`check`:** Discovers oracle sessions from `nbs-ts list`, then for each:
1. Timeout (>600s elapsed) → kill session
2. Grace (<30s elapsed) → skip
3. Session not alive → skip (already dead)
4. Chat post found (`nbs-chat search <chat-file> "" --handle=<role> --after=<epoch>`) → kill session after 10s settle
5. Otherwise → keep

Session kill: `nbs-ts list --name=<role> | grep alive | head -1 | cut -f1` → `nbs-ts kill <handle>`.

### Callers

**sidecar (triggers.c):** In the wall-clock section, every 10s: `exec_fire_and_forget("nbs-oracle-reaper", "check", nbs_root)`.

**terminal.c:** In the main loop, every 10s: `system("nbs-oracle-reaper check <root>")`.

### nbs-spawn-worker

nbs-spawn-worker has a bash monitor subshell that handles oracle lifecycle (10-minute timeout, task file state check). Uses unique session names per launch.

## Files to modify

| File | Change |
|------|--------|
| `bin/nbs-oracle-reaper` | New bash script |
| `src/nbs-sidecar/triggers.c` | Add check call in wall-clock section |
| `src/nbs-sidecar/sidecar.c` | Add check call every 10s |
| `src/nbs-chat/terminal.c` | Add check in main loop |
| `bin/nbs-spawn-worker` | Bash monitor subshell handles lifecycle |

## What does NOT change

- `bin/nbs-launch-agent` — untouched
- `bin/nbs-spawn-worker` launch logic — untouched
- Trigger lock/dedup — untouched
- No C lifecycle module — all bash

## Verification

1. `/pythia` → pythia posts to chat → session killed within 30s
2. Sidecar trigger → same
3. No oracle sessions remain in `nbs-ts list` after all oracles reaped
4. No orphan oracles after 15 minutes
5. `make clean && make` succeeds
6. Existing tests pass
