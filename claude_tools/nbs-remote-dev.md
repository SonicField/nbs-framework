---
description: Remote development workflow for editing, building, and debugging on remote machines
allowed-tools: Bash
---

# Remote Development Workflow

Tools and patterns for editing files, running builds, and debugging on remote machines from an AI agent. Consolidated from 1000+ messages of real team experience.

---

## The Problem

AI agents run on machines that may not have direct SSH access to development servers. Remote work flows through `nbs-ts` sessions, which creates specific failure modes:

1. **Sed/heredoc corruption**: Using `sed` or heredocs through terminal sessions to edit C/C++ files causes cascading corruption — duplicate lines, broken syntax, missing struct fields. This was the single largest source of wasted time.
2. **Build blindness**: Agents go silent for 10-20 minutes during builds because they block on `sleep 120`. The team assumes the agent has stopped.
3. **Session collisions**: Two agents sending commands to the same session simultaneously corrupt each other's output.
4. **No file coordination**: Multiple agents can edit the same remote file without knowing.

---

## Tools

Three tools address these problems. All are in `~/.nbs/bin/`.

### nbs-remote-edit — Safe Remote File Editing

Pull a remote file to a local staging area, edit with the normal Edit tool (full undo, syntax awareness, no corruption), then push back.

```bash
# 1. Download the file
nbs-remote-edit pull <host> <remote-path>
# Returns: .nbs/remote-edit/<host>/<remote-path>

# 2. Edit locally using the Edit tool — no sed, no heredocs

# 3. Verify your changes
nbs-remote-edit diff <host> <remote-path>

# 4. Push back
nbs-remote-edit push <host> <remote-path>
```

**Environment variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `NBS_REMOTE_EDIT_DIR` | `.nbs/remote-edit` | Local staging directory |
| `NBS_REMOTE_EDIT_KEY` | — | SSH identity file |
| `NBS_REMOTE_EDIT_PORT` | 22 | SSH port |

**Exit codes:** 0=success, 2=file not found, 3=SSH failed, 4=bad arguments.

**Uses `ssh cat` internally** (not scp/sftp) to work in environments where the sandbox blocks the SFTP subsystem. If the sandbox blocks SSH entirely (Enforcer: FS, FILE_ACCESS), this tool will not work — see the the sandbox section below.

---

### nbs-remote-build — Chat-Aware Builds

Run a build command on a remote `nbs-ts` session while staying responsive to chat. Polls for build completion and checks chat between polls.

```bash
# Basic: run build, wait for shell prompt to reappear
nbs-remote-build <session> '<build-command>'

# Chat-aware: check chat while building
nbs-remote-build my-server 'make -j8' \
    --chat=<chat-file> --handle=claude

# Custom prompt pattern (e.g. venv prompt)
nbs-remote-build my-server 'make -j8' --prompt='(venv)'
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--prompt=PATTERN` | `\$ *>?\s*$` | Regex to detect shell prompt (build done) |
| `--timeout=N` | 300 | Build timeout in seconds |
| `--poll=N` | 5 | Poll interval in seconds |
| `--chat=FILE` | — | Chat file to check between polls |
| `--handle=NAME` | — | Chat handle for unread messages (required with --chat) |
| `--quiet` | — | Suppress progress dots |

**Exit codes:** 0=build completed, 2=session not found, 3=timeout, 4=bad arguments.

**This tool wraps nbs-ts, not SSH.** It works even when the sandbox blocks direct SSH. Always use this instead of `sleep N && nbs-ts read-new`.

---

### nbs-chat-remote — Remote Chat Access

Drop-in replacement for `nbs-chat` that executes commands on a remote machine via SSH. Same CLI, same exit codes — file paths refer to paths on the remote machine.

```bash
export NBS_CHAT_HOST=user@build-server
export NBS_CHAT_KEY=~/.ssh/id_ed25519

# All commands work identically to nbs-chat
nbs-chat-remote read /project/.nbs/chat/coordination.chat --last=5
nbs-chat-remote send /project/.nbs/chat/coordination.chat my-handle "Message"
```

**Requires direct SSH.** Will not work when the sandbox blocks SSH entirely.

---

## Higher-Level Remote Tools

### nbs-remote-run — One-Shot Remote Command

Runs a single command on a remote machine via SSH. Creates a temporary session, executes the command, captures output, cleans up.

```bash
nbs-remote-run <host> '<command>'
nbs-remote-run <host> --cwd=/path/to/project 'make -j8'
```

### nbs-remote-session — Persistent Remote Shell

Creates a named, persistent `nbs-ts` session with SSH to a remote host. Use for interactive work that spans multiple commands.

```bash
nbs-remote-session <host> --name=build --cwd=/path/to/project
```

### nbs-remote-diff — Remote Diff to Chat

Fetches `git diff` output from a remote session. Optionally posts the diff to a chat channel.

