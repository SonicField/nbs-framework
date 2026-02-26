---
description: Interact with long-running terminal processes across multiple tool calls
allowed-tools: Bash
---

# Interactive Terminal Sessions (pty-session)

This skill enables interaction with long-running interactive processes like REPLs, debuggers, and CLI tools that require multi-step conversation. It also provides access to the user's login shell environment for operations that require the user's credentials or environment configuration.

---

## The Problem

The Bash tool is one-shot: run a command, get output, done. This prevents:
- Interacting with REPLs (python, node, gdb)
- Controlling processes that require multi-step interaction
- Monitoring long-running processes
- Running commands that need the user's login environment (SSH keys, proxy credentials, authenticated git remotes, corporate tooling)

## The Solution

Use `pty-session` — a tmux-based session manager that allows creating, interacting with, and reading from persistent terminal sessions.

Sessions run under the user's login shell with their full environment, so commands that fail from the Bash tool due to missing credentials or proxy configuration work through pty-session.

---

## Commands

```bash
pty-session create <name> <command>   # Create session running command
pty-session send <name> <text>        # Send keystrokes (adds Enter by default)
pty-session read <name>               # Read output (live, cache, or log)
pty-session wait <name> <pattern>     # Poll until pattern appears
pty-session kill <name>               # Terminate session (screen cached)
pty-session list                      # Show active sessions
pty-session help                      # Show usage
```

### Options

- `--no-enter` with `send`: Don't append Enter after text
- `--timeout=N` with `wait`: Timeout in seconds (default 60)
- `--scrollback=N` with `read`: Lines of history to capture (default 100)
- `--last=N` with `read`: Alias for `--scrollback=N`

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Session not found |
| 3 | Timeout (wait command) |
| 4 | Invalid arguments |

---

## Usage Patterns

### User's Login Shell

When the Bash tool cannot perform an operation because it lacks credentials, proxy configuration, or other environment setup that the user's shell provides, use pty-session to run the command in the user's login shell:

```bash
# Push to a git remote that requires the user's credentials
pty-session create gitpush 'bash --login'
sleep 2
pty-session send gitpush 'git push origin master 2>&1; echo "DONE_EXIT=$?"'
sleep 10
pty-session read gitpush
pty-session kill gitpush
```

This is useful for:
- **Git push/pull** to authenticated remotes
- **Package installation** through corporate proxies
- **SSH commands** that require the user's SSH agent
- **Corporate tooling** that reads credentials from the user's environment

### REPL Interaction

```bash
# Start a Python REPL
pty-session create mypy 'python3'

# Wait for prompt
pty-session wait mypy '>>>'

# Send commands
pty-session send mypy 'x = 42'
pty-session send mypy 'print(x * 2)'

# Read output
pty-session read mypy

# Clean up
pty-session kill mypy
```

### Monitoring Long-Running Process

```bash
# Start build
pty-session create build 'make -j8'

# Check progress periodically
pty-session read build

# Wait for completion
pty-session wait build 'Build complete' --timeout=600

# Clean up
pty-session kill build
```

---

## Persistent Logging

All session output is logged to `~/.pty-session/logs/<name>.log` from the moment of creation. This log survives any exit — including sessions that exit naturally before `kill` is called.

`read` resolves output from three sources in priority order:

1. **Live pane** — if the session is still running, captures the current screen
2. **Cache** — if the session was killed, reads the snapshot taken at kill time (consumed on read)
3. **Persistent log** — if neither live nor cache is available, reads the full log (ANSI-stripped)

This means output is never lost, regardless of how the session ends.

---

## Important Notes

### Session Isolation

- All sessions are prefixed with `pty_` internally
- Sessions created via `pty-session` are isolated from user's tmux sessions
- Running `pty-session list` only shows pty-session sessions
- Running `pty-session kill` cannot affect non-pty-session tmux sessions

### Timing Considerations

- After `send`, the command may not have executed yet
- Use `wait` to block until expected output appears
- Use `read` to capture current state without waiting

### Nested Tmux

- Works correctly when already running inside tmux
- Creates detached sessions that don't interfere with the parent

### Double-Enter Issue

- Some TUI applications need Enter sent twice
- If a command doesn't execute, try: `pty-session send name '' ` (send empty string to trigger extra Enter)

---

## When to Use This

- Running commands that need the user's login environment (git push, SSH, corporate tools)
- Interacting with REPLs (Python, Node, GDB, psql)
- Automating multi-step terminal workflows
- Running processes that need monitoring and intervention
- Any situation where you need to send input and read output across multiple tool calls

**For managing Claude worker instances**, use `nbs-workers` instead (see `/nbs-tmux-worker`).

---

## Remote File Editing (nbs-remote-edit)

When editing files on remote machines (e.g. devserver), use `nbs-remote-edit` instead of sed/heredoc commands through pty-session. This downloads the file locally, lets you use the normal Edit tool, then pushes it back.

### Commands

```bash
nbs-remote-edit pull <host> <remote-path>   # Download file for local editing
nbs-remote-edit push <host> <remote-path>   # Upload edited file back
nbs-remote-edit diff <host> <remote-path>   # Show diff between local and remote
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NBS_REMOTE_EDIT_DIR` | `.nbs/remote-edit` | Local staging directory |
| `NBS_REMOTE_EDIT_KEY` | — | SSH identity file |
| `NBS_REMOTE_EDIT_PORT` | 22 | SSH port |

### Workflow

```bash
# 1. Download the file
nbs-remote-edit pull devserver.example.com /data/users/dev/project/Jit/pyjit.cpp
# Returns: .nbs/remote-edit/devserver.example.com/data/users/dev/project/Jit/pyjit.cpp

# 2. Edit locally using the normal Edit tool (no sed needed!)

# 3. Check what changed
nbs-remote-edit diff devserver.example.com /data/users/dev/project/Jit/pyjit.cpp

# 4. Push back
nbs-remote-edit push devserver.example.com /data/users/dev/project/Jit/pyjit.cpp
```

### When to Use

- Editing C/C++ source files on remote build machines
- Any multi-line file edit that would be error-prone via sed/heredoc
- When you need to see the full file context before editing

**For single commands on remote machines**, use `pty-session` instead — it's simpler for command execution.

---

## Chat-Aware Builds (nbs-remote-build)

When building on a remote machine via pty-session, use `nbs-remote-build` to stay responsive to chat messages during builds. Instead of blocking on `sleep 120`, it polls for build completion and checks chat between polls.

### Usage

```bash
# Basic: run build, wait for prompt to reappear
nbs-remote-build my-server 'cmake --build build -j8'

# Chat-aware: check chat while building
nbs-remote-build my-server 'cmake --build build -j8' \
    --chat=.nbs/chat/live.chat --handle=claude

# Custom prompt pattern (e.g. venv prompt)
nbs-remote-build my-server 'make -j8' --prompt='(venv)'
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--prompt=PATTERN` | `\$ *>?` | Regex to detect shell prompt (build done) |
| `--timeout=N` | 300 | Build timeout in seconds |
| `--poll=N` | 5 | Poll interval in seconds |
| `--chat=FILE` | — | Chat file to check between polls |
| `--handle=NAME` | — | Chat handle for unread messages |
| `--quiet` | — | Suppress progress dots |

### When to Use

- Building CinderX or other large C++ projects on devserver
- Any pty-session command where you'd otherwise use `sleep N`
- When you need to stay responsive to team chat during long operations

---

## Location

The `pty-session` script is at: `{{NBS_ROOT}}/bin/pty-session`

Ensure it's in your PATH or use the full path.
