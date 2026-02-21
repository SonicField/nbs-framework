---
description: Remote development workflow for editing, building, and debugging on remote machines
allowed-tools: Bash
---

# Remote Development Workflow

This skill provides the workflow and tools for editing files, running builds, and debugging on remote machines (e.g. devserver) from an AI agent pod. It consolidates lessons from 1000+ messages of real team experience into actionable patterns.

---

## The Problem

AI agents run on pods (e.g. 3pai containers) that cannot directly SSH to development machines. All work on remote machines must flow through `pty-session`, which creates specific failure modes:

1. **Sed/heredoc corruption**: Using `sed` or heredocs through pty-session to edit C/C++ files causes cascading corruption — duplicate lines, broken syntax, missing struct fields. This was the single largest source of wasted time, causing a full session stop.
2. **Build blindness**: Agents go silent for 10-20 minutes during builds because they cannot read chat while blocked on `sleep 120`. The team assumes the agent has stopped, leading to premature task reassignment and near-conflicts.
3. **Pty-session collisions**: Two agents sending commands to the same pty-session simultaneously corrupt each other's output.
4. **No file coordination**: Multiple agents can edit the same remote file without knowing. Coordination is manual, through chat only.

---

## Tools

Three tools address these problems. All are in `bin/` or `~/.nbs/bin/`.

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

**Uses `ssh cat` internally** (not scp/sftp) to work in environments where BpfJailer blocks the SFTP subsystem. However, if BpfJailer blocks SSH entirely (Enforcer: FS, FILE_ACCESS), this tool will not work — see the BpfJailer section below.

---

### nbs-remote-build — Chat-Aware Builds

Run a build command on a remote pty-session while staying responsive to chat. Polls for build completion and checks chat between polls.

```bash
# Basic: run build, wait for shell prompt to reappear
nbs-remote-build <session> '<build-command>'

# Chat-aware: check chat while building
nbs-remote-build build-host 'make -j8' \
    --chat=.nbs/chat/live.chat --handle=claude

# Custom prompt pattern (e.g. venv prompt)
nbs-remote-build build-host 'make -j8' --prompt='(venv)'
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

**This tool wraps pty-session, not SSH.** It works even when BpfJailer blocks direct SSH. Always use this instead of `sleep N && pty-session read`.

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

**Requires direct SSH.** Will not work when BpfJailer blocks SSH entirely.

---

## BpfJailer Constraint

On 3pai pods, BpfJailer may block all outbound SSH (Enforcer: FS, FILE_ACCESS). When this happens:

- **nbs-remote-edit will not work** (requires SSH)
- **nbs-remote-edit-pty WILL work** (wraps pty-session, not SSH) — **use this instead**
- **nbs-chat-remote will not work** (requires SSH)
- **nbs-remote-build WILL work** (wraps pty-session, not SSH)
- **nbs-remote-diff WILL work** (wraps pty-session, not SSH)
- **nbs-remote-status WILL work** (wraps pty-session, not SSH)
- **pty-session is the only path** to the remote machine

### Preferred: nbs-remote-edit-pty (BpfJailer-proof editing)

Use `nbs-remote-edit-pty` instead of Python str.replace scripts. It provides the same safe pull/edit/push workflow as nbs-remote-edit but transfers files via base64 through pty-session. See the tool section above for usage.

### Fallback: Python String Replacement via pty-session

If nbs-remote-edit-pty is unavailable, use Python `str.replace()` instead of sed. This is safer because it does exact string matching (no regex surprises) and can verify the replacement was unique.

```bash
# Write a Python edit script
pty-session send build-host "python3 -c \"
import pathlib
p = pathlib.Path('/data/users/dev/project/Jit/inliner.cpp')
src = p.read_text()
old = '''exact old text here'''
new = '''exact new text here'''
assert src.count(old) == 1, f'Expected 1 match, found {src.count(old)}'
p.write_text(src.replace(old, new))
print('OK')
\""
```

**Rules for Python edit scripts through pty-session:**

1. **Assert uniqueness**: Always verify `src.count(old) == 1` before replacing.
2. **One replacement per script**: Do not chain multiple replacements — each is a separate command.
3. **Verify after edit**: Re-read the file and confirm the change is correct.
4. **Post the script to chat before executing** so other agents can review.
5. **Prefer multi-line old/new strings** with enough context to be unique.

### Fallback: pty-session wait (instead of sleep)

Never use `sleep N` to wait for a build. Use `pty-session wait`:

```bash
# Wait for shell prompt to reappear (build done)
pty-session wait build-host '\$' --timeout=300

# Wait for specific build output
pty-session wait build-host 'Built target' --timeout=600
```

---

## Workflow Patterns

### Pattern 1: Edit-Build-Test Cycle

The standard cycle for modifying code on a remote machine.

**With nbs-remote-edit (when SSH works):**

```bash
# Pull all files you need to edit
nbs-remote-edit pull devserver.example.com /data/users/dev/project/Jit/pyjit.cpp
nbs-remote-edit pull devserver.example.com /data/users/dev/project/Jit/inliner.cpp

