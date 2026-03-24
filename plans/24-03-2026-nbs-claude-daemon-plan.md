# Plan: nbs-claude --daemon

## Problem

There are currently four ways to spawn an agent, each working around the same set of bugs independently:

1. **Restart script** (`nbs-chat-terminal-restart.sh`) — uses `setsid nbs-claude --root=... >/dev/null 2>&1 &`
2. **nbs-workers spawn** (C binary) — uses `fork+exec nbs-ts create "cd ... && nbs-claude ..."`
3. **nbs-claude direct** — runs interactively, blocks on `nbs-ts attach` (tail -f)
4. **Manual respawn** — ad hoc bash commands with varying combinations of `setsid`, `nohup`, `&`

Each has produced its own bugs this session:

| Bug | Cause | Which path |
|-----|-------|-----------|
| Double nbs-ts session | Restart script wrapped nbs-claude in nbs-ts, nbs-claude created its own | Restart |
| Agents die on parent exit | `disown` doesn't work in non-interactive bash | Restart |
| Sidecar loop dies on signal | Subshell with `disown` not properly detached | All paths via nbs-claude |
| Wrong working directory | Helper spawns bash in HOME, not project dir | All paths |
| Trust dialog blocks startup | No pre-trust mechanism | All paths |
| Env var size limit | Full skill file in NBS_INITIAL_PROMPT | Restart |
| Orphan processes after restart | Old cleanup traps race with new agents | Restart |
| Workers die in 10 seconds | Monitor loop kills before trust dialog is dismissed | nbs-workers spawn |

The root cause: nbs-claude was designed for interactive use (human runs it, it blocks on `nbs-ts attach`). Every automated caller has to work around this by wrapping it in `setsid`, `&`, redirects, and hoping the sidecar subshell survives.

## Design: --daemon flag

`nbs-claude --daemon` means: "I am being launched by automation, not by a human. Do everything needed to run unattended and exit immediately after setup."

### What --daemon does

1. **Daemonise** — `setsid()` in C (not bash `setsid`), redirect stdout/stderr to log file, fork and have the parent exit immediately. The caller gets the exit code from the setup phase without waiting for the session to end.

2. **Create nbs-ts session** — same as current ts mode. Named session `nbs-<handle>-<tag>`.

3. **Start sidecar loop** — as a child of the daemonised process (same process group, survives signals). No `disown`, no temp scripts. The sidecar loop is a direct child of the daemon, not a subshell.

4. **Do NOT attach** — no `nbs-ts attach` (no `exec tail -f`). The daemon stays alive as the parent of the sidecar loop. When the nbs-ts session dies, the sidecar loop exits, the daemon cleans up and exits.

5. **Print session info to stdout before daemonising** — the caller captures the handle, session ID, and PID.

### What --daemon does NOT do

- No interactive terminal (no attach)
- No blocking — parent exits after setup
- No dependency on bash job control (`disown`, `bg`, `&`)

### Process tree comparison

**Current (interactive):**
```
nbs-claude (bash)
├── (subshell) sidecar auto-restart loop  [dies on signal]
│   └── nbs-sidecar
└── tail -f output.log  [blocks until session dies]
```

**With --daemon:**
```
nbs-claude-daemon (C or bash, setsid'd)
└── nbs-sidecar  [restarted by parent if it dies]
```

The nbs-ts session runs independently (daemon process, manages its own PTY). The daemon's only job is to keep the sidecar alive and clean up when the session dies.

### Implementation option A: bash with proper daemonisation

Keep nbs-claude as bash. Add a `--daemon` flag that:

```bash
if [[ "$DAEMON_MODE" == "1" ]]; then
    # Create session (same as current)
    SESSION_NAME=$("$NBS_TS" create $NBS_TS_NAME_FLAG "$CLAUDE_CMD")

    # Print info for caller
    echo "handle=$SESSION_NAME"
    echo "pid=$$"

    # Redirect and detach
    exec > "$NBS_LOG_FILE" 2>&1

    # Sidecar loop (direct, not subshell)
    while true; do
        "${SIDECAR_ARGS[@]}"
        rc=$?
        if ! "$NBS_TS" status "$SESSION_NAME" 2>/dev/null | grep -q "alive"; then
            break
        fi
        sleep 5
    done

    # Cleanup
    rm -f "$PIDFILE" "$SESSION_META"
    exit 0
fi
```

The key difference: no `&`, no `disown`, no subshell. The script itself IS the sidecar supervisor. `setsid` is done by the caller (restart script) or by nbs-claude itself.

### Implementation option B: C daemon binary

Write `nbs-claude-daemon` in C. It:
1. Parses args (handle, root, transport, initial-prompt, session name)
2. Calls `nbs-ts create` (fork+exec) with the claude command
3. Calls `fork()` + `setsid()` to daemonise
4. Parent prints session info and exits
5. Daemon child runs sidecar in a loop with proper signal handling
6. On SIGTERM: kills sidecar, kills nbs-ts session, exits