```bash
# Show unstaged changes
nbs-remote-diff my-server --cwd=/path/to/project

# Show diff for a specific file
nbs-remote-diff my-server --path=Jit/inliner.cpp --cwd=/path/to/project

# Show diff against base commit and post to chat
nbs-remote-diff my-server --commit=0ca33338 --cwd=/path/to/project \
    --chat=<chat-file> --handle=claude

# Just the diffstat
nbs-remote-diff my-server --stat --cwd=/path/to/project
```

**Options:** `--path=PATH`, `--stat`, `--staged`, `--commit=REF`, `--chat=FILE`, `--handle=NAME`, `--cwd=DIR`.

**Exit codes:** 0=success, 2=session not found, 3=timeout, 4=bad arguments.

---

### nbs-remote-status — Quick State Check

One-command state check: HEAD commit, branch, modified files, and diffstat.

```bash
# Quick state check
nbs-remote-status my-server --cwd=/path/to/project

# Post state to chat
nbs-remote-status my-server --cwd=/path/to/project \
    --chat=<chat-file> --handle=helper
```

Output:
```
=== Remote Status: my-server ===
HEAD: 0ca33338 Initial commit
Branch: main
Working tree: 3 files changed
Modified:
 M Jit/inliner.cpp
 M Jit/pyjit.cpp
 M Jit/hir/builder.cpp
Diff stat:
 3 files changed, 45 insertions(+), 12 deletions(-)
```

---

## the sandbox Constraint

On sandboxed environments, the sandbox blocks direct SSH from the Bash tool (Enforcer: FS, FILE_ACCESS). All remote tools use `nbs-ts` sessions internally to bypass this:

- **nbs-remote-edit** — uses `nbs-ts` for scp
- **nbs-remote-build** — uses `nbs-ts`
- **nbs-remote-diff** — uses `nbs-ts`
- **nbs-remote-status** — uses `nbs-ts`
- **nbs-chat-remote will not work** (requires direct SSH)
- **nbs-ts is the only path** to the remote machine

### File Editing: nbs-remote-edit

Use `nbs-remote-edit` for editing remote files. It uses scp via `nbs-ts` internally.

```bash
# 1. Download the file
nbs-remote-edit pull buildserver.example.com /path/to/project/Jit/inliner.cpp
# Returns: .nbs/remote-edit/buildserver.example.com/path/to/project/Jit/inliner.cpp

# 2. Edit locally using the normal Edit tool

# 3. Verify your changes
nbs-remote-edit diff buildserver.example.com /path/to/project/Jit/inliner.cpp

# 4. Push back
nbs-remote-edit push buildserver.example.com /path/to/project/Jit/inliner.cpp
```

### Fallback: Python String Replacement via nbs-ts

If nbs-remote-edit is unavailable, use Python `str.replace()` instead of sed. Safer because it does exact string matching (no regex surprises) and can verify the replacement was unique.

```bash
# Write a Python edit script
nbs-ts send <session> "python3 -c \"
import pathlib
p = pathlib.Path('/path/to/project/Jit/inliner.cpp')
src = p.read_text()
old = '''exact old text here'''
new = '''exact new text here'''
assert src.count(old) == 1, f'Expected 1 match, found {src.count(old)}'
p.write_text(src.replace(old, new))
print('OK')
\""
```

**Rules for Python edit scripts through nbs-ts:**

1. **Assert uniqueness**: Always verify `src.count(old) == 1` before replacing.
2. **One replacement per script**: Do not chain multiple replacements — each is a separate command.
3. **Verify after edit**: Re-read the file and confirm the change is correct.
4. **Post the script to chat before executing** so other agents can review.
5. **Prefer multi-line old/new strings** with enough context to be unique.

### Fallback: Polling Loop (instead of sleep)

Never use `sleep N` to wait for a build. Use a polling loop with `nbs-ts read-new`:

```bash
# Poll for shell prompt to reappear (build done)
timeout=300
elapsed=0
while [ $elapsed -lt $timeout ]; do
    output=$(nbs-ts read-new <session> --strip 2>/dev/null)
    if echo "$output" | grep -qE '\$ *$'; then
        break
    fi
    sleep 5
    elapsed=$((elapsed + 5))
done
```

Or use `nbs-ts wait-pattern` if available:
```bash
nbs-ts wait-pattern <session> '\$' --timeout=300
```

---

## Workflow Patterns

### Pattern 1: Edit-Build-Test Cycle

The standard cycle for modifying code on a remote machine.

**With nbs-remote-edit (when SSH works):**

```bash
# Pull all files you need to edit
nbs-remote-edit pull devserver.example.com /path/to/project/Jit/pyjit.cpp
nbs-remote-edit pull devserver.example.com /path/to/project/Jit/inliner.cpp

# Edit locally with the Edit tool (safe, reversible, syntax-aware)

# Diff to verify
nbs-remote-edit diff devserver.example.com /path/to/project/Jit/pyjit.cpp
nbs-remote-edit diff devserver.example.com /path/to/project/Jit/inliner.cpp

# Push back
nbs-remote-edit push devserver.example.com /path/to/project/Jit/pyjit.cpp
nbs-remote-edit push devserver.example.com /path/to/project/Jit/inliner.cpp

# Build with chat awareness
nbs-remote-build my-server 'make -j8' --chat=<chat-file> --handle=claude
```

