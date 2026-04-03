#!/bin/bash
# Test: Deferred sidecar fixes — capture window and cooldown extraction
#
# TDD red-phase tests for deferred items 3 and 4:
#   Item 3: Dual cooldown check must use a single source function
#   Item 4: Injection verification must capture > 1 line
#
# These tests verify structural properties of sidecar.c.
# They will FAIL against the current code and PASS after the fix.
#
# Falsifiable tests covering:
#   1. Injection verify capture uses > 1 line (not tp->capture(tp, 1))
#   2. cooldown_is_active function exists in sidecar.h
#   3. cooldown_is_active function is defined in sidecar.c
#   4. should_inject_notify calls cooldown_is_active (not inline computation)
#   5. Main loop cooldown tracking calls cooldown_is_active
#   6. No independent cooldown computation remains (elapsed < cfg->notify_cooldown)
#   7. notify_fail_threshold is compared against notify_fail_count
#   8. Retry-Enter is replaced with chat warning (no tp->send_key Enter in verify)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SIDECAR_C="${PROJECT_ROOT}/src/nbs-sidecar/sidecar.c"
SIDECAR_H="${PROJECT_ROOT}/src/nbs-sidecar/sidecar.h"

ERRORS=0
PASS_COUNT=0

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

# Safe grep -c that works with pipefail.
# Returns the count as a clean integer, 0 if no matches.
count_matches() {
    grep -c "$@" 2>/dev/null || true
}

# Same but for piped input (stdin)
count_stdin_matches() {
    grep -c "$@" 2>/dev/null || true
}

echo "=== Deferred Sidecar Fixes — Structural Tests ==="
echo ""

# ---- Item 4: Capture window width ----
echo "--- Item 4: Injection verification capture window ---"

# The injection verification loop captures terminal content to check if the
# notification was consumed. Currently it captures only 1 line:
#   tp->capture(tp, 1)
# This should capture more lines (at least 3) to handle terminal layout
# variations (resize, status bar, tmux decoration).

# Find the verify capture line in the injection verification section.
# Use a wider range: from "Verify injection" to "injection_consumed = 1"
# to capture the full retry loop including the tp->capture call.
verify_section=$(sed -n '/Verify injection consumed/,/injection_consumed = 1/p' "$SIDECAR_C")
verify_capture_1line=$(echo "$verify_section" | count_stdin_matches 'tp->capture(tp, 1)')

check "verify capture does NOT use 1-line window" \
    "$([[ "$verify_capture_1line" -eq 0 ]] && echo pass || echo fail)"

# The verify capture should use at least 3 lines
verify_capture_multi=$(echo "$verify_section" | count_stdin_matches -E 'tp->capture\(tp, [3-9]\)|tp->capture\(tp, [1-9][0-9]')

check "verify capture uses >= 3 lines" \
    "$([[ "$verify_capture_multi" -ge 1 ]] && echo pass || echo fail)"

echo ""

# ---- Item 3: Single-source cooldown ----
echo "--- Item 3: Cooldown extraction to single function ---"

# cooldown_is_active should be declared in sidecar.h
check "cooldown_is_active declared in sidecar.h" \
    "$(grep -q 'cooldown_is_active' "$SIDECAR_H" && echo pass || echo fail)"

# cooldown_is_active should be defined in sidecar.c
check "cooldown_is_active defined in sidecar.c" \
    "$(grep -q 'cooldown_is_active' "$SIDECAR_C" && echo pass || echo fail)"

# should_inject_notify should call cooldown_is_active instead of inline computation
inject_fn=$(sed -n '/^static int should_inject_notify/,/^}/p' "$SIDECAR_C")
inject_uses_cooldown=$(echo "$inject_fn" | count_stdin_matches 'cooldown_is_active')

check "should_inject_notify calls cooldown_is_active" \
    "$([[ "$inject_uses_cooldown" -ge 1 ]] && echo pass || echo fail)"

# Main loop cooldown tracking should call cooldown_is_active
mainloop_section=$(sed -n '/Root Cause B.*cooldown suppression/,/catchup_needed.*cooldown expired/p' "$SIDECAR_C")
mainloop_uses_cooldown=$(echo "$mainloop_section" | count_stdin_matches 'cooldown_is_active')

check "main loop cooldown tracking calls cooldown_is_active" \
    "$([[ "$mainloop_uses_cooldown" -ge 1 ]] && echo pass || echo fail)"

# No inline cooldown computation should remain outside cooldown_is_active.
# Count total occurrences of the pattern, then subtract those inside cooldown_is_active.
total_inline=$(count_matches 'elapsed.*<.*notify_cooldown' "$SIDECAR_C")
cooldown_fn=$(sed -n '/^int cooldown_is_active/,/^}/p' "$SIDECAR_C" 2>/dev/null || true)
inside_fn=0
if [[ -n "$cooldown_fn" ]]; then
    inside_fn=$(echo "$cooldown_fn" | count_stdin_matches 'elapsed.*<.*notify_cooldown')
fi
outside_fn=$((total_inline - inside_fn))

check "no inline cooldown computation outside cooldown_is_active ($outside_fn remaining)" \
    "$([[ "$outside_fn" -eq 0 ]] && echo pass || echo fail)"

echo ""

# ---- Item 1: notify_fail_threshold connected ----
echo "--- Item 1: notify_fail_threshold wired to action ---"

# notify_fail_count should be compared against notify_fail_threshold somewhere
check "notify_fail_count compared against threshold" \
    "$(grep -qE 'notify_fail_count.*notify_fail_threshold|notify_fail_threshold.*notify_fail_count' "$SIDECAR_C" && echo pass || echo fail)"

echo ""

# ---- Item 2: Retry-Enter replaced with chat warning ----
echo "--- Item 2: Retry-Enter replaced with chat warning ---"

# The old retry-Enter in the injection verification loop should be removed.
# Currently it's commented out: /* tp->send_key(tp, "Enter"); */
# After the fix, the comment should be gone and replaced with a chat warning.
# Re-extract with wider range for Enter check
verify_wide=$(sed -n '/Verify injection consumed/,/notify_fail_count/p' "$SIDECAR_C")
verify_enter=$(echo "$verify_wide" | count_stdin_matches 'send_key.*Enter')

check "no send_key Enter in verification loop (disabled or removed)" \
    "$([[ "$verify_enter" -eq 0 ]] && echo pass || echo fail)"

# Should have a chat warning mechanism — either in the verification loop
# or in the threshold check that follows it. Use a wider range to capture both.
verify_threshold=$(sed -n '/Verify injection consumed/,/state\.idle_seconds = 0/p' "$SIDECAR_C")
verify_chat_warn=$(echo "$verify_threshold" | count_stdin_matches -E 'chat_client_error|nbs-chat.*send|chat.*warn')

check "chat warning on failed injection verification" \
    "$([[ "$verify_chat_warn" -ge 1 ]] && echo pass || echo fail)"

echo ""

# ---- Summary ----
echo "=== Results ==="
TOTAL=$((PASS_COUNT + ERRORS))
echo "Pass: $PASS_COUNT | Fail: $ERRORS | Total: $TOTAL"
if [[ $ERRORS -eq 0 ]]; then
    echo "All tests passed."
    exit 0
else
    echo "$ERRORS test(s) failed — expected in TDD red phase."
    exit 1
fi