# Edit locally with the Edit tool (safe, reversible, syntax-aware)

# Diff to verify
nbs-remote-edit diff devserver.example.com /data/users/dev/project/Jit/pyjit.cpp
nbs-remote-edit diff devserver.example.com /data/users/dev/project/Jit/inliner.cpp

# Push back
nbs-remote-edit push devserver.example.com /data/users/dev/project/Jit/pyjit.cpp
nbs-remote-edit push devserver.example.com /data/users/dev/project/Jit/inliner.cpp

# Build with chat awareness
nbs-remote-build build-host 'make -j8' --chat=.nbs/chat/live.chat --handle=claude
```

**With pty-session fallback (when SSH is blocked):**

```bash
# Read the file
pty-session send build-host 'cat /data/users/dev/project/Jit/pyjit.cpp'
sleep 3
pty-session read build-host --last=500

# Edit via Python str.replace (see above)

# Verify the edit
pty-session send build-host 'cat /data/users/dev/project/Jit/pyjit.cpp | head -220 | tail -20'

# Build with chat awareness
nbs-remote-build build-host 'make -j8' --chat=.nbs/chat/live.chat --handle=claude
```

### Pattern 2: Clean State Before Edits

Always verify the working tree is clean before starting edits. Stale changes from previous sessions cause cascading build failures.

```bash
pty-session send build-host 'cd /data/users/dev/project && git status && git diff --stat'
sleep 2
pty-session read build-host --last=30
```

If dirty:
```bash
# Revert to clean state
pty-session send build-host 'git checkout -- .'
# Or reset to a known commit
pty-session send build-host 'git checkout 0ca33338'
```

### Pattern 3: Exclusive Session Access

**Never share a pty-session between agents.** If two agents need remote access simultaneously, create separate sessions:

```bash
# Agent 1 uses build-host (already exists)
pty-session send build-host 'make -j8'

# Agent 2 creates their own session
pty-session create build-host 'ssh devserver.example.com'
pty-session wait build-host '\$' --timeout=30
pty-session send build-host 'cd /data/users/dev/project && source venv/bin/activate'
```

**Announce session ownership in chat:**
```
@team — I am using pty-session build-host for the build. No one else should send commands to it until I report back.
```

**Or use pty-session-lock (preferred):**
```bash
# Acquire exclusive access before using a session
pty-session-lock acquire build-host claude
# ... do your work ...
pty-session-lock release build-host claude

# Check who holds a session
pty-session-lock check build-host
# Output: build-host locked by claude (since 2026-02-20T15:30:00Z)

# With chat notification
pty-session-lock acquire build-host claude \
    --chat=.nbs/chat/live.chat --chat-handle=claude
```

Exit codes: 0=success, 2=lock held by another, 3=timeout, 5=wrong owner release.

### Pattern 4: Build Mode Selection

Use debug builds for correctness iteration, optimised builds for benchmarks only:

```bash
# Debug build (fast compile, for development)
nbs-remote-build build-host './configure --with-pydebug --disable-gil && make -j8' \
    --chat=.nbs/chat/live.chat --handle=claude --timeout=600

# Optimised build (slow compile, for benchmarks)
nbs-remote-build build-host './configure --disable-gil --enable-optimizations --with-lto && make -j8' \
    --chat=.nbs/chat/live.chat --handle=claude --timeout=1200
```

Switching between modes requires `make clean` first.

---

## Anti-Patterns (Do Not Do These)

### 1. Do not use sed for multi-line C/C++ edits

Sed through pty-session corrupts files. Every session that used sed extensively ended with cascading errors and a full stop.

```bash
# BAD — will corrupt the file eventually
pty-session send build-host "sed -i '414,453d' inliner.cpp"

# GOOD — exact string replacement with verification
pty-session send build-host "python3 -c \"...str.replace()...\""
```

### 2. Do not use sleep to wait for builds

Sleep wastes time (too long) or misses completion (too short). Use `nbs-remote-build` or `pty-session wait`.

```bash
# BAD — blind guess, no chat access
pty-session send build-host 'make -j8'
sleep 120
pty-session read build-host

