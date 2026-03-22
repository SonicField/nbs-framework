# NBS Terminal Service: Replacing tmux for Local AI Agent Sessions

**Date:** 2026-03-22
**Author:** Alex Turner
**Status:** Design document — not yet approved for implementation

## 0. What is nbs-ts?

`nbs-ts` (NBS Terminal Service) is a C library and CLI tool for running commands in managed terminal sessions. It replaces tmux for local process management in the NBS framework.

### What it does

You give it a command. It runs that command in a pseudo-terminal. It captures all output to a log file on disk. It tells you when the command finishes, what the exit code was, and whether the process is still alive. When you're done, you kill the session and everything is cleaned up.

```bash
# Run a command, get a handle back
HANDLE=$(nbs-ts create 'bash')

# Send input (written directly to stdin — no keystroke simulation)
nbs-ts send "$HANDLE" 'echo hello world'

# Wait for the command to complete
nbs-ts wait-complete "$HANDLE" --timeout=10

# Read the output (from an append-only log file — never lost, never truncated)
nbs-ts read-new "$HANDLE"
# → hello world

# Check the exit code
nbs-ts exit-code "$HANDLE"
# → 0

# Kill when done (automatic cleanup — no zombie sessions)
nbs-ts kill "$HANDLE"
```

For SSH, wrap the ssh command in a session. The remote side still uses pattern matching for completion detection (same as today), but the local transport is clean:

```bash
HANDLE=$(nbs-ts create 'ssh user@build-server')
nbs-ts send "$HANDLE" 'cd /project && make -j8'
nbs-ts wait-pattern "$HANDLE" '\$' --timeout=300
nbs-ts read-new "$HANDLE"
nbs-ts kill "$HANDLE"
```

### How it works internally

Each session is a directory on disk:

```
~/.nbs-ts/sessions/<handle>/
├── output.log        # Append-only. Every byte of stdout+stderr. Never truncated.
├── completion.log    # One line per command: sequence number + exit code.
├── heartbeat         # Touched periodically. mtime = liveness.
├── pid               # Shell process ID.
└── meta              # Command, cwd, start time.
```

**Output capture**: A thread reads the PTY master file descriptor and appends to `output.log`. Every write is `fsync`'d so `inotify` watchers fire immediately.

**Completion signalling**: On session start, nbs-ts injects `PROMPT_COMMAND` into bash. After every command, bash writes the exit code to `completion.log`. The `wait-complete` operation watches this file with `inotify` — no polling, sub-millisecond latency.

**Liveness**: `waitpid(WNOHANG)` checks if the shell process is alive. The heartbeat file is touched periodically as a secondary signal. No pane scraping, no prompt parsing.

**Cleanup**: When `kill` is called or the owning process exits, the shell is terminated via `SIGTERM`, the PTY is closed, and the session directory is removed. No zombie sessions.

### What it is NOT

- **Not a terminal multiplexer.** No windows, no panes, no split screens. Each session is one command in one terminal.
- **Not a daemon.** No server process. Sessions are files and processes managed directly.
- **Not a remote execution framework.** SSH sessions work by wrapping the `ssh` command in a local PTY. The remote side is unmanaged — same as today.
- **Not a replacement for the human terminal.** Humans can watch agents with `nbs-ts attach <handle>` (tails the output log), but the primary interface is programmatic.

### The C library

For the sidecar and other C code that currently calls `fork+exec tmux capture-pane`, nbs-ts provides in-process calls:

```c
nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
nbs_ts_send(s, "echo hello\n", 11);

nbs_ts_completion_t c;
nbs_ts_wait_complete(s, 5000, &c);  /* 5 second timeout */
printf("exit code: %d\n", c.exit_code);

char buf[4096];
size_t n = nbs_ts_read_new(s, buf, sizeof(buf));
/* buf contains "hello\n" */

nbs_ts_destroy(s);
```

No fork+exec per operation. No parsing tmux output. No polling. The sidecar's 1Hz capture-pane polling loop becomes an inotify wait that fires when output arrives.

### ANSI escape handling

The output log stores raw bytes — ANSI colour codes, cursor movement, everything. This is necessary for `nbs-ts attach` to render correctly for human viewers.

But AI agents don't want escape codes. They're parsing text, not rendering terminals. So `read-new` and `read` support a `--strip` flag:

```bash
nbs-ts read-new "$HANDLE"          # Raw: includes \033[32mgreen\033[0m
nbs-ts read-new "$HANDLE" --strip  # Stripped: includes green
```

The C library provides both forms:

```c
size_t nbs_ts_read_new(session, buf, len);           /* Raw */
size_t nbs_ts_read_new_stripped(session, buf, len);   /* ANSI stripped */
```

