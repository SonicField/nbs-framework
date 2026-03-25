# How Agent Spawning Works (and Why)

## The Constraint

Claude Code expects to be launched from a human terminal. It reads stdin, writes to a PTY, negotiates trust with the workspace, and manages its own session lifecycle. It was not designed to be spawned by a daemon, a C binary, or a cron job.

Every attempt to launch Claude Code from anything other than a bash shell running in a terminal emulator has failed. The failures are subtle — Claude starts, connects to the API, processes a few turns, then exits silently. No error message. No crash. Just "Resume this session" and gone.

## The Solution

We launch Claude Code as though a human typed the command.

Three components:

| Component | Role |
|-----------|------|
| `nbs-ts` | Terminal emulator. Creates a PTY via the helper daemon. Claude runs inside this PTY exactly as it would in a human's terminal. |
| `nbs-launch-agent` | Bash function. Sets environment variables and calls `setsid nbs-claude`. This is the only code that launches Claude. |
| `nbs-claude` | Wrapper script. Handles session creation, sidecar startup, and attachment. Calls `nbs-ts create` internally to get Claude its PTY. |

The chain: caller → `nbs-launch-agent` → `setsid nbs-claude` → `nbs-ts create` (via helper) → Claude Code runs in a PTY.

## nbs-launch-agent

A bash function. Four arguments, one job:

```bash
launch_agent HANDLE PROJECT_ROOT NBS_CLAUDE_PATH INITIAL_PROMPT
```

It unsets inherited env vars that would confuse Claude, sets three environment variables as bash prefix vars on the command line, calls `setsid`, and backgrounds:

```bash
unset CLAUDECODE TMUX

NBS_HANDLE="$handle" \
NBS_TRANSPORT=ts \
NBS_INITIAL_PROMPT="$initial_prompt" \
setsid "$nbs_claude_path" --root="$project_root" --dangerously-skip-permissions \
    >/dev/null 2>&1 &
```

The `unset` is critical. When workers are spawned from within a running Claude Code session (e.g. `/pythia`), the parent's `CLAUDECODE=1` leaks into the child. Claude Code detects this variable on startup and treats it as a nested session — causing the child to exit after ~30 seconds. See "The CLAUDECODE Bug" below.

The function is shared by the restart script and `nbs-spawn-worker`. Both source the same file. One code path.

## What Does Not Work

Every alternative has been tried. None survived.

| Approach | Failure mode |
|----------|-------------|
| C `fork()` + `setsid()` + `execl("nbs-claude")` | Claude exits after a few API calls. Reason unknown but reproducible. |
| C `fork()` + `execl("bash", "-c", "nbs-claude ...")` | Same. The C process's environment or file descriptor state is wrong in ways we cannot identify. |
| `nbs-ts create "nbs-claude ..."` | Double session. nbs-claude creates its own nbs-ts session, so you get an outer session (tail -f) wrapping an inner session (claude). The outer session is pointless and its sidecar loop breaks. |
| `putenv()` + `execl()` in C | Stack-allocated env buffers may not survive exec correctly. Even when they do, Claude still exits. |
| Redirecting stdin to `/dev/null` | Claude exits immediately. It needs stdin open even though the actual input comes via the PTY. |
| `flock` on pid files | File descriptor 9 inherited by child processes (sidecar, tail). Lock held indefinitely. Blocked subsequent spawns of the same handle. Testkeeper was permanently blocked. |
| Timing-based coordination (`sleep N` then check) | Race conditions. The system must work without any timing assumptions. |

## The CLAUDECODE Bug (solved 2026-03-25)

Claude Code sets `CLAUDECODE=1` in its own environment. Any child process inherits this. When a new Claude Code instance starts and sees `CLAUDECODE=1`, it treats itself as a nested session and exits after ~30 seconds.

This was the root cause of the intermittent 30-second worker death that plagued oracle spawns (`/pythia`, `/librarian`, `/shepard`, `/fixup`). These are triggered from within an active Claude Code session, so the child inherits `CLAUDECODE=1`. Team agent restarts via the watchdog daemon (`nbs-chat-terminal-restart.sh`) were unaffected because the watchdog is not a Claude Code child — no `CLAUDECODE` in its environment.

