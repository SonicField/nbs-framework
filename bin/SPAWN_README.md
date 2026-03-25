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

It sets three environment variables as bash prefix vars on the command line, calls `setsid`, and backgrounds:

```bash
NBS_HANDLE="$handle" \
NBS_TRANSPORT=ts \
NBS_INITIAL_PROMPT="$initial_prompt" \
setsid "$nbs_claude_path" --root="$project_root" --dangerously-skip-permissions \
    >/dev/null 2>&1 &
```

That is the entire function. It is shared by the restart script and `nbs-spawn-worker`. Both source the same file. One code path.

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

## Why Bash Works and C Does Not

We do not know the exact mechanism. What we know empirically:

- `setsid nbs-claude` from a bash script works every time.
- `fork()` + `setsid()` + `execl()` from C does not work, even with identical environment variables and arguments.
- The difference is not in the environment (verified by dumping `env` from both paths).
- The difference is not in the command-line arguments (verified by comparing `/proc/PID/cmdline`).
- The difference is somewhere in the process setup that C's fork+exec produces versus bash's setsid command.

Rather than debug something we cannot observe, we use what works. The bash function is the contract. Do not replace it with C.

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
- **Do not change the launch_agent function** without testing that all oracles (pythia, librarian, shepard, fixup) survive for at least 60 seconds after spawn.
