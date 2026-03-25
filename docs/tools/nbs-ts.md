# nbs-ts and nbs-ts-helper

Session management for NBS. `nbs-ts` creates and controls PTY-backed sessions. `nbs-ts-helper` allocates the PTYs and runs the processes.

They are separate binaries because they solve separate problems. The CLI is stateless — fire and forget. The helper is a long-running daemon that owns process lifetimes and provides the caller's login environment to spawned children.

## nbs-ts (the CLI)

### Commands

| Command | Usage | Description |
|---------|-------|-------------|
| `create` | `nbs-ts create [--name=NAME] <command>` | Create a session. Prints the 8-char hex handle to stdout. |
| `send` | `nbs-ts send <handle> <text>` | Write text to the session's input FIFO. Appends `\r` automatically. |
| `read-new` | `nbs-ts read-new <handle> [--strip]` | Read output since last `read-new`. `--strip` removes ANSI escapes. |
| `read` | `nbs-ts read <handle> [--offset=N\|--last=N]` | Read from byte offset, or last N lines. Mutually exclusive. |
| `wait-complete` | `nbs-ts wait-complete <handle> [--timeout=N]` | Wait for the next PROMPT_COMMAND to fire (default 60s). Prints exit code. Does not detect `exit`/logout — use `exit-code` for that. |
| `wait-pattern` | `nbs-ts wait-pattern <handle> <pattern> [--timeout=N]` | Wait for a substring in output.log (default 60s). Uses inotify. |
| `status` | `nbs-ts status <handle>` | Prints `alive` or `dead`. If dead, prints `exit_code: N`. |
| `exit-code` | `nbs-ts exit-code <handle>` | Prints exit code, or `-1` if not yet exited. |
| `kill` | `nbs-ts kill <handle>` | SIGTERM the process group, then SIGKILL if it survives 100ms. Removes session directory. |
| `list` | `nbs-ts list [--name=PATTERN]` | List sessions. `--name` filters by substring match. Tab-separated: handle, alive/dead, name, command. |
| `find` | `nbs-ts find <name>` | Find session by exact name. Prints handle. Exit 2 if not found. |
| `attach` | `nbs-ts attach <handle>` | `tail -f` on the output log. For humans watching a session live. |
| `gc` | `nbs-ts gc [--hours=N]` | Remove dead sessions older than N hours (default 4). Skips alive sessions. |

### Exit codes

`0` success, `1` error, `2` not found, `3` timeout, `4` bad arguments.

### Named sessions

Sessions are identified by 8-character hex handles (random, from `/dev/urandom`). Names are optional metadata:

- `--name=NAME` on `create` writes the name to a file in the session directory.
- `--name=PATTERN` on `list` filters by substring match against session names.
- `find <name>` does exact match and returns the handle.

Names must match `[a-zA-Z0-9_.-]`, maximum 64 characters.

### Session directory layout

Each session lives at `~/.nbs-ts/sessions/<handle>/` and contains:

| File | Content |
|------|---------|
| `output.log` | All PTY output, appended by the daemon relay loop with fsync after each write. |
| `input.fifo` | Named pipe. `send` writes here; the daemon forwards to the PTY master fd. |
| `pid` | PID of the child process (the thing running your command). |
| `daemon_pid` | PID of the relay daemon. |
| `meta` | Two lines: `command: <cmd>` and `start: <unix_timestamp>`. |
| `name` | Session name, if one was given. |
| `completion.log` | Lines of `<seq> <exit_code>`, written by PROMPT_COMMAND after each command. |
| `bashrc` | Generated rcfile that sets up PROMPT_COMMAND for completion tracking. |
| `slave_pty` | Path to the PTY slave device (empty string when using helper mode). |
| `exit_code` | Written by the daemon when the child exits. |
| `read_cursor` | Byte offset for `read-new` cursor tracking. |
| `completion_cursor` | Sequence number for `wait-complete` cursor tracking. |

### The meta file

```
command: bash -c "make -j8 && make test"
start: 1711324800
```

Two fields, one per line. The start timestamp is `time(NULL)` at session creation. Used by `gc` to determine session age.

### Completion tracking

Interactive sessions use `PROMPT_COMMAND` to append `<seq> <exit_code>` to `completion.log` after every command. The sequence number increments from 0. `wait-complete` watches this file with inotify and returns when a new sequence number appears beyond the stored cursor.

Non-interactive sessions (one-shot commands) use a `trap EXIT` instead, since `PROMPT_COMMAND` does not fire in non-interactive bash. The trap writes `0 $?` to `completion.log` on exit.

### The relay daemon

`nbs-ts create` forks a daemon process that outlives the CLI. The daemon runs a `poll(2)` loop with two file descriptors:

- **PTY master fd** — reads output, appends to `output.log` with fsync.
- **input.fifo** — reads input from `send` callers, writes to PTY master fd.

The FIFO is opened `O_RDWR` to prevent `POLLHUP` when the last writer closes. The daemon exits when the PTY master returns EOF (child process died).

## nbs-ts-helper (the daemon)

### What it is

A centralised PTY allocator. It listens on a Unix domain socket, and for each connection it allocates a PTY, forks a child, and sends the master fd back to the caller via `SCM_RIGHTS`.

### Why it exists

The child process runs as a descendant of the helper, not of whoever called `nbs-ts create`. This is the entire point. The helper runs in the user's login shell, started from a real terminal. Its children inherit that environment — SSH keys, proxy credentials, authenticated sessions.

The caller's environment does NOT reach the child. Claude's sandbox environment, the cron environment, whatever called `nbs-ts create` — none of that propagates. The child gets `bash --login -c "..."` under the helper's process tree.

This is critical. Without the helper, `git push`, `ssh`, and anything requiring credentials will fail silently or hang.

### Starting it

```bash
bin/nbs-ts-helper    # runs in foreground, logs to stdout, Ctrl-C to stop
```

Must be running before any sessions are created. If the helper is not running, `nbs-ts create` falls back to direct `openpty+fork` with a warning. The fallback works but lacks the login environment.

### Socket protocol

Listens on `~/.nbs-ts/helper.sock` (Unix stream socket, mode 0600).

For each connection:

1. **Verify peer credentials** — `SO_PEERCRED`. Rejects connections from different UIDs.
2. **Read command** — reads until EOF (caller closes write end after sending). Max 4096 bytes.
3. **Allocate PTY** — `openpty()` with 24x80 terminal size.
4. **Fork child** — `setsid()`, set controlling terminal, redirect stdin/stdout/stderr to slave, `execlp("bash", "bash", "--login", "-c", cmd)`. Resets `SIGCHLD` to `SIG_DFL` so the child's own `waitpid` calls work.
5. **Send master fd** — `SCM_RIGHTS` ancillary message over the socket.
6. **Send child PID** — plain integer string after the fd.

### Process ownership

The helper reaps its children via `waitpid(WNOHANG)` in the main loop. It logs every child death with exit code or signal number. This audit trail is the only record when something dies unexpectedly.

The daemon forked by `nbs-ts create` cannot `waitpid` on helper-spawned children (they are not its descendants). Instead, it reads the exit code from `completion.log`, which the child's bash trap writes on exit.

### Lifecycle

Runs until `SIGINT` or `SIGTERM`. On shutdown it removes the socket file and logs a stop message. Uses `select()` with a 1-second timeout in the main loop so it can check the quit flag and reap children periodically.

### After rebuilding

If you rebuild `nbs-ts-helper` with `make install`, you must kill and restart the running helper process. The old binary is still in memory until you do.
