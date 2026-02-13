# Plan: nbs-claude Plan Mode Auto-Select

**Date:** 2026-02-12
**Problem:** When a Claude worker (or nbs-claude session) enters plan mode, Claude Code displays an interactive menu prompt that blocks execution. No human is watching worker sessions, so the prompt hangs indefinitely.

**Alex's prompt example:**
```
 Claude has written up a plan and is ready to execute. Would you like to proceed?

   1. Yes, clear context and bypass permissions
 ❯ 2. Yes, and bypass permissions
   3. Yes, manually approve edits
   4. Type here to tell Claude what to change

 ctrl-g to edit in Vim · ~/.claude/plans/abstract-crafting-cake.md
```

**Required action:** The nbs-claude sidecar must detect this prompt and auto-select option 2 ("Yes, and bypass permissions").

## Falsification Criteria

| Criterion | Falsifier |
|-----------|-----------|
| Detection | Inject plan mode prompt text into a tmux pane → sidecar does NOT detect it within 2 poll cycles |
| Selection | Sidecar detects prompt → does NOT send "2" + Enter within 5s |
| No false positive | Normal `❯` prompt → sidecar sends "2" when it should send `/nbs-poll` |
| Both modes | Plan mode auto-select works in tmux mode but NOT in pty mode (or vice versa) |

## Architecture

The sidecar already captures the last 5 lines of the tmux pane every second and checks for the `❯` prompt. The plan mode prompt also contains `❯` (it's the cursor indicator on the selected option), so the sidecar already detects idle state — it just doesn't distinguish plan mode from normal prompt.

**Approach:** Before checking for the normal prompt, check if the captured content matches the plan mode prompt pattern. If so, send "2" + Enter instead of `/nbs-poll`.

**Pattern to match:** The plan mode prompt contains the text `Would you like to proceed?`. This is a reliable discriminator — it will not appear in normal Claude Code output at the `❯` prompt. We match against the full 5-line capture, not just the last 3 lines.

**Key detail:** The plan mode prompt is a numbered menu. Sending "2" selects option 2. We do NOT need arrow keys or complex navigation — just the character "2" followed by Enter.

However: examining the prompt more carefully, the `❯` is already on option 2. The user interface may use arrow-key navigation where Enter selects the highlighted option. So we should just send Enter (to confirm the already-highlighted option 2).

**Wait — we need to verify this.** The default selection (`❯`) is on option 2. If we just send Enter, it selects option 2. This is simpler and more robust than sending "2".

**BUT**: We cannot assume the cursor is always on option 2. The `❯` cursor position shown in Alex's screenshot happens to be on option 2, but it might default differently in other contexts. Safer approach: send the literal keystroke "2" to explicitly select option 2, then Enter.

**Revised approach:** Actually, Claude Code's plan mode menu uses number keys for selection. Send keystroke "2" to select option 2, which auto-confirms. No Enter needed — but we send Enter anyway for robustness.

**Final approach after reflection:** The safest is to match the prompt pattern, and send `2` followed by a short delay and Enter. This explicitly selects option 2 regardless of cursor position.

## Changes

### 1. `bin/nbs-claude` — Add plan mode detection to both sidecar loops

In both `poll_sidecar_tmux()` and `poll_sidecar_pty()`, insert a plan mode check **before** the existing prompt check. This runs on every content capture (every 1s), not just after idle timeout — plan mode should be resolved quickly, not after 30s of idle.

**New function:** `detect_plan_mode()`
```bash
detect_plan_mode() {
    local content="$1"
    echo "$content" | grep -qF 'Would you like to proceed?'
}
```

**Modified sidecar loop logic:**
```
capture content
hash content → detect changes → reset idle if changed

# NEW: Immediate plan mode check (every cycle, not just after idle)
if detect_plan_mode(content):
    send "2" + Enter
    reset idle, sleep 5
    continue

# Existing idle check
if idle >= interval:
    if prompt detected:
        send /nbs-poll + Enter
```

The plan mode check fires immediately on detection (not after idle timeout), because the worker is blocked and every second of delay is wasted.

### 2. `tests/automated/test_nbs_claude.sh` — Add plan mode tests

Add tests verifying:
- Test N+1: `detect_plan_mode` function exists in the script
- Test N+2: Plan mode detection pattern (`Would you like to proceed?`) is present
- Test N+3: Plan mode sends "2" keystroke (not `/nbs-poll`)
- Test N+4: Plan mode detection is separate from idle-timeout prompt detection

### 3. Integration test (manual verification)

The real test requires a running Claude Code instance entering plan mode in tmux. This cannot be automated without a Claude Code mock. Manual verification:
1. Start `nbs-claude` in tmux
2. Ask Claude to enter plan mode
3. Verify sidecar auto-selects option 2 within a few seconds
4. Verify normal `/nbs-poll` injection still works after

## Implementation Sequence

1. Write the `detect_plan_mode` function
2. Modify `poll_sidecar_tmux` to check plan mode before idle check
3. Modify `poll_sidecar_pty` identically
4. Add tests to `test_nbs_claude.sh`
5. Run `make test` or the test script directly
6. Verify manually if possible

## Out of Scope

- Other interactive prompts (e.g., "Do you want to continue?" confirmations) — can be added later with the same pattern
- Configurable option selection (always option 2 for now)
- Back-off if plan mode detection fires repeatedly on the same prompt (unlikely — sending "2" should dismiss it)
