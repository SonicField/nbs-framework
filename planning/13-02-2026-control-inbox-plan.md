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

1. Write tests for control inbox parsing and registry management
2. Implement `check_control_inbox()` and `seed_registry()` functions
3. Integrate into `poll_sidecar_tmux()` and `poll_sidecar_pty()`
4. Verify all existing nbs-claude tests still pass
5. Commit
