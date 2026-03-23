# Plan: nbs-ts-helper — Centralised Process Launcher for nbs-ts

**Date:** 2026-03-23

## What

`nbs-ts-helper` is a long-running process launcher that spawns PTY sessions on behalf of `nbs-ts`. It provides centralised PTY allocation, process-group management, and audit logging. All `nbs-ts create` calls route through it.

## Why

nbs-ts creates sessions by forking directly from the calling process. This means the child inherits the caller's process context. A centralised launcher provides:

1. **Consistent process context** — all sessions are children of the same long-lived process, regardless of which tool or agent requested them.
2. **Audit logging** — every session creation is logged to stdout with timestamp, command, and PID.
3. **Process-group management** — the launcher is the parent of all sessions, enabling clean group-based shutdown.
4. **Centralised PTY allocation** — all PTYs are allocated by one process, simplifying lifecycle tracking.

## Architecture

```
nbs-ts-helper (long-running, user-level process)
  ├── listens on ~/.nbs-ts/helper.sock (Unix domain socket)
  ├── accepts connections, reads command
  ├── openpty + fork + exec for each request
  ├── passes PTY master fd back to caller via SCM_RIGHTS
  └── logs all activity to stdout

nbs-ts create <command>
  ├── connect to ~/.nbs-ts/helper.sock
  ├── send command
  ├── receive PTY master fd via SCM_RIGHTS
  ├── proceed with output logging, completion signalling, etc.
  └── fallback: if helper not running, direct openpty+fork (reduced capabilities)
```

The caller receives the PTY master fd and owns all I/O from that point. The helper has no ongoing involvement in the session — it forks, passes the fd, and moves on.

## Security

1. **Socket permissions** — `~/.nbs-ts/` directory created with mode 0700. Socket created with mode 0600. Only the owning user can connect.
2. **Peer credential check** — on every accepted connection, `SO_PEERCRED` verifies the peer's uid matches `getuid()`. Reject and log if mismatched.
3. **No privilege escalation** — the helper runs as the same user who starts it. It creates processes with the user's own credentials. It does not setuid, setgid, or modify capabilities.
4. **Audit trail** — every spawn is logged with timestamp, command, child PID, and peer PID.

## Interface

### nbs-ts-helper (the launcher)

```bash
nbs-ts-helper              # Run in foreground, log to stdout
nbs-ts-helper &            # Background
```

Logs to stdout:
```
[2026-03-23T10:00:00Z] nbs-ts-helper started (pid 12345)
[2026-03-23T10:00:00Z] Listening on /home/alex/.nbs-ts/helper.sock
[2026-03-23T10:00:05Z] spawn: "bash" → pid 12346 (peer pid 12400)
[2026-03-23T10:00:10Z] spawn: "ssh host" → pid 12347 (peer pid 12401)
[2026-03-23T10:01:00Z] spawn: "make -j8" → pid 12348 (peer pid 12402)
```

Exits on: SIGTERM, SIGINT (Ctrl-C). Cleans up socket on exit.

### nbs-ts changes (in session.c)

On `nbs_ts_create()`:

1. Try `connect()` to `~/.nbs-ts/helper.sock`
2. If connected: send command, receive fd via `SCM_RIGHTS`, use it
3. If not connected: fall back to direct `openpty + fork + exec` (current code)
4. In fallback mode: print once to stderr: `"nbs-ts-helper not running — sessions will use direct fork (some operations may be restricted)"`

### Startup integration

Tools that interact with humans check for the helper on startup:

**nbs-chat-terminal**: On startup, check if `~/.nbs-ts/helper.sock` exists and is connectable. If not, print warning:
```
Warning: nbs-ts-helper is not running. Start it with: nbs-ts-helper
Some operations (SSH, proxy access) may not work without it.
```

**nbs-chat-init**: Same check, same warning.

These tools do NOT start the helper automatically — the user decides when and where to start it.

## Implementation

### File: `src/nbs-ts/helper.c` (~200 lines)

```
main()
  - create ~/.nbs-ts/ directory (mode 0700) if missing
  - unlink old socket if exists
  - bind Unix socket (mode 0600)
  - signal handlers: SIGTERM/SIGINT → cleanup socket, exit
  - loop:
    - accept connection
    - SO_PEERCRED check → reject if uid mismatch
    - read command (max 4096 bytes, NUL-terminated)
    - openpty() → master_fd, slave_fd
    - fork()
      child: setsid, dup2 slave to 0/1/2, exec bash -c <command>
      parent: send master_fd via SCM_RIGHTS, close master_fd, log
```

### File: `src/nbs-ts/helper_client.c` (~80 lines)

Shared code for connecting to the helper and receiving an fd:

```c
/* Returns master_fd on success, -1 if helper not running */
int helper_request_pty(const char *command);
```

Used by `session.c` in `nbs_ts_create()`.

### File: `src/nbs-ts/session.c` (~30 lines changed)

Replace the `openpty + fork + exec` block with:

```c
int master_fd = helper_request_pty(command);
if (master_fd < 0) {
    /* Fallback: direct fork (helper not running) */
    /* ... existing code ... */
}
```

### File: `src/nbs-ts/Makefile`

Add `helper.c` and `helper_client.c` to the build. New binary: `nbs-ts-helper`.

### File: `src/nbs-chat/terminal.c` (~10 lines)

After project root resolution, before interactive loop:

```c
/* Check nbs-ts-helper */
char sock_path[8192];
snprintf(sock_path, sizeof(sock_path), "%s/.nbs-ts/helper.sock", getenv("HOME"));
struct stat st;
if (stat(sock_path, &st) != 0) {
    printf("%sWarning: nbs-ts-helper is not running. "
           "Start it with: nbs-ts-helper%s\n", DIM, RESET);
}
```

## Testing

### Unit tests

1. **Helper starts and listens** — start helper, verify socket exists
2. **Local command via helper** — `echo hello` through helper, verify output
3. **Peer credential rejection** — connect from a different uid (if testable), verify rejection logged
4. **Multiple concurrent requests** — 10 simultaneous creates, all succeed
5. **Helper not running fallback** — `nbs-ts create` without helper, local command works
6. **Helper not running warning** — verify stderr message appears once
7. **Socket cleanup on exit** — send SIGTERM, verify socket removed

### Integration test

1. Start `nbs-ts-helper` in background
2. `nbs-ts create 'bash'` → returns handle
3. `nbs-ts send <handle> 'echo test'`
4. `nbs-ts read-new <handle>` → contains "test"
5. `nbs-ts kill <handle>`
6. Kill helper
7. Repeat steps 2-5 without helper — local commands work, verify fallback message

## Verification

The helper is correct if:
- All existing nbs-ts tests pass (helper running or not)
- `nbs-remote-run <host> 'hostname'` works when helper is running
- `nbs-remote-run <host> 'hostname'` fails with clear error when helper is not running
- Socket permissions are 0600, directory 0700
- `SO_PEERCRED` check logged on every connection
- Every spawn logged with timestamp, command, PIDs
- SIGTERM cleanup removes socket

## Falsification

The helper adds no value if:
- Direct fork works for all operations (no process context restrictions)
- Or: the fallback path handles all failure modes with clear errors

The helper is wrong if:
- It introduces a single point of failure (sessions die when helper dies)
  → Sessions are independent processes; helper only forks them. After fork, helper is not involved.
- It introduces security risk beyond the user's own permissions
  → SO_PEERCRED + socket permissions. No privilege escalation.