# GOOD — polls for completion, checks chat
nbs-remote-build build-host 'make -j8' --chat=.nbs/chat/live.chat --handle=claude
```

### 3. Do not share pty-sessions between agents

Two agents sending to the same session corrupt each other's commands and output. Each agent must own their session exclusively during use. Use `pty-session-lock` to reserve sessions.

### 4. Do not edit files without checking git status first

Stale changes from previous sessions cause build failures that look like your edits are wrong. Always verify clean state.

### 5. Do not go silent during long operations

Post a status update before starting any operation that takes more than 30 seconds:

```
@team — Starting build on build-host Using nbs-remote-build, will stay chat-responsive. ETA: build typically takes 2-3 minutes.
```

---

## Troubleshooting

### nbs-remote-edit fails with "BpfJailer"

SSH is blocked from this pod. Use the pty-session fallback (Python str.replace scripts). nbs-remote-build still works because it wraps pty-session.

### Build times out (nbs-remote-build exit code 3)

Increase timeout: `--timeout=600` (10 minutes) or `--timeout=1200` (20 minutes). CinderX full rebuilds on aarch64 can take 10-20 minutes.

### Prompt pattern not matching

The default prompt pattern matches `$ ` at end of line. If the remote shell has a custom prompt (e.g. `(venv) $>`), set `--prompt='(venv)'` or a regex that matches it.

### pty-session read returns stale output

Use `--last=N` to get the most recent N lines: `pty-session read build-host --last=50`. The default scrollback (100 lines) may include old output from previous commands.

### File appears corrupted after edit

Revert to a known-good state immediately:
```bash
pty-session send build-host 'cd /data/users/dev/project && git checkout -- Jit/inliner.cpp'
```

Then re-apply edits using Python str.replace (not sed).

---

### nbs-remote-edit-pty — BpfJailer-Proof File Editing

Drop-in replacement for `nbs-remote-edit` that transfers files through `pty-session` instead of SSH. Uses base64 encoding to prevent any character corruption during transport.

```bash
# 1. Download the file (via pty-session, not SSH)
nbs-remote-edit-pty pull build-host /data/users/dev/project/Jit/inliner.cpp
# Returns: .nbs/remote-edit/build-host

# 2. Edit locally using the Edit tool — same as nbs-remote-edit

# 3. Verify your changes
nbs-remote-edit-pty diff build-host /data/users/dev/project/Jit/inliner.cpp

# 4. Push back (with automatic md5 verification)
nbs-remote-edit-pty push build-host /data/users/dev/project/Jit/inliner.cpp
```

**Key differences from nbs-remote-edit:**

| Feature | nbs-remote-edit | nbs-remote-edit-pty |
|---------|----------------|---------------------|
| Transport | SSH (ssh cat) | pty-session + base64 |
| BpfJailer | Blocked | Works |
| First argument | hostname | pty-session name |
| Push verification | None | md5 checksum |
| Large files | Streaming | Chunked (400-char chunks) |

**Exit codes:** 0=success, 2=file not found, 3=pty-session error, 4=bad arguments, 5=verification failed.

**Use this tool instead of Python str.replace scripts.** It provides the same safe pull/edit/push workflow as nbs-remote-edit but works when BpfJailer blocks SSH.

---

### nbs-remote-diff — Remote Diff to Chat

Fetches `git diff` output from a remote pty-session. Optionally posts the diff to a chat channel.

```bash
# Show unstaged changes
nbs-remote-diff build-host --cwd=/data/users/dev/project

# Show diff for a specific file
nbs-remote-diff build-host --path=Jit/inliner.cpp --cwd=/data/users/dev/project

# Show diff against base commit and post to chat
nbs-remote-diff build-host --commit=0ca33338 --cwd=/data/users/dev/project \
    --chat=.nbs/chat/live.chat --handle=claude

# Just the diffstat
nbs-remote-diff build-host --stat --cwd=/data/users/dev/project
```

**Options:** `--path=PATH`, `--stat`, `--staged`, `--commit=REF`, `--chat=FILE`, `--handle=NAME`, `--cwd=DIR`.

**Exit codes:** 0=success, 2=session not found, 3=timeout, 4=bad arguments.

---

### nbs-remote-status — Quick State Check

One-command state check: HEAD commit, branch, modified files, and diffstat.

```bash
# Quick state check
nbs-remote-status build-host --cwd=/data/users/dev/project

# Post state to chat
nbs-remote-status build-host --cwd=/data/users/dev/project \
    --chat=.nbs/chat/live.chat --handle=helper
```

Output:
```
=== Remote Status: build-host ===
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

## Location

Tools are at:
- `bin/nbs-remote-edit` or `~/.nbs/bin/nbs-remote-edit`
- `bin/nbs-remote-edit-pty` or `~/.nbs/bin/nbs-remote-edit-pty` **(BpfJailer-proof)**
- `bin/nbs-remote-build` or `~/.nbs/bin/nbs-remote-build`
- `bin/nbs-remote-diff` or `~/.nbs/bin/nbs-remote-diff`
- `bin/nbs-remote-status` or `~/.nbs/bin/nbs-remote-status`
- `bin/pty-session-lock` or `~/.nbs/bin/pty-session-lock`
- `bin/nbs-chat-remote` or `~/.nbs/bin/nbs-chat-remote`
- `~/.nbs/bin/pty-session`
