# Feature Request: Sidecar Manager

## Problem

The sidecar lifecycle is the single most unstable component in the NBS framework. 75 commits, 35 of which are fixes or reverts — a 47% fix rate. The sidecar binary itself is stable. The instability is entirely in the lifecycle management: the bash scripts that launch, find, kill, and respawn sidecars.

### Root Cause

Five independent code paths manage sidecar processes, each reconstructing state from process tables:

| Owner | What it does | How it finds sidecars |
|-------|-------------|----------------------|
| `nbs-claude` | Creates sidecar + loop script | Knows because it just spawned it |
| `nbs-sidecar-restart` | Kills and respawns | `pgrep` + `/proc/PID/cmdline` parsing |
| `nbs-fixup-auto.md` | Diagnoses and repairs | `pgrep` + PID files |
| `/sidecar` (terminal) | Calls nbs-sidecar-restart | Inherits sidecar-restart's bugs |
| `nbs-team-kill` | Kills everything | `pkill` by pattern |

Each owner has a partial, stale, and often wrong model of what sidecars exist. They fight each other.

### Bug Catalogue (Non-Exhaustive)

| Bug | Root cause |
|-----|-----------|
| Orphaned sidecars (PPID=1) | Sidecar-loop killed, sidecar survives with no parent |
| Stale PID files block respawn | Sidecar dies, PID file remains, new sidecar sees "duplicate" |
| `pgrep -f` matches itself | Command-line pattern appears in pgrep's own `/proc/PID/cmdline` |
| Cross-project session theft | Session re-discovery greps all `nbs-ts` sessions globally |
| `printf '%q'` escapes `$CURRENT_SESSION` | Shell variable in heredoc passes through printf quoting |
| Handle extraction eats tag | Regex `[a-zA-Z0-9_-]+` includes hyphen, matches `supervisor-vib` not `supervisor` |
| 14-retry duplicate detection race | PID marker not removed during kill phase |
| Cross-project duplicate detection | Sidecar from project A blocks respawn in project B |
| Sidecar-loop scripts leak in /tmp | Loop killed before reaching self-delete `rm` |
| Silent sidecar death | SIGHUP from shell exit kills backgrounded sidecar |

Every one of these bugs is a consequence of distributed state reconstruction from process tables. Fix one, another appears, because the architecture is fundamentally wrong.

## Proposal

### `nbs-sidecar-mgr` — Single Owner for Sidecar Lifecycle

A single long-lived C process that owns all sidecar processes for one project. Everything else talks to it instead of scanning process tables.

### Architecture

```
nbs-sidecar-mgr (one per project)
├── forks sidecar for supervisor
├── forks sidecar for gatekeeper
├── forks sidecar for generalist
├── forks sidecar for testkeeper
├── forks sidecar for theologian
├── forks sidecar for scribe
└── forks sidecar for medic
```

The manager is the direct parent of every sidecar. It receives SIGCHLD when they die. No orphans, no PID files, no `pgrep`.

### State File

`<root>/.nbs/sidecar-state.honest` — the single source of truth.

```
type SidecarState = record
    handle   : String;
    session  : String;
    pid      : Integer;
    status   : (Alive, Dead, Restarting);
    started  : String;      { ISO 8601 timestamp }
    restarts : Integer;     { count since manager start }
end;

type ManagerState = record
    manager_pid : Integer;
    project_root : String;
    sidecars : sequence of SidecarState;
end;

var state : ManagerState;
```

Written atomically on every state change. Readable by any tool without IPC.

### Command Interface

Unix socket at `<root>/.nbs/sidecar-mgr.sock` plus a CLI wrapper:

```bash
nbs-sidecar-mgr status                    # print state (or read state file directly)
nbs-sidecar-mgr start <handle> <session>  # spawn a sidecar for this agent
nbs-sidecar-mgr restart <handle>          # kill and respawn one
nbs-sidecar-mgr restart-all               # kill and respawn all
nbs-sidecar-mgr update-session <handle> <new-session>  # agent restarted, switch session
nbs-sidecar-mgr stop <handle>             # kill one, don't respawn
nbs-sidecar-mgr shutdown                  # kill all, exit manager
```

### Session Re-Discovery

The manager periodically checks `nbs-ts list` for its agents' sessions. If a session dies and a new one appears with the same name pattern (`nbs-<handle>-<tag>`), the manager:

