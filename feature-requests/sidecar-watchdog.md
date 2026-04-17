# Feature Request: Lightweight Sidecar Watchdog

## Problem

When a sidecar dies or an agent session disappears, the team continues deaf (no notifications) until either:
- A human notices and runs `/sidecar` (~minutes to hours)
- The hourly fixup oracle spawns and diagnoses (~up to 61 minutes)

The fixup oracle is expensive — it spawns a Claude instance, reads chat history, runs diagnostics. For the easy cases (dead sidecar with alive session, dead session), this is unnecessary. The dashboard already has deterministic code that detects these conditions. It just doesn't act on them.

## Proposal

### `nbs-sidecar-watchdog` — deterministic health check every 601 seconds

A C binary (or bash script sourcing `nbs-sidecar-lib.sh` once that exists) that runs as a background daemon, checks sidecar and session health, and takes the obvious corrective action. No AI. No context window. No Claude spawned.

### Interval

**601 seconds** (prime, ~10 minutes). Prime avoids synchronisation with other periodic processes (oracle intervals, poll intervals, heartbeats).

### Actions

| Condition | Detection | Action |
|-----------|-----------|--------|
| Agent session alive, sidecar missing | `nbs-ts status` alive, no `pgrep -x nbs-sidecar` with matching session+root | `nbs-sidecar-restart <handle>` |
| Agent session dead | `nbs-ts status` dead | `nbs-kick-agent <handle>` |
| Orphaned sidecar (session gone) | Sidecar running, session not in `nbs-ts list` | Kill the sidecar |
| Sidecar-loop dead, sidecar alive | Sidecar PPID=1 or parent not a sidecar-loop | Kill sidecar, `nbs-sidecar-restart <handle>` |

### What It Does NOT Do

- Diagnose stalled agents (agent alive but not producing output) — that's fixup's job
- Detect context exhaustion or modal dialogues — that's fixup's job
- Make judgement calls — all actions are deterministic
- Spawn Claude instances — zero AI cost

### Rate Limiting

Per-handle cooldown: if an action was taken for a handle in the last 601 seconds, skip it on the next cycle. Prevents restart loops if there's a persistent failure. After 3 consecutive failures for the same handle, stop trying and log a warning — let fixup handle it.

### Logging

Writes to `<root>/.nbs/sidecar-watchdog.log`:
```
2026-04-17T08:30:01Z check: 7 agents, 7 sidecars, 0 actions
2026-04-17T08:40:02Z action: scribe sidecar missing, restarting
2026-04-17T08:40:03Z action: scribe sidecar spawned PID 12345
2026-04-17T08:50:03Z check: 7 agents, 7 sidecars, 0 actions
```

### Lifecycle

Started by `nbs-chat-init` or the terminal's watchdog thread. Writes PID to `<root>/.nbs/pids/sidecar-watchdog.pid`. Killed by `nbs-team-kill`. Respects the pause file (`.nbs/control-pause`).

### Relationship to Other Components

| Component | Role after watchdog exists |
|-----------|---------------------------|
| Sidecar watchdog (new) | Deterministic fixes every 601s — dead sidecars, dead sessions, orphans |
| Fixup oracle (existing) | AI diagnosis every 61 min — stalled agents, context exhaustion, judgement calls |
| `/sidecar` command | Manual override — human-triggered, immediate |
| Dashboard | Display only — no longer the only way to notice problems |

The watchdog handles the 80% case (something died, restart it). Fixup handles the 20% case (something is alive but wrong).

### Implementation

If `nbs-sidecar-lib.sh` exists (from the refactor plan), the watchdog is ~50 lines of bash:

```bash
#!/bin/bash
source "$(dirname "$0")/nbs-sidecar-lib.sh"
INTERVAL=601
while true; do
    # ... check each agent, take action, log ...
    sleep "$INTERVAL"
done
```

If the library doesn't exist yet, it can use the same inline patterns currently in sidecar-restart, with the understanding that they'll be migrated to library calls later.

As a C binary (~200 lines), it avoids bash entirely and uses the same `nbs-ts` CLI for session queries.

### Verification

1. Kill a sidecar. Within 601 seconds, the watchdog restarts it.
2. Kill an agent session. Within 601 seconds, the watchdog kicks and respawns.
3. Pause the team. Watchdog takes no action.
4. Create a persistent failure (e.g., broken sidecar binary). After 3 attempts, watchdog stops trying and logs.
5. Run for 24 hours across three projects. No cross-project interference.
