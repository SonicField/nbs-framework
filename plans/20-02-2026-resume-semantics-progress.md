# Resume Semantics — Progress Log

**Date:** 20-02-2026
**Author:** helper

## Completed

### Step 1: Session metadata infrastructure (nbs-claude)
- Added `--model=` and `--continue=` arg parsing
- Added `NBS_MODEL` and `NBS_CONTINUE_SESSION` environment variables
- UUID generation with 3-level fallback: `uuidgen || /proc/sys/kernel/random/uuid || python3 uuid`
- Session metadata written to `.nbs/sessions/<handle>.json` on startup
- Cleanup removes session metadata on exit
- `--continue=<id>` maps to `claude --resume <id>`; fresh starts use `claude --session-id <uuid>`

### Step 2: nbs-worker continue command
- `cmd_continue()` reads `.nbs/sessions/<handle>.json`
- Validates UUID format before passing to claude
- Kills old tmux session, cleans stale pidfile
- Respawns with `--continue=`, `--model=`, `--dangerously-skip-permissions`
- Supports `--model=<override>` to change model on continue
- Publishes `worker-continued` bus event

### Step 3: nbs-worker session command
- `cmd_session()` displays session metadata
- Shows PID liveness and tmux session status
- Reports session age

### Step 4: Model passthrough
- `nbs-claude` passes `--model` to Claude Code when `NBS_MODEL` is set
- `nbs-worker continue` reads model from metadata, allows override
- `nbs-worker dismiss` cleans session metadata

### Step 6: Tests
- `test_resume_semantics.sh`: 15 tests, all pass, no warnings
- `test_standup_message.sh`: 15 tests, all pass (standup enhancement)

### Additional: Standup enhancement
- Changed standup check-in message to @-mention scribe and supervisor
- Both local and remote message copies updated consistently

### Step 5: Runbook doc updates
- `claude_tools/nbs-teams-fixup.md` Level 3 updated: `nbs-worker continue` as preferred method, manual fallback preserved
- `claude_tools/nbs-teams-restart.md` updated: Level 3 renamed to "Continue", session metadata shortcut added, Step 6 updated with `nbs-worker continue` and `--model` examples

## Pending

None — all steps complete. Changes are uncommitted.

## Naming decision
Alex reviewed and directed: use `--continue` at the NBS level (not `--resume`) to avoid confusion with Claude Code's `--resume` flag. Implemented as directed.

## Open questions (resolved)
1. `nbs-worker continue` supports `--model=<override>` — YES (Alex)
2. `nbs-worker dismiss` removes session metadata — AGREED (Alex)
3. `nbs-teams-start` bootstrap uses `--session-id` — OPTIONALLY yes (Alex)

## Negative results
None observed. All tests pass on first run after fixes. The grep warning fix in test_resume_semantics.sh was the only post-implementation correction (changed `'\-\-flag'` patterns to `grep -qF -- '--flag'`).

## Known limitations
- Tests are static analysis (grep source patterns), not runtime integration tests
- JSON metadata uses unescaped shell variable interpolation — safe for typical handles but not robust against handles containing double-quotes
- Changes are uncommitted
