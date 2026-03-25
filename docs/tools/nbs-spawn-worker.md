# nbs-spawn-worker: The Worker Spawn Pipeline

Spawning a Claude agent from inside another Claude agent should not work. Claude Code was designed for human terminals, not daemon children. This pipeline is the result of finding, the hard way, the one path that survives.

## The Pipeline

```
nbs-chat-terminal (/pythia)
  → spawn_trigger_worker (C, double-fork)
    → nbs-workers spawn (C)
      → nbs-spawn-worker (bash)
        → launch_agent (bash)
          → setsid nbs-claude
            → nbs-ts create (via nbs-ts-helper)
              → Claude Code runs in a PTY
```

The C layer handles lifecycle (double-fork to detach from the caller). The bash layer handles launching. This division exists because Claude Code dies when launched directly from C `fork()` + `execl()`. The reason is unknown. The workaround is: let bash do it.

## nbs-spawn-worker

A bash script. Four arguments: role, project root, skill file, task instructions. It does three things.

**1. Creates a task file**

Location: `.nbs/workers/<role>-<suffix>.md`

The suffix is 4 bytes of `/dev/urandom` rendered as hex. The full worker handle is `ROLE-SUFFIX` (e.g. `pythia-a1b2c3d4`). This handle is unique per launch — two `/pythia` invocations a second apart get different handles, different task files, different sessions.

The task file contains the skill content (embedded verbatim from the skill `.md` file), the task instructions, tooling tips, and a status block:

```markdown
## Status

State: running
Started: 2026-03-25 14:30:00
Completed:
```

Workers update `State:` to `completed`, `failed`, or `escalated` when done. The monitor and `nbs-workers status` both read this field.

**2. Calls launch_agent**

Sources `nbs-launch-agent` and calls the shared function:

```bash
launch_agent "${ROLE}-${SUFFIX}" "${PROJECT_ROOT}" "${NBS_CLAUDE}" \
    "Read ${TASK_FILE} and execute the task. Update the Status and Log sections when complete."
```

The unique handle ensures the nbs-ts session name (`nbs-pythia-a1b2c3d4-poem`) is unique per launch, not per role. Without this, monitors from different launches of the same role would kill each other's sessions.

**3. Starts a monitor subshell**

A backgrounded subshell, stdout/stderr redirected to `/dev/null` so the caller's `$()` capture does not block. The monitor:

- Waits 15 seconds for initialisation
- Discovers the session by grepping `nbs-ts list` for the unique handle name
- Polls every 10 seconds (up to 60 iterations = 10 minutes)
- On each poll: checks session liveness via `nbs-ts status`, reads `State:` from the task file
- Kills the session when state is `completed`, `done`, `failed`, or `escalated`
- Kills the session on timeout (10 minutes)
- Exits silently if the session is already dead

The monitor writes debug output to `/tmp/nbs-spawn-worker-monitor-$$.log` for post-mortem diagnosis.

## nbs-launch-agent

The single function that launches Claude. Shared by the restart script and `nbs-spawn-worker`. One code path. No duplication.

```bash
launch_agent HANDLE PROJECT_ROOT NBS_CLAUDE_PATH INITIAL_PROMPT
```

It does four things:

1. **Unsets `CLAUDECODE` and `TMUX`** — safety net. When a worker is spawned from within a running Claude Code session, `CLAUDECODE=1` leaks into the child. Claude detects this, treats itself as nested, and exits after ~30 seconds. The `unset` prevents this. It is also needed for `TMUX`, which causes similar nesting detection.

2. **Sets three environment variables** as command-line prefix vars: `NBS_HANDLE`, `NBS_TRANSPORT=ts`, `NBS_INITIAL_PROMPT`.

3. **Calls `setsid nbs-claude`** with `--root` and `--dangerously-skip-permissions`, stdout/stderr to `/dev/null`, backgrounded.

4. **Returns immediately.** The caller does not wait.

### HERE BE DRAGONS

Do not change the launch line. The exact incantation — bash `setsid`, prefix env vars, redirect to `/dev/null`, background with `&` — is the only combination that works. Every variation has been tried. Every variation has failed. See "What NOT to do" below.

The `CLAUDECODE` unset is not strictly necessary when the helper runs Claude (the helper's environment is clean). It is strictly necessary when oracles are spawned from within a Claude Code session, which is the common case. It stays.

## What NOT to Do

These are field-tested failures, not theoretical concerns.

| Approach | What happens |
|----------|-------------|
| Launch Claude from C `fork()` + `execl()` | Claude starts, processes a few API calls, exits silently. No error. Reproducible. Root cause unknown. |
| Wrap `nbs-claude` in `nbs-ts create` | Double session. `nbs-claude` creates its own nbs-ts session internally. You get an outer session wrapping an inner session. The outer sidecar loop breaks. |
| Redirect stdin to `/dev/null` | Claude exits immediately. It needs stdin open even though input arrives via the PTY. |
| Use `flock` on pid files | The lock's file descriptor (fd 9) is inherited by child processes. Lock held indefinitely. Future spawns of the same handle block forever. |
| Set env vars with C `putenv()` then `execl()` | Stack-allocated buffers may not survive exec. Even when they do, Claude still exits (Bug 2). |
| Add sleeps or timing assumptions | Race conditions. The system works without them. |
| Inherit `CLAUDECODE` or `TMUX` from parent | Nesting detection. Early exit. The `unset` in `launch_agent` exists for this reason. Do not remove it. |
| Change the `launch_agent` function | Test all oracles (`/pythia`, `/librarian`, `/shepard`, `/fixup`) for 60-second survival after spawn before merging. |

## File Layout

```
bin/nbs-spawn-worker         # The spawn script
bin/nbs-launch-agent         # The shared launch function
.nbs/workers/
├── pythia-a1b2c3d4.md       # Task file
├── pythia-a1b2c3d4.log      # Session output (via nbs-ts)
```

Installed to `~/.nbs/bin/` by `bin/install.sh`.