Advantage: proper Unix daemon with signal handling, no bash edge cases.
Disadvantage: another binary to maintain.

### Recommendation: Option A

Option A is simpler and keeps the existing nbs-claude logic. The current script already does 90% of the work — the --daemon flag just changes the tail end from "attach" to "sidecar loop". No new binary.

## Caller changes

### Restart script

Before:
```bash
NBS_HANDLE="$h" NBS_TRANSPORT=ts NBS_INITIAL_PROMPT="..." NBS_FORCE_SPAWN=1 \
    setsid "${NBS_BIN}/nbs-claude" --root="$PROJECT_ROOT" --dangerously-skip-permissions \
    >/dev/null 2>&1 &
```

After:
```bash
NBS_HANDLE="$h" NBS_TRANSPORT=ts NBS_INITIAL_PROMPT="..." NBS_FORCE_SPAWN=1 \
    "${NBS_BIN}/nbs-claude" --root="$PROJECT_ROOT" --dangerously-skip-permissions --daemon
```

No `setsid`, no `&`, no redirects. nbs-claude handles all of it.

### nbs-workers spawn

Before (C binary): fork+exec nbs-ts create with a complex command string wrapping nbs-claude.

After: fork+exec `nbs-claude --daemon --root=<dir> --handle=<slug>` with NBS_INITIAL_PROMPT set. No nbs-ts wrapping — nbs-claude creates its own session.

### Manual use

```bash
# Interactive (current default — human watching)
nbs-claude --root=/path/to/project

# Daemon (automation — exits immediately, sidecar runs in background)
nbs-claude --root=/path/to/project --daemon
```

## Sidecar lifecycle

With --daemon, the sidecar lifecycle is clean:

1. nbs-claude creates nbs-ts session
2. nbs-claude enters sidecar loop (not a subshell — the main process)
3. Sidecar runs, exits (crash, signal, whatever)
4. nbs-claude checks if session is alive → if yes, restart sidecar after 5s
5. nbs-claude checks if session is alive → if no, clean up and exit

The sidecar loop cannot die independently because it IS the main process. No orphaning, no signal races, no `disown`.

### nbs-sidecar-restart compatibility

`nbs-sidecar-restart` kills the sidecar process. The daemon's loop detects the exit and restarts it. This is the correct behaviour — `nbs-sidecar-restart` causes a sidecar restart, not a sidecar death.

### /pause compatibility

The sidecar itself checks the pause file. When paused, it sleeps 5s per tick. The daemon loop doesn't need to know about pause — it just keeps the sidecar running.

## Process cleanup

With --daemon, cleanup becomes simpler:

| To kill an agent | Do this |
|-----------------|---------|
| Graceful | `kill <daemon-pid>` — daemon cleans up sidecar and session |
| Hard | `nbs-ts kill <session-handle>` — daemon detects session death and exits |
| Warp core | `nbs-chat-init --name=<name>` — Phase 0 kills everything |

The daemon PID is in `.nbs/pids/<handle>.pid` (already written by nbs-claude).

## Testing

| Test | Verifies |
|------|----------|
| `nbs-claude --daemon` exits immediately | Caller not blocked |
| Session is alive after daemon exits | nbs-ts session running |
| Sidecar is alive after daemon exits | ps shows sidecar process |
| Kill sidecar → sidecar restarts | Daemon loop works |
| Kill nbs-ts session → daemon exits | Clean shutdown |
| Kill daemon PID → session and sidecar die | Cleanup works |
| Restart script spawns 6 agents via --daemon | No setsid/disown wrappers needed |

## Files to modify

1. `bin/nbs-claude` — add `--daemon` flag, daemon mode code path
2. `bin/nbs-chat-terminal-restart.sh` — simplify spawn to use `--daemon`
3. `src/nbs-workers/worker.c` — simplify spawn to use `nbs-claude --daemon`
4. `src/nbs-chat/terminal.c` — `spawn_trigger_worker` uses `nbs-claude --daemon`

## What this does NOT fix

- Trust dialog (still needs human or pre-trust mechanism)
- Worker 10-second death (if caused by trust dialog, not by process management)
- Sidecar detection logic (separate refactor, already done)

## Dependencies

None. This is a refactor of the spawn path. All callers already use nbs-claude — they just wrap it differently. --daemon eliminates the wrappers.

## Effort

Medium. The bash changes in nbs-claude are ~30 lines. The caller simplifications are ~5 lines each. The main risk is ensuring the daemon's sidecar loop handles all the edge cases (SIGTERM, session death, sidecar crash) that the current subshell loop handles badly.
