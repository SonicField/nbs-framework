# NBS-Claude Control Inbox — Plan

**Date:** 13 February 2026
**Scope:** Add dynamic resource registration to bin/nbs-claude via append-only control inbox

## Design

### What
Add a `.nbs/control-inbox` file that the AI writes to and the nbs-claude sidecar reads from. The sidecar processes new lines (forward-only, never truncates) and maintains a resource registry.

### How

1. **Control inbox file:** `.nbs/control-inbox` (append-only)
   - AI writes lines like: `register-chat .nbs/chat/debug.chat`
   - Sidecar tracks read offset (line count), processes only new lines
   - Full history preserved for audit

2. **Registry file:** `.nbs/control-registry` (rewritten by sidecar)
   - Current set of registered resources, one per line: `chat:.nbs/chat/live.chat`
   - Seeded on startup from existing `.nbs/chat/*.chat` and `.nbs/events/`
   - Modified when control commands arrive

3. **Sidecar integration:**
   - `check_control_inbox()` called every iteration (1 second)
   - Separate from idle detection — registration should be immediate
   - No pane scanning for control commands (bench-claude's recommendation)

### Falsification criteria
1. Writing `register-chat .nbs/chat/new.chat` to `.nbs/control-inbox` causes the registry to include it within 2 seconds
2. The sidecar does not re-process old lines after restarting the check loop
3. The control inbox file is never truncated or modified by the sidecar
4. Duplicate registrations are idempotent (no duplicate entries in registry)
5. Unregister removes the entry from the registry
6. Malformed lines are silently ignored (no crash, no partial state)

## Steps

1. ~~Write tests for control inbox parsing and registry management~~ ✓
2. ~~Implement `check_control_inbox()` and `seed_registry()` functions~~ ✓
3. ~~Integrate into `poll_sidecar_tmux()` and `poll_sidecar_pty()`~~ ✓
4. ~~Verify all existing nbs-claude tests still pass~~ ✓ (54/54 pass)
5. ~~Commit~~ ✓ (599cacc)

## Completion Notes

All falsification criteria verified by functional tests:
- Criterion 1: `check_control_inbox` processes new lines → registry updated
- Criterion 2: Forward-only offset tracking → old lines not re-processed
- Criterion 3: No `>` redirect to inbox file found in code; inbox preserved (3 lines intact after 3 commands)
- Criterion 4: Both `seed_registry` and `register-*` commands are idempotent (grep-before-write)
- Criterion 5: `unregister-chat` verified to remove entry
- Criterion 6: Empty lines, whitespace-only lines, and unknown commands handled without crash

Bug found and fixed: `for chat in .nbs/chat/*.chat 2>/dev/null` is a bash syntax error — the `2>/dev/null` redirect cannot go on the `for` line in bash 5.1. The `[[ -f "$chat" ]] || continue` guard already handles the no-match case.