1. Updates the session ID in state
2. Kills the old sidecar (it's stuck on a dead session)
3. Spawns a new sidecar with the new session ID
4. Writes the updated state file

This replaces the sidecar-loop session re-discovery logic entirely.

### Auto-Restart on Sidecar Crash

When the manager receives SIGCHLD for a sidecar:

1. Check if the agent's session is still alive (`nbs-ts status`)
2. If alive: restart the sidecar after a 5-second delay. Increment restart counter.
3. If dead: mark the sidecar as Dead. Don't restart.
4. Rate limit: max 10 restarts per hour per handle. After that, mark as Dead and log a warning.

This replaces the sidecar-loop bash scripts entirely.

### Manager Lifecycle

Started by `nbs-chat-init` or the first `nbs-claude` launch. Writes its PID to `<root>/.nbs/sidecar-mgr.pid`.

If the manager dies:
- Sidecars become orphaned (PPID=1) but continue running
- The next `nbs-claude` or `/sidecar` command detects the manager is dead
- A new manager starts, reads the state file, discovers what should be running
- Reconciliation: for each entry in the state file, check if the PID is alive. If not, respawn. If a sidecar is running but not in the state file, adopt or kill it.

### Integration Changes

| Component | Current | After |
|-----------|---------|-------|
| `nbs-claude` | Creates sidecar-loop heredoc | Calls `nbs-sidecar-mgr start <handle> <session>` |
| `nbs-sidecar-restart` | Scans `/proc`, kills, respawns | Thin wrapper: calls `nbs-sidecar-mgr restart <handle>` |
| `/sidecar` (terminal) | Calls nbs-sidecar-restart | Calls nbs-sidecar-restart (which calls manager) |
| `nbs-fixup-auto` | `pgrep` + PID file checks | Reads state file, calls manager to restart |
| `nbs-team-kill` | `pkill` by pattern | Calls `nbs-sidecar-mgr shutdown` |
| `nbs-kick-agent` | Kills sidecar, removes PID | Calls `nbs-sidecar-mgr update-session` after respawn |
| `nbs-chat-terminal-restart.sh` | Kills sidecars by pattern | Calls `nbs-sidecar-mgr shutdown` then `nbs-sidecar-mgr start` for each |
| Dashboard | `pgrep` for sidecar status | Reads state file |
| `/health` | `nbs-team-check` uses `pgrep` | Reads state file |

### What Gets Deleted

- `nbs-sidecar-restart` bash script (~350 lines) → thin wrapper (~20 lines)
- `nbs-sidecar-find-session` → absorbed into manager
- Sidecar-loop heredoc generation in `nbs-claude` (~30 lines)
- Sidecar-loop heredoc generation in `nbs-sidecar-restart` (~60 lines × 2)
- PID file management for sidecars in fixup, kick-agent, claude
- All `pgrep -f "nbs-sidecar"` patterns across the codebase
- All `/proc/PID/cmdline` parsing for sidecar discovery

Estimated deletion: ~400 lines of bash. Estimated addition: ~500 lines of C.

### What This Eliminates

| Current problem | Gone because |
|----------------|-------------|
| Orphaned sidecars (PPID=1) | Manager is always the direct parent |
| Stale PID files | No PID files — manager tracks its own children |
| `pgrep -f` matching itself | No pgrep — manager knows its children's PIDs |
| Cross-project session theft | Manager scopes to one project root |
| Shell script heredoc quoting | No generated scripts — manager forks directly |
| Five owners fighting | One owner, everyone else asks it |
| Sidecar-loop scripts leaking in /tmp | No loop scripts |
| Handle extraction regex bugs | Manager receives the handle explicitly at spawn time |
| Silent sidecar death | Manager receives SIGCHLD directly |
| Duplicate detection races | Manager has authoritative child list |

### Risks

**Single point of failure.** If the manager crashes, no sidecars restart until a new manager starts. Mitigation: the manager is a simple C program (fork, waitpid, signal handling, socket listener) with no complex logic. Much less likely to crash than bash scripts parsing `/proc`.

**Migration.** Existing running teams need to transition. The manager must handle the case where sidecars were launched by the old system (no manager, sidecar-loops in /tmp). On first start, it should adopt existing sidecars by scanning process tables once, then never scan again.

**Socket-based IPC.** Adds a Unix socket. The socket must be cleaned up on exit. If the manager crashes without cleanup, the stale socket must be detected and removed on restart. This is a well-understood problem (check PID in lock file, unlink stale socket).

### Implementation Order

1. Write the manager binary (C, ~500 lines)
2. Add state file read/write
3. Add Unix socket command interface
4. Add session re-discovery loop
5. Add auto-restart with rate limiting
6. Modify `nbs-claude` to call manager instead of creating loop
7. Reduce `nbs-sidecar-restart` to a thin wrapper
8. Modify fixup to read state file
9. Modify dashboard/health to read state file
10. Delete dead code (loop heredocs, pgrep patterns, PID file management)
11. Migration: manager adopts pre-existing sidecars on first run

### Verification

1. All existing sidecar tests pass
2. Manager survives agent restart (session re-discovery works)
3. Manager survives sidecar crash (auto-restart works)
4. Manager survives its own restart (reconciliation from state file works)
5. `/sidecar scribe` works on first attempt (no pgrep, no PID file, no cross-project)
6. Multiple projects on the same machine don't interfere
7. Dashboard shows correct sidecar state from state file
8. No sidecar-loop scripts in /tmp after migration

### The Test

Run three teams on the same machine for 24 hours. Kill sidecars randomly. Restart agents. Kill the manager. Verify that after every disruption, the system self-heals within 30 seconds and no cross-project contamination occurs.

If the sidecar fix rate drops from 47% to under 5% in the month after deployment, the redesign worked.
