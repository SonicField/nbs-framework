# Feature Request: Live Team Dashboard

## Problem

Managing a 7-agent team requires constant context-switching between tools:

- `/health` gives a snapshot but no live updates
- `@handle?` is unreliable and only shows one agent at a time
- `nbs-team-status` shows processes but not what agents are doing
- Cursor state, sidecar health, and last-post times require separate commands
- There is no way to watch an agent's terminal output without leaving the chat

The human operator is flying blind between health checks. Problems (dead sidecars, cursor desyncs, stuck agents) are discovered minutes after they occur, not when they happen.

## Proposal

### `/dashboard` — a live team monitoring mode

A new full-screen mode in `nbs-chat-terminal` that shows the entire team's state in real time, with the ability to drill into any agent's terminal output.

### Overview Screen

The default view shows all 7 agents in a table, updated every 2 seconds:

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  NBS DASHBOARD — phoenix (7 agents, 7 sidecars)           04 Apr 15:32:18  ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                            ║
║  Agent        Status    Sidecar  Cursor   Last Post   Activity             ║
║  ──────────── ──────── ──────── ──────── ─────────── ────────────────────── ║
║  supervisor   alive    OK       1752/52  2m ago      Reading chat          ║
║  generalist   alive    OK       1750/52  30s ago     Bash: make -j8        ║
║  gatekeeper   alive    OK       1752/52  5m ago      Idle at prompt        ║
║  theologian   alive    OK       1751/52  3m ago      Thinking (42s)        ║
║▸ testkeeper   alive    OK       1748/52  1m ago      Bash: ./python test   ║
║  scribe       alive    OK       1752/52  4m ago      Bash: nbs-scribe-log  ║
║  medic        alive    OK       1752/52  8m ago      Grep: session log     ║
║                                                                            ║
║  Oracles: pythia (12m ago) shepard (18m ago) librarian (7m ago)            ║
║  Paused: no    Auto-repair: idle    Messages: 1752                         ║
╚══════════════════════════════════════════════════════════════════════════════╝
  ↑/↓ navigate  Enter: view agent  q: back to chat  r: refresh  ?: help
```

### Column Definitions

| Column | Source | Update frequency |
|--------|--------|-----------------|
| Agent | Static — the 7 permanent roles | — |
| Status | `nbs-ts find` + `nbs-ts status` | Every 2s |
| Sidecar | `pgrep -f "nbs-sidecar.*--handle=<agent>"` | Every 2s |
| Cursor | Read cursor file, compare to `nbs-chat count` — shows `cursor/behind` | Every 2s |
| Last Post | Scan last 50 chat messages for agent's handle, compute time delta | Every 5s |
| Activity | Last 3 lines of `nbs-ts read-new`, summarised to one line | Every 2s |

### Agent Detail View

Pressing `Enter` on a selected agent opens a live view of that agent's terminal output, rendered through `nbs-ts-render`:

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  generalist — session af152163                             04 Apr 15:32:18 ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                            ║
║  ● Bash(cd ~/project/cpython && make -j8)                                  ║
║    ⎿  [100%] Linking CXX shared library _cinderx.so                        ║
║                                                                            ║
║  ● Good — build succeeded. Now running test suite:                         ║
║                                                                            ║
║  ● Bash(./python -m pytest Python/jit/tests/ -x -q)                       ║
║    ⎿  Running… (23s · timeout 5m)                                          ║
║                                                                            ║
║  ✻ Thinking… (12s)                                                         ║
║                                                                            ║
║  ────────────────────────────────────────────────────────────────────────── ║
║  ❯                                                                         ║
║  ────────────────────────────────────────────────────────────────────────── ║
║    ⏵⏵ bypass permissions on · esc to interrupt                             ║
║                                                                            ║
╚══════════════════════════════════════════════════════════════════════════════╝
  Esc: back to overview  r: refresh  f: follow (auto-scroll)
```

The detail view pipes the agent's `output.log` through `nbs-ts-render` (the same mechanism as `@handle?` but live). It shows the last screen-clear point forward, giving a clean view of the agent's current state.

### Navigation

| Key | Overview screen | Detail view |
|-----|----------------|-------------|
| Up/Down arrows | Move cursor between agents | Scroll output |
| Page Up/Page Down | — | Scroll one page |
| Home/End | Jump to first/last agent | Jump to top/bottom of output |
| Enter | Open selected agent's detail | — |
| Escape | Exit dashboard, return to chat | Return to overview |
| q | Exit dashboard, return to chat | Return to overview |
| r | Force refresh all data | Force refresh output |
| f | — | Toggle follow mode (auto-scroll to bottom) |
| 1-7 | Jump to agent by number | — |
| s | Sort by: status, last-post, cursor-behind | — |
| ? | Show help | Show help |