**With nbs-ts fallback (when SSH is blocked):**

```bash
# Read the file
nbs-ts send <session> 'cat /path/to/project/Jit/pyjit.cpp'
sleep 3
nbs-ts read-new <session> --strip

# Edit via Python str.replace (see above)

# Verify the edit
nbs-ts send <session> 'cat /path/to/project/Jit/pyjit.cpp | head -220 | tail -20'

# Build with chat awareness
nbs-remote-build my-server 'make -j8' --chat=<chat-file> --handle=claude
```

### Pattern 2: Clean State Before Edits

Always verify the working tree is clean before starting edits. Stale changes from previous sessions cause cascading build failures.

```bash
nbs-ts send <session> 'cd /path/to/project && git status && git diff --stat'
sleep 2
nbs-ts read-new <session> --strip
```

If dirty:
```bash
# Revert to clean state
nbs-ts send <session> 'git checkout -- .'
# Or reset to a known commit
nbs-ts send <session> 'git checkout 0ca33338'
```

### Pattern 3: Exclusive Session Access

**Never share an nbs-ts session between agents.** If two agents need remote access simultaneously, create separate sessions:

```bash
# Agent 1 uses my-server (already exists)
nbs-ts send <session-1> 'make -j8'

# Agent 2 creates their own session
nbs-remote-session devserver.example.com --name=build-2
```

**Announce session ownership in chat:**
```
@team — I am using session my-server for the build. No one else should send commands to it until I report back.
```

### Pattern 4: Build Mode Selection

Use debug builds for correctness iteration, optimised builds for benchmarks only:

```bash
# Debug build (fast compile, for development)
nbs-remote-build my-server './configure --with-pydebug --disable-gil && make -j8' \
    --chat=<chat-file> --handle=claude --timeout=600

# Optimised build (slow compile, for benchmarks)
nbs-remote-build my-server './configure --disable-gil --enable-optimizations --with-lto && make -j8' \
    --chat=<chat-file> --handle=claude --timeout=1200
```

Switching between modes requires `make clean` first.

---

## Anti-Patterns (Do Not Do These)

### 1. Do not use sed for multi-line C/C++ edits

Sed through terminal sessions corrupts files. Every session that used sed extensively ended with cascading errors and a full stop.

```bash
# BAD — will corrupt the file eventually
nbs-ts send <session> "sed -i '414,453d' inliner.cpp"

# GOOD — exact string replacement with verification
nbs-ts send <session> "python3 -c \"...str.replace()...\""
```

### 2. Do not use sleep to wait for builds

Sleep wastes time (too long) or misses completion (too short). Use `nbs-remote-build` or a polling loop with `nbs-ts read-new`.

```bash
# BAD — blind guess, no chat access
nbs-ts send <session> 'make -j8'
sleep 120
nbs-ts read-new <session> --strip

# GOOD — polls for completion, checks chat
nbs-remote-build my-server 'make -j8' --chat=<chat-file> --handle=claude
```

### 3. Do not share nbs-ts sessions between agents

Two agents sending to the same session corrupt each other's commands and output. Each agent must own their session exclusively during use.

### 4. Do not edit files without checking git status first

Stale changes from previous sessions cause build failures that look like your edits are wrong. Always verify clean state.

### 5. Do not go silent during long operations

Post a status update before starting any operation that takes more than 30 seconds:

```
@team — Starting build on my-server. Using nbs-remote-build, will stay chat-responsive. ETA: build typically takes 2-3 minutes.
```

---

## Troubleshooting

### nbs-remote-edit fails with "the sandbox"

SSH is blocked from this pod. Use the nbs-ts fallback (Python str.replace scripts). nbs-remote-build still works because it wraps nbs-ts.

### Build times out (nbs-remote-build exit code 3)

Increase timeout: `--timeout=600` (10 minutes) or `--timeout=1200` (20 minutes). Full rebuilds on aarch64 can take 10-20 minutes.

### Prompt pattern not matching

The default prompt pattern matches `$ ` at end of line. If the remote shell has a custom prompt (e.g. `(venv) $>`), set `--prompt='(venv)'` or a regex that matches it.

### nbs-ts read-new returns stale output

Use `--strip` to remove ANSI escape codes. The default scrollback may include old output from previous commands.

### File appears corrupted after edit

Revert to a known-good state immediately:
```bash
nbs-ts send <session> 'cd /path/to/project && git checkout -- Jit/inliner.cpp'
```

Then re-apply edits using Python str.replace (not sed).

---

## Location

Tools are at:
- `~/.nbs/bin/nbs-remote-edit`
- `~/.nbs/bin/nbs-remote-build`
- `~/.nbs/bin/nbs-remote-diff`
- `~/.nbs/bin/nbs-remote-status`
- `~/.nbs/bin/nbs-remote-run`
- `~/.nbs/bin/nbs-remote-session`
- `~/.nbs/bin/nbs-chat-remote`