The fix: `unset CLAUDECODE TMUX` in `launch_agent` before `setsid`. This gives every spawned agent a clean environment regardless of the caller.

## Two Separate Bugs

There are two distinct problems. Do not confuse them.

**Bug 1: CLAUDECODE inheritance (solved).** When oracles are spawned from within a Claude Code session (e.g. `/pythia` from the chat terminal, or sidecar triggers), `CLAUDECODE=1` leaks into the child. Claude detects this and exits. Fix: `unset CLAUDECODE TMUX` in `launch_agent`.

**Bug 2: C fork+exec kills Claude (unsolved).** Launching Claude via C `fork()` + `setsid()` + `execl()` fails even when CLAUDECODE is NOT set. Tested from the chat terminal (no Claude Code parent, no CLAUDECODE in environment). Claude starts, processes a few API calls, then exits with "Resume this session". The bash `setsid` command does not have this problem. The root cause is unknown. The C path is not used.

These are independent. Bug 1 affects bash callers running inside Claude Code. Bug 2 affects C callers regardless of context. The `launch_agent` bash function fixes Bug 1 and avoids Bug 2 by never using C fork+exec.

## Session Discovery: Names Not JSON

After spawning an agent, the monitor needs to find its nbs-ts session handle to check liveness and kill it when done. Two approaches exist. One works. One doesn't.

**Use: `nbs-ts list --name=<role>` (name-based lookup)**

nbs-claude creates named sessions (`nbs-pythia-poem`, `nbs-supervisor-poem`). The name is written atomically by `nbs-ts create --name=`. The monitor greps `nbs-ts list` output for the role name and extracts the hex handle. This is deterministic — if the session exists, the name exists.

```bash
TS_HANDLE=$("$NBS_TS" list 2>/dev/null \
    | grep "nbs-${ROLE}" | grep "alive" | head -1 | cut -f1 || true)
```

**Do not use: `.nbs/sessions/<handle>.json` (JSON metadata)**

nbs-claude writes a JSON metadata file with the session handle inside it. This file is:
- Written asynchronously — may not exist when the monitor checks
- Stale from a previous run — contains a dead session's handle
- Deleted by cleanup traps — disappears when the agent exits
- A race target — multiple restarts overwrite it

Parsing JSON in bash (`grep -o '"nbs_ts_handle"...' | sed ...`) is fragile. The file is a convenience for humans reading session state, not a reliable IPC mechanism.

**Why names work:** nbs-ts sessions have unique names assigned at creation time. The name lives in the session directory as a simple file. `nbs-ts list` reads it atomically. No parsing, no race, no stale data. If the session is alive, the name is there. If it's dead, `nbs-ts list` says dead. Simple.

**Naming convention:** `nbs-<handle>-<tag>` where tag comes from the chat filename. Examples: `nbs-pythia-poem`, `nbs-supervisor-live`, `nbs-librarian-poem`. These names are unique across teams (different tags) and roles (different handles). Use them for all session discovery.

## Callers

| Caller | How it uses launch_agent |
|--------|------------------------|
| `nbs-chat-terminal-restart.sh` | Sources `nbs-launch-agent`, calls `launch_agent` for each of the 6 team agents. |
| `nbs-spawn-worker` | Sources `nbs-launch-agent`, calls `launch_agent` for ephemeral workers (pythia, librarian, shepard, fixup). |
| `nbs-workers spawn` (C binary) | Creates the task file, then calls `nbs-spawn-worker` (bash) to do the actual launch. The C binary does not launch Claude itself. |

## Rules

- **Do not launch Claude from C.** Use the bash function.
- **Do not wrap nbs-claude in nbs-ts create.** nbs-claude handles its own session creation.
- **Do not redirect stdin.** Claude needs it open.
- **Do not use flock on pid files.** The lock gets inherited by children and blocks future spawns.
- **Do not add sleeps or timing.** The system works without them. Timing introduces race conditions.
- **Do not inherit CLAUDECODE or TMUX from the parent.** These cause nesting detection and early exit. `launch_agent` unsets them — do not remove the unset.
- **Do not change the launch_agent function** without testing that all oracles (pythia, librarian, shepard, fixup) survive for at least 60 seconds after spawn.
