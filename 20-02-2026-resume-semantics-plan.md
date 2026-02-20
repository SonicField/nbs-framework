# Resume Semantics for NBS Team Members

**Date:** 20-02-2026
**Author:** helper
**Status:** APPROVED — All steps implemented (Steps 1-6). Uncommitted.

## Problem Statement

Currently, restarting a team member destroys accumulated context. The `/nbs-teams-restart` and `/nbs-teams-fixup` runbooks document a Level 3 recovery (`--resume`) but the process is manual and fragile:

1. Find the session ID by grep-ing tmux scrollback or `/proc/<pid>/cmdline`
2. Kill the tmux session
3. Manually construct the respawn command with the correct handle, model, resume ID, and permissions

**What Alex wants:** A single command that resumes a team member with:
- The correct session ID from the old session
- The correct model (which may change between sessions)
- Permissions bypass for unattended operation
- No manual grep-ing or command construction

## Current State Analysis

### How session IDs are obtained today

Claude Code stores session IDs internally. The only ways to extract them:

1. **`/proc/<pid>/cmdline`** — if the agent was started with `--resume <id>`, the ID is in the command line. If started fresh, there's no `--resume` flag and the internally-generated session ID is not exposed.
2. **tmux scrollback grep** — the session ID appears in the Claude Code startup banner as a UUID. This is unreliable on long-running sessions (scrollback may rotate).
3. **`claude --continue`** — resumes the most recent session in the current directory. This is fragile when multiple agents share a directory.
4. **`claude --resume` interactive picker** — `claude -r` without an ID opens an interactive picker. Not scriptable.

### Gap: no persistent session ID record

The fundamental problem: `nbs-claude` and `nbs-worker` don't record the Claude Code session ID anywhere persistent. When a session needs to be resumed, someone has to manually find the ID.

### Model selection

Claude Code accepts `--model <model>` to specify the model. Current spawn commands don't pass this — agents use whatever the default is. If the default model changes (e.g., from sonnet to opus), respawned agents silently get a different model than original agents.

## Design

### Approach: Persistent session metadata in `.nbs/sessions/`

Create a session metadata directory `.nbs/sessions/<handle>.json` that records everything needed to resume an agent:

```json
{
  "handle": "theologian",
  "session_id": "abc12345-1234-1234-1234-123456789abc",
  "model": "opus",
  "tmux_session": "nbs-theologian-live",
  "started": "2026-02-20T11:44:07Z",
  "initial_prompt": "Your NBS handle is 'theologian'. Load /nbs-teams-chat...",
  "project_root": "/home/alexturner/claude_docs/nbs-framework",
  "pid": 12345
}
```

### Session ID capture

Two mechanisms, both needed:

**A. Startup capture (new sessions):** When `nbs-claude` launches Claude Code, it passes `--session-id <generated-uuid>` to explicitly set the session ID rather than letting Claude generate one internally. This makes the ID known at spawn time.

**B. Runtime capture (existing sessions):** For agents already running, extract the session ID via `/proc` inspection (the `nbs-worker search` equivalent for session metadata).

### Changes required

#### 1. `nbs-claude` changes

- Accept `--model=<model>` argument, pass to `claude --model <model>`
- Accept `--resume=<session-id>` argument, pass to `claude --resume <session-id>`
- Generate a UUID at startup if no `--resume` given, pass via `--session-id <uuid>`
- Write session metadata to `.nbs/sessions/<handle>.json` on startup
- Update metadata on clean exit

#### 2. `nbs-worker` changes (or new `nbs-team resume` command)

Add `nbs-worker resume <name>` command:
- Read `.nbs/sessions/<handle>.json` to get session ID, model, initial prompt
- Kill the old tmux session
- Respawn with `--resume <session-id> --model <model> --dangerously-skip-permissions`
- Update the session metadata file with new PID and timestamp

Add `nbs-worker session <name>` command:
- Display current session metadata (ID, model, context level, uptime)

#### 3. `/nbs-teams-restart` changes

Update the Level 3 recovery to use the new session metadata:
```bash
# Old (manual):
SESSION_ID=$(cat /proc/<pid>/cmdline | tr '\0' ' ' | grep -oE '[0-9a-f-]{36}')
tmux kill-session -t nbs-<handle>-live
claude --resume $SESSION_ID --dangerously-skip-permissions

# New (automated):
nbs-worker resume <handle>
```

#### 4. `/nbs-teams-fixup` changes