The strip implementation reuses `strip_ansi.c` from the existing sidecar codebase — already written, tested, and handles all standard SGR, CSI, and OSC sequences.

### Implementation language: C

nbs-ts is written in C11, same as every other NBS tool. The reasons:

- **The sidecar links against it.** The sidecar is C. The transport vtable is C function pointers. C-to-C linkage is a header include. C-to-anything-else is FFI.
- **AI agents read the source.** C is transparent — no hidden constructors, no implicit conversions, no template instantiation. An agent can read the entire tool in one pass and understand every code path.
- **Assertions as the type system.** `ASSERT_MSG` at boundaries, `-Wall -Wextra -Wshadow -Werror`, `assert_check` test binary. Same discipline as nbs-chat, nbs-bus, nbs-sidecar.
- **No runtime dependencies.** Pure POSIX. No allocator, no garbage collector, no standard library surprises.

Build: `gcc -Wall -Wextra -Wshadow -Werror -std=c11 -O2`

### Key properties

| Property | How |
|----------|-----|
| **Output never lost** | Append-only file, fsync'd after every write |
| **Completion signalled, not polled** | PROMPT_COMMAND writes to completion.log, inotify wakes waiters |
| **No keystroke simulation** | Direct write(2) to PTY master fd |
| **No shared sessions** | Each session has its own output log. No interleaving possible. |
| **No zombies** | Cleanup on kill, on owning process exit, and on explicit destroy |
| **No server** | Sessions are files and processes. Nothing to crash, nothing to restart. |
| **Sub-millisecond wait** | inotify instead of 100ms poll intervals |
| **Clean output for AI** | `--strip` flag removes ANSI escapes. Raw preserved for human viewing. |
| **Written in C11** | Same toolchain, same style, same assertions as all NBS tools. |

## 1. Why Replace tmux?

The NBS framework uses tmux as its process isolation and terminal access layer. Every agent runs in a tmux session. Every remote command goes through `send-keys` and `capture-pane`. The sidecar monitors agents by polling tmux panes. Workers are spawned as tmux sessions.

tmux was designed for humans who want persistent terminal sessions. AI agents need programmatic command execution with reliable output capture and explicit completion signalling. The mismatch costs the NBS codebase ~2,000 lines of workaround code and has caused 15 documented incidents.

This plan addresses **local sessions only** — where agents, workers, and sidecars run. SSH sessions continue to fork+exec `ssh` and talk to it through a PTY, same as today but without tmux in the middle. Local is where 90% of the team's work happens and where the wins are largest.

## 2. What We Lose (tmux workaround code)

| Workaround | Lines | Purpose | Eliminated by |
|------------|-------|---------|---------------|
| Fence marker protocol | ~60 | UUID generation, fence files, pattern-after-fence scan | Direct completion signalling |
| Polling loops | ~100 | fork+exec `tmux capture-pane` at 100ms intervals | inotify on output log |
| Capture buffer management | ~80 | 32KB/256KB buffers, caching layer, truncation handling | Append-only log file |
| Pane ID validation | ~30 | Validate `%[0-9]+` format, reject injection | No pane IDs (opaque handles) |
| TMUX env var hack | ~10 | `unsetenv("TMUX")` to allow nesting | No server, no nesting issue |
| Session naming convention | ~40 | `pty_` prefix, collision avoidance, name derivation | Opaque handles, auto-generated |
| Advisory locking | ~150 | pty-session-lock, flock, race prevention | Structural isolation (no shared sessions) |
| Keystroke simulation delays | ~20 | 100ms sleep between send-keys and Enter | Direct write to PTY master fd |
| Pane scraping for liveness | ~80 | capture-pane + hash + regex for prompt detection | waitpid + heartbeat file |
| **Total** | **~570** | | |

Plus ~1,400 lines in pty-session itself that wraps tmux.

## 3. What We Get

### 3.1 For agents and workers (local)

| Before (tmux) | After (nbs-ts) |
|---------------|----------------|
| Output in circular pane buffer, 32KB limit, data loss | Append-only log file, no limit, no loss |
| `send-keys -l` keystroke simulation + 100ms sleep | `write(master_fd)` direct, immediate |
| Poll capture-pane 3,000 times per 5-min wait | `inotify` on log file, one syscall per event |
| Fence markers for command tracking | Completion records on separate channel |
| `has-session` for liveness | `waitpid(WNOHANG)` + heartbeat mtime |
| Session naming collisions, zombies | Opaque handles, auto-cleanup on process exit |
| Advisory locking for exclusive access | Nothing to lock — each session is isolated |

### 3.2 For SSH (unchanged)

SSH sessions continue to:
- Fork+exec `ssh` and connect it to a PTY
- Use pattern matching for prompt/completion detection
- Merge stdout/stderr in the PTY stream

