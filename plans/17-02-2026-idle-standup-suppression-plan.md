# Idle Standup Suppression Plan

**Date**: 17-02-2026
**Author**: generalist
**Problem**: Sidecar standups wake idle agents, consuming context tokens with zero-information responses ("standing by"). This causes context rot over hours.

## Root Cause

`check_standup_trigger()` posts a standup to chat. `check_chat_unread()` detects it as unread. `should_inject_notify()` injects `/nbs-notify` into the idle agent. The agent processes it, emits a low-information response that cannot be compacted.

## Design

**Approach**: Refined Option B (per @gatekeeper's suggestion). In `should_inject_notify()`, after `check_chat_unread()` returns unread messages, check whether ALL unread messages are from handle `sidecar`. If yes AND there are no bus events, suppress the injection.

**Mechanism**: New function `are_chat_unread_sidecar_only()` that:
1. Reads the chat file directly (no `nbs-chat` CLI — avoid cursor advancement)
2. Extracts lines after the `---` separator
3. Skips to the cursor position for this sidecar's handle
4. Base64-decodes each remaining line
5. Checks if the handle prefix (before `:`) is `sidecar` for ALL unread messages
6. Returns 0 if all unread are sidecar-only, 1 otherwise

**Integration point**: In `should_inject_notify()`, after line 690 (the "nothing pending" check), add:
```
# If only bus events pending, proceed normally
# If only chat unread and all from sidecar, suppress
if [[ $bus_rc -ne 0 && $chat_rc -eq 0 ]]; then
    if are_chat_unread_sidecar_only; then
        return 1  # suppress — idle agent doesn't need sidecar-only standups
    fi
fi
```

**Invariants**:
- If any non-sidecar message is unread, injection proceeds normally (responsiveness preserved)
- If bus events exist, injection proceeds normally (bus events always matter)
- If both bus events and sidecar-only chat exist, injection proceeds (bus events take priority)
- Cursor is never advanced by this check (read-only peek)

**Falsifier**: An agent should NOT be woken if the only unread messages are sidecar standups. An agent SHOULD be woken if any human or agent message is unread.

## Plan

1. Write test T20: idle standup suppression
2. Write `are_chat_unread_sidecar_only()` function
3. Modify `should_inject_notify()` to call it
4. Run full test suite
5. Post results to chat

## Test Design (T20)

### T20a — Structural: function exists
Verify `are_chat_unread_sidecar_only` exists in nbs-claude.

### T20b — Sidecar-only unread: suppresses injection
- Create a chat file with messages
- Set cursor to current position
- Send a sidecar standup message
- Verify `are_chat_unread_sidecar_only` returns 0
- Verify `should_inject_notify` returns 1 (suppressed)

### T20c — Mixed unread: allows injection
- Create a chat file with messages
- Set cursor to current position
- Send a sidecar standup message
- Send a non-sidecar message (e.g. from "alice")
- Verify `are_chat_unread_sidecar_only` returns 1
- Verify `should_inject_notify` returns 0 (allowed)

### T20d — Bus events override suppression
- Create a chat with sidecar-only unread
- Also create a bus event
- Verify `should_inject_notify` returns 0 (bus events always fire)

### T20e — Empty unread: no suppression needed
- Create a chat file, cursor caught up
- Verify `are_chat_unread_sidecar_only` returns 1 (nothing to suppress)

### T20f — Cursor safety: no cursor advancement
- Record cursor position before check
- Run `are_chat_unread_sidecar_only`
- Verify cursor position unchanged