Same simplification at Level 3.

## Implementation Plan

### Step 1: Session metadata infrastructure

**Files:** `bin/nbs-claude` (modify), `.nbs/sessions/` (new directory)

- Add `--model=` argument parsing in nbs-claude's arg loop
- Add `--resume=` argument parsing (distinct from Claude Code's --resume)
- Generate UUID at startup: `SESSION_UUID=$(uuidgen || cat /proc/sys/kernel/random/uuid)`
- Pass `--session-id $SESSION_UUID` to Claude Code launch command
- Write `.nbs/sessions/<handle>.json` after session starts
- Clean up metadata in the cleanup() trap

**Falsifier:** Start nbs-claude, verify `.nbs/sessions/<handle>.json` exists and contains a valid UUID.

### Step 2: nbs-worker resume command

**Files:** `bin/nbs-worker` (modify)

- Add `cmd_resume()` function
- Read session metadata from `.nbs/sessions/<handle>.json`
- Kill old tmux session
- Respawn with `--resume`, `--model`, and `--dangerously-skip-permissions`
- Validate session ID format (UUID regex) before passing to claude

**Falsifier:** Start an agent, run `nbs-worker resume <name>`, verify agent comes back with context preserved (chat post before and after resume should both be visible to the agent).

### Step 3: nbs-worker session command

**Files:** `bin/nbs-worker` (modify)

- Add `cmd_session()` function
- Read and display `.nbs/sessions/<handle>.json`
- Include live data: tmux alive status, PID alive status, approximate context level from tmux capture

**Falsifier:** `nbs-worker session <name>` shows session ID, model, and PID status.

### Step 4: Model passthrough

**Files:** `bin/nbs-claude` (modify), `bin/nbs-worker` (modify)

- nbs-claude passes `--model` to Claude Code
- nbs-worker spawn accepts `--model=<model>` and passes to nbs-claude
- nbs-worker resume reads model from session metadata and passes it
- Default model comes from environment variable `NBS_MODEL` (fallback: no --model flag, use Claude Code default)

**Falsifier:** Start agent with `--model=opus`, resume, verify the resumed session uses the same model (check `/proc/<pid>/cmdline`).

### Step 5: Update runbook docs

**Files:** `claude_tools/nbs-teams-restart.md`, `claude_tools/nbs-teams-fixup.md`

- Replace manual session ID extraction with `nbs-worker resume`
- Add `nbs-worker session` to diagnostic steps
- Document `--model` flag

### Step 6: Tests

**Files:** `tests/automated/test_resume_semantics.sh` (new)

Tests:
1. Session metadata file created on nbs-claude start
2. Session metadata contains valid UUID
3. Session metadata contains model when --model specified
4. nbs-worker resume reads metadata correctly
5. nbs-worker session displays metadata
6. Resume with stale metadata (PID dead) still works
7. Resume with missing metadata file gives clear error
8. Model override on resume (--model on resume overrides metadata)
9. Handle collision guard still works with resume
10. Concurrent resume attempts (flock protection)

## Dependencies and Risks

| Risk | Mitigation |
|------|------------|
| `--session-id` flag may not be available in all Claude Code versions | Check `claude --help` output before using; fall back to `--continue` |
| UUID generation unavailable (`uuidgen` not installed) | Fall back to `/proc/sys/kernel/random/uuid` (Linux) or `python3 -c 'import uuid; print(uuid.uuid4())'` |
| Session metadata stale (agent crashed without cleanup) | `nbs-worker resume` should check PID liveness before trusting metadata |
| Model name format changes | Validate model against known aliases (sonnet, opus, haiku) |
| `.nbs/sessions/` directory doesn't exist | Create in nbs-claude startup, same as pidfiles |

## Estimated Scope

- **nbs-claude:** ~50 lines of changes (arg parsing, UUID generation, metadata write)
- **nbs-worker:** ~80 lines (resume command, session command)
- **Tests:** ~150 lines
- **Docs:** ~30 lines of updates to two runbooks
- **Total:** ~310 lines of code + docs

## Open Questions for Alex

1. Should `nbs-worker resume` also support a `--model=<new-model>` override, or always use the model from the metadata? (I recommend supporting override — the model may change.)
2. Should session metadata survive `nbs-worker dismiss`? (I recommend deleting it on dismiss — it's stale after that.)
3. Should the `nbs-teams-start` bootstrap also use `--session-id` for initial spawns? (I recommend yes — consistency.)