The only improvement: no tmux server in the middle of the local PTY ↔ SSH connection. Marginally faster, same reliability. SSH completion signalling is a future project, not this one.

### 3.3 For humans

Humans currently watch agents via `tmux attach-session`. Replacement: `nbs-ts attach <handle>` tails the output log with ANSI pass-through. Functionally identical for watching. No window/pane switching (agents don't use multiple panes anyway).

## 4. Architecture

### 4.1 The Session

A session is:
- A PTY pair: master fd (for I/O) + slave fd (connected to the shell)
- An output log file: `~/.nbs-ts/sessions/<id>/output.log` (append-only)
- A completion log: `~/.nbs-ts/sessions/<id>/completion.log` (structured records)
- A heartbeat file: `~/.nbs-ts/sessions/<id>/heartbeat` (mtime-based)
- A PID file: `~/.nbs-ts/sessions/<id>/pid`
- A metadata file: `~/.nbs-ts/sessions/<id>/meta` (command, cwd, start time)

No daemon. No server. No global state.

### 4.2 Output Capture

A dedicated thread reads the PTY master fd and appends to the output log:

```c
while ((n = read(master_fd, buf, sizeof(buf))) > 0) {
    write(log_fd, buf, n);
    fsync(log_fd);  /* inotify fires on fsync */
}
```

The output log is the authority. If a human attaches, they tail this file.

### 4.3 Completion Signalling

On session creation, nbs-ts injects a `PROMPT_COMMAND` into the shell:

```bash
PROMPT_COMMAND='echo "$NBS_TS_CMD_SEQ $?" >> '"$NBS_TS_COMPLETION_LOG"
```

Every time bash executes a command, it appends the sequence number and exit code to the completion log. `wait_complete()` watches this file with `inotify`.

### 4.4 CLI

```bash
# Create
HANDLE=$(nbs-ts create 'bash')
HANDLE=$(nbs-ts create 'ssh user@host')   # SSH: just a PTY around ssh

# Send
nbs-ts send "$HANDLE" 'make -j8'

# Wait for completion
nbs-ts wait-complete "$HANDLE" --timeout=300
echo "Exit: $(nbs-ts exit-code "$HANDLE")"

# Read output
nbs-ts read-new "$HANDLE"              # Since last read
nbs-ts read "$HANDLE" --offset=1000    # From byte offset

# Status
nbs-ts status "$HANDLE"    # alive / dead / exit_code

# Attach (human viewer)
nbs-ts attach "$HANDLE"    # tail -f output.log with ANSI

# Kill
nbs-ts kill "$HANDLE"

# List
nbs-ts list
```

### 4.5 C Library API

```c
nbs_ts_session_t *nbs_ts_create(const char *command, const nbs_ts_opts_t *opts);
void              nbs_ts_destroy(nbs_ts_session_t *s);
int               nbs_ts_send(nbs_ts_session_t *s, const char *data, size_t len);
size_t            nbs_ts_read_new(nbs_ts_session_t *s, char *buf, size_t max_len);
size_t            nbs_ts_read(nbs_ts_session_t *s, char *buf, size_t len, off_t offset);
int               nbs_ts_wait_complete(nbs_ts_session_t *s, int timeout_ms, nbs_ts_completion_t *out);
int               nbs_ts_wait_pattern(nbs_ts_session_t *s, const char *pattern, int timeout_ms);
nbs_ts_status_t   nbs_ts_status(nbs_ts_session_t *s);
```

In-process calls. No fork+exec per operation.

## 5. Implementation Phases

### Phase 1: Core Library (1-2 sessions)

Build `src/nbs-ts/`:

| File | Purpose | Lines (est) |
|------|---------|-------------|
| `session.c` | PTY creation, fork+exec, log files, cleanup | 400 |
| `io.c` | send (write to master), read (pread from log) | 200 |
| `wait.c` | inotify-based wait_pattern and wait_complete | 300 |
| `completion.c` | PROMPT_COMMAND injection, record parsing | 150 |
| `status.c` | Process status, heartbeat, exit code | 100 |
| `nbs_ts.h` | Public API header | 80 |
| `main.c` | CLI wrapper (create/send/read/wait/kill/list/attach) | 400 |

**Total**: ~1,630 lines of C.

**Verification**:
- `nbs-ts create 'echo hello'` + `nbs-ts read-new` → "hello"
- `nbs-ts create 'exit 42'` + `nbs-ts wait-complete` → exit code 42
- `nbs-ts create 'sleep 2; echo done'` + `nbs-ts wait-pattern done --timeout=5` → found
- 50 concurrent sessions: no data loss, no zombies, all cleaned up
- Output log survives session kill (data persisted)

### Phase 2: Sidecar Transport (1 session)

Add `src/nbs-sidecar/transport_ts.c` implementing existing vtable:

| Function | tmux version | nbs-ts version |
|----------|-------------|----------------|
| `read_content()` | fork+exec `tmux capture-pane` | `nbs_ts_read_new()` |
| `send_text()` | fork+exec `tmux send-keys` | `nbs_ts_send()` |
| `send_key()` | fork+exec `tmux send-keys Enter` | `nbs_ts_send("\n")` |
| `is_alive()` | fork+exec `tmux list-panes` | `nbs_ts_status()` |

Add `--transport=ts` flag to sidecar. Existing `--transport=tmux` and `--transport=pty` unchanged.

**Verification**:
- Launch one agent with `--transport=ts`, others with tmux
- ts-agent receives notifications, responds to chat, fires skills
- Sidecar log shows no tmux commands for ts-transport agent

### Phase 3: Agent Launcher + Workers (1 session)

Update `bin/nbs-claude`:
- New code path: `NBS_TRANSPORT=ts` → use `nbs-ts create` instead of tmux
- Pass `--transport=ts` to sidecar
- Human attach: `nbs-ts attach` instead of `tmux attach-session`

Update `bin/nbs-spawn-worker`:
- Replace `tmux new-session` with `nbs-ts create`
- Replace `tmux has-session` poll loop with `nbs-ts wait-complete`
- Replace `tmux kill-session` with `nbs-ts kill`
- Replace `tmux pipe-pane` with nothing (output logging is automatic)

**Verification**:
- Full team launch (6 agents) via nbs-ts
- Workers spawn and complete via nbs-ts
- `nbs-ts list` shows all sessions
- Kill an agent: `nbs-ts kill` → session gone, no zombie

### Phase 4: Remote Tools (1 session)

Rewrite `bin/nbs-remote-run`:
- `nbs-ts create 'ssh host'` instead of pty-session create + SSH
- `nbs-ts send` instead of pty-session send
- `nbs-ts wait-pattern` instead of pty-session wait (still pattern matching for SSH — no change in reliability, just cleaner transport)
- `nbs-ts read-new` instead of pty-session read + sed/grep marker extraction
- `nbs-ts kill` instead of pty-session kill

Rewrite `bin/nbs-remote-session`:
- Same pattern, persistent instead of ephemeral

**Verification**:
- `nbs-remote-run host 'hostname'` → clean output, no fence markers
- `nbs-remote-session host --name=work` → handle, send/read works
- Long-running remote commands: output not lost

### Phase 5: Restart, Watchdog, Cleanup (1 session)

Update `bin/nbs-chat-terminal-restart.sh`:
- Kill agents: `nbs-ts kill` by handle (from `nbs-ts list --filter=nbs-`)
- Spawn agents: `nbs-ts create` with nbs-claude command
- Skill injection: `nbs-ts send` (no keystroke simulation delay)

Update `src/nbs-chat/watchdog.c`:
- Count live agents: `nbs-ts list` count instead of `tmux list-sessions`
- Liveness: `nbs-ts status` instead of pane capture

**Verification**:
- `/restart` kills and relaunches all agents via nbs-ts
- Watchdog detects team death via nbs-ts, triggers restart
- `--goal-file` + `--restart` workflow works end-to-end

### Phase 6: Remove tmux (1 session)

- Delete `src/nbs-sidecar/transport_tmux.c`
- Delete `src/nbs-pty-session/` (entire directory)
- Delete `bin/pty-session-lock` (no shared sessions to lock)
- Remove `pty_` prefix convention, fence marker code, TMUX env var hack
- Update `docs/tools.md`: replace pty-session references with nbs-ts
- Update nbs-remote-dev skill

**Verification**:
- `grep -r 'tmux\|pty-session\|capture-pane\|send-keys' src/ bin/` → zero results
- Full test suite passes
- Full benchmark suite runs via nbs-remote-run (SSH through nbs-ts)
- Team operates for 1 hour without tmux installed

## 6. What We Deliberately Leave Alone

- **SSH completion signalling**: still pattern matching. Future project.
- **SSH output parsing**: still markers. Works, no worse than today.
- **Remote liveness**: still "did the ssh process die." Adequate.
- **Human TUI viewer**: `nbs-ts attach` is just `tail -f`. A proper TUI is future work.
- **Window/pane multiplexing**: agents don't use it. Not implemented.

## 7. Falsification

The replacement is wrong if:
- Any of the 15 documented tmux incidents can recur
- Output is ever lost or truncated (the primary win)
- Wait latency exceeds 10ms for local sessions
- Agent startup is slower than with tmux
- The test suite passes with tmux but fails with nbs-ts
- `nbs-ts attach` is functionally worse than `tmux attach-session` for watching agents