Navigation uses standard terminal keys only — arrow keys, Page Up/Down, Home/End. No vim-style j/k. This is a monitoring tool for humans operating the team, not a text editor.

### Alerts

The dashboard highlights problems with colour:

| Condition | Colour | Column |
|-----------|--------|--------|
| Agent dead or missing | Red background | Status |
| Sidecar missing | Red background | Sidecar |
| Cursor behind by >10 | Yellow text | Cursor |
| Cursor behind by >50 | Red text | Cursor |
| No post for >15 minutes | Yellow text | Last Post |
| No post for >30 minutes | Red text | Last Post |
| "bypass permissions" in output | Yellow text | Activity |
| "context window" or "auto-compact" in output | Red text | Activity |

### Status Bar

The bottom row shows team-wide state:

- **Paused**: yes/no (control-pause file exists)
- **Auto-repair**: idle/running/triggered
- **Messages**: total chat message count
- **Oracle status**: last run time for pythia, shepard, librarian
- **Team health**: healthy/degraded/critical (same classification as fixup)

### Implementation

#### Architecture

The dashboard is a new mode in `nbs-chat-terminal`, similar to `/browse`. It saves terminal state on entry, takes over the full screen, runs its own poll loop, and restores state on exit.

```
src/nbs-dashboard/
    dashboard.c     — main dashboard logic: layout, data collection, rendering
    dashboard.h     — public API
    Makefile        — builds libdashboard.a

src/nbs-chat/
    terminal.c      — /dashboard command handler, calls dashboard_run()
```

Or, if the dashboard is complex enough to warrant it, a standalone binary:

```
bin/nbs-dashboard <chat-file>
```

that can be launched from the terminal via `/dashboard` (like how `/browse` works) or independently.

#### Data Collection

Each refresh cycle (every 2 seconds):

1. `nbs-ts find nbs-<role>-<tag>` for each agent — session handle and status
2. `pgrep -f "nbs-sidecar.*--handle=<role>"` — sidecar alive check
3. Read cursor file — one `grep` per agent
4. `nbs-chat count` — total message count
5. `stat .nbs/control-pause` — paused check

These are all cheap operations — no subprocess spawning except `nbs-ts find` (which is a socket call to the helper).

For the Activity column, `nbs-ts read-new <handle> --strip` with truncation to last 3 lines. This is the most expensive operation but still fast (reads from the session's output.log).

For the Detail view, `tail -c +N output.log | nbs-ts-render` — same mechanism as `handle_query` in the sidecar.

#### Rendering

Use the existing nbs-framework colour palette (`nbs_term_attr.c`). Box-drawing characters from the `nbs-md-viewer` table renderer. The palette is designed for dark terminals and tested across screen/tmux.

#### Live Updates

The dashboard runs its own poll loop (similar to the terminal's main loop but without stdin message sending). Every 2 seconds it collects data, diffs against the previous state, and redraws changed cells. Full redraw on terminal resize (SIGWINCH).

### What Does NOT Change

- The chat display — `/dashboard` is a mode you enter and exit, like `/browse`
- Agent behaviour — the dashboard is read-only, it never sends commands to agents
- Sidecar behaviour — the dashboard reads the same files sidecars read
- The chat file format
- Any existing slash commands

### Development

Build a fake team scenario with `nbs-ts create` sessions running simple scripts (sleep loops, echo commands) and test the dashboard rendering against them. Use `nbs-ts-render` for verification. Polish the TUI with real keyboard interaction before integrating into the terminal.

### Testing

| Test | Verification |
|------|-------------|
| `/dashboard` opens and `q` exits | Terminal state fully restored |
| Overview shows all 7 agents | Correct status, sidecar, cursor for each |
| Dead agent shows red | Kill an agent session, verify dashboard updates |
| Missing sidecar shows red | Kill a sidecar, verify dashboard updates |
| Cursor behind shows yellow/red | Desync a cursor, verify colour |
| Enter opens detail view | Agent terminal output renders correctly |
| Escape returns to overview | State preserved |
| Resize handled | SIGWINCH triggers redraw |
| Follow mode auto-scrolls | New output appears at bottom |
| Paused state shown | Create control-pause, verify status bar |
| Live update cycle | Data refreshes every 2 seconds without flicker |
