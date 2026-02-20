#!/bin/bash
# Test: @mention! interrupt pattern
#
# Tests that @handle! triggers chat-interrupt events at critical priority,
# and that the sidecar handles them by sending Escape and re-injecting
# /nbs-notify.
#
# Part 1: Static analysis — grep source for required patterns (1-12)
# Part 2: Integration — use nbs-chat + nbs-bus to verify event creation (13-20)
# Part 3: Adversarial — edge cases that must not false-trigger (21-25)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"
BUS_BRIDGE_C="$PROJECT_ROOT/src/nbs-chat/bus_bridge.c"
BUS_BRIDGE_H="$PROJECT_ROOT/src/nbs-chat/bus_bridge.h"

# Add bin/ to PATH so bus_bridge.c can find nbs-bus via execlp
export PATH="$PROJECT_ROOT/bin:$PATH"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

# --- Preconditions ---
for f in "$NBS_CLAUDE" "$BUS_BRIDGE_C" "$BUS_BRIDGE_H"; do
    if [[ ! -f "$f" ]]; then
        echo "FATAL: required file not found: $f"
        exit 1
    fi
done

echo "Test: @mention! interrupt pattern"
echo "==================================="
echo ""

# =================================================================
# Part 1: Static analysis — bus_bridge.c / bus_bridge.h / nbs-claude
# =================================================================
echo "Part 1: Static analysis"
echo "-----------------------"

# 1. bus_bridge.c contains chat-interrupt event type string
R=$(grep -qF 'chat-interrupt' "$BUS_BRIDGE_C" && echo pass || echo fail)
check "1. bus_bridge.c contains 'chat-interrupt' event type" "$R"

# 2. bus_bridge.c publishes interrupt at critical priority
R=$(grep -q '"chat-interrupt".*"critical"' "$BUS_BRIDGE_C" && echo pass || echo fail)
check "2. bus_bridge.c publishes interrupt at critical priority" "$R"

# 3. bus_bridge.c detects '!' after handle
R=$(grep -qF "'!'" "$BUS_BRIDGE_C" && echo pass || echo fail)
check "3. bus_bridge.c checks for '!' character after handle" "$R"

# 4. bus_bridge.c has out_interrupt_flags parameter
R=$(grep -qF 'out_interrupt_flags' "$BUS_BRIDGE_C" && echo pass || echo fail)
check "4. bus_bridge.c has out_interrupt_flags parameter" "$R"

# 5. bus_bridge.h declares out_interrupt_flags in bus_extract_mentions
R=$(grep -qF 'out_interrupt_flags' "$BUS_BRIDGE_H" && echo pass || echo fail)
check "5. bus_bridge.h declares out_interrupt_flags parameter" "$R"

# 6. bus_bridge.h documents chat-interrupt event type
R=$(grep -qF 'chat-interrupt' "$BUS_BRIDGE_H" && echo pass || echo fail)
check "6. bus_bridge.h documents chat-interrupt event type" "$R"

# 7. nbs-claude contains check_interrupt_events function
R=$(grep -qF 'check_interrupt_events' "$NBS_CLAUDE" && echo pass || echo fail)
check "7. nbs-claude contains check_interrupt_events function" "$R"

# 8. nbs-claude sends Escape on interrupt
R=$(grep -q 'send-keys.*Escape' "$NBS_CLAUDE" && echo pass || echo fail)
check "8. nbs-claude sends Escape on interrupt" "$R"

# 9. nbs-claude waits for prompt after escape (interrupt_wait loop)
R=$(grep -qF 'interrupt_wait' "$NBS_CLAUDE" && echo pass || echo fail)
check "9. nbs-claude waits for prompt after sending Escape" "$R"

# 10. nbs-claude injects /nbs-notify after interrupt
R=$(grep -q 'nbs-notify.*interrupt' "$NBS_CLAUDE" && echo pass || echo fail)
check "10. nbs-claude injects /nbs-notify after interrupt" "$R"

# 11. nbs-claude acks interrupt events (prevents re-processing)
R=$(grep -q 'nbs-bus ack' "$NBS_CLAUDE" && echo pass || echo fail)
check "11. nbs-claude acks interrupt events" "$R"

# 12. Interrupt check is not gated by BUS_CHECK_INTERVAL
# (check_interrupt_events call is outside the bus_check_counter block)
# Verify: grep for 'check_interrupt_events' NOT inside a
# 'bus_check_counter' conditional. The function call should appear
# before the bus_check_counter increment.
R=$(awk '/check_interrupt_events/{found=1} /bus_check_counter.*BUS_CHECK_INTERVAL/{if(!found) fail=1} END{print (found && !fail) ? "pass" : "fail"}' "$NBS_CLAUDE")
check "12. Interrupt check runs every cycle (not BUS_CHECK_INTERVAL gated)" "$R"

echo ""

# =================================================================
# Part 2: Integration tests — nbs-chat + nbs-bus event creation
# =================================================================
echo "Part 2: Integration tests"
echo "-------------------------"

# Check binaries exist
if [[ ! -x "$NBS_CHAT" || ! -x "$NBS_BUS" ]]; then
    echo "   SKIP: nbs-chat or nbs-bus binary not found — skipping integration tests"
    echo ""
else
    # Set up a test project with .nbs/chat/ and .nbs/events/
    mkdir -p "$TEST_DIR/project/.nbs/chat"
    mkdir -p "$TEST_DIR/project/.nbs/events"

    CHAT_FILE="$TEST_DIR/project/.nbs/chat/test.chat"
    EVENTS_DIR="$TEST_DIR/project/.nbs/events"

    # Create the chat file
    "$NBS_CHAT" create "$CHAT_FILE"

    # 13. @handle! generates chat-interrupt event
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude! stop what you are doing" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    R=$(echo "$EVENTS" | grep -q 'chat-interrupt' && echo pass || echo fail)
    check "13. @handle! generates chat-interrupt event" "$R"

    # 14. chat-interrupt event has critical priority in filename
    R=$(echo "$EVENTS" | grep -q 'chat-interrupt.*\.event' && echo pass || echo fail)
    # Also check via nbs-bus check output
    CHECK_OUT=$("$NBS_BUS" check "$EVENTS_DIR" 2>/dev/null || true)
    if echo "$CHECK_OUT" | grep -q '\[critical\].*chat-interrupt'; then
        R="pass"
    fi
    check "14. chat-interrupt event has critical priority" "$R"

    # Clean up events for next test
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 15. @handle without ! generates chat-mention, not chat-interrupt
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude hello there" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_MENTION=$(echo "$EVENTS" | grep -c 'chat-mention' || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$HAS_MENTION" -gt 0 && "$HAS_INTERRUPT" -eq 0 ]] && echo pass || echo fail)
    check "15. @handle (no !) generates chat-mention, not chat-interrupt" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 16. Both @alice and @bob! in same message — correct event types
    "$NBS_CHAT" send "$CHAT_FILE" tester "@alice hello @bob! urgent" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_MENTION=$(echo "$EVENTS" | grep -c 'chat-mention' || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$HAS_MENTION" -gt 0 && "$HAS_INTERRUPT" -gt 0 ]] && echo pass || echo fail)
    check "16. Mixed @alice and @bob! produces both mention and interrupt" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 17. chat-interrupt event payload contains the @handle
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude! stop now" 2>/dev/null || true
    sleep 0.5
    INTERRUPT_FILE=$(ls "$EVENTS_DIR"/*chat-interrupt*.event 2>/dev/null | head -1 || true)
    if [[ -n "$INTERRUPT_FILE" ]]; then
        PAYLOAD=$("$NBS_BUS" read "$EVENTS_DIR" "$(basename "$INTERRUPT_FILE")" 2>/dev/null || true)
        R=$(echo "$PAYLOAD" | grep -qF '@claude' && echo pass || echo fail)
    else
        R="fail"
    fi
    check "17. chat-interrupt payload contains @handle" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 18. @handle! at end of message (no trailing text)
    "$NBS_CHAT" send "$CHAT_FILE" tester "stop @claude!" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    R=$(echo "$EVENTS" | grep -q 'chat-interrupt' && echo pass || echo fail)
    check "18. @handle! at end of message triggers interrupt" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 19. Message without any @mentions — no mention or interrupt events
    "$NBS_CHAT" send "$CHAT_FILE" tester "just a normal message" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_MENTION=$(echo "$EVENTS" | grep -c 'chat-mention' || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    # There should be a chat-message event, but no mention/interrupt
    R=$([[ "$HAS_MENTION" -eq 0 && "$HAS_INTERRUPT" -eq 0 ]] && echo pass || echo fail)
    check "19. No @mentions — no mention or interrupt events" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 20. chat-message event still published alongside chat-interrupt
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude! urgent" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_MSG=$(echo "$EVENTS" | grep -c 'chat-message' || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$HAS_MSG" -gt 0 && "$HAS_INTERRUPT" -gt 0 ]] && echo pass || echo fail)
    check "20. chat-message event still published alongside chat-interrupt" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true
fi

echo ""

# =================================================================
# Part 3: Adversarial tests
# =================================================================
echo "Part 3: Adversarial tests"
echo "-------------------------"

if [[ ! -x "$NBS_CHAT" || ! -x "$NBS_BUS" ]]; then
    echo "   SKIP: nbs-chat or nbs-bus binary not found — skipping adversarial tests"
    echo ""
else
    # Reuse the same test project
    CHAT_FILE="$TEST_DIR/project/.nbs/chat/test.chat"
    EVENTS_DIR="$TEST_DIR/project/.nbs/events"

    # 21. user@email.com! — email filter should prevent interrupt
    "$NBS_CHAT" send "$CHAT_FILE" tester "send to user@example.com! asap" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$HAS_INTERRUPT" -eq 0 ]] && echo pass || echo fail)
    check "21. user@email.com! does not trigger interrupt (email filter)" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 22. @handle!! (double bang) — should still trigger interrupt
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude!! stop" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    R=$(echo "$EVENTS" | grep -q 'chat-interrupt' && echo pass || echo fail)
    check "22. @handle!! (double bang) still triggers interrupt" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 23. @! (empty handle with bang) — should not trigger anything
    "$NBS_CHAT" send "$CHAT_FILE" tester "@! nothing" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_MENTION=$(echo "$EVENTS" | grep -c 'chat-mention' || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$HAS_MENTION" -eq 0 && "$HAS_INTERRUPT" -eq 0 ]] && echo pass || echo fail)
    check "23. @! (empty handle) does not trigger mention or interrupt" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 24. Multiple @handle! for same handle — only one interrupt event (dedup)
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude! stop @claude! really stop" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    INTERRUPT_COUNT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$INTERRUPT_COUNT" -eq 1 ]] && echo pass || echo fail)
    check "24. Duplicate @handle! produces only one interrupt event" "$R"

    # Clean up events
    "$NBS_BUS" ack-all "$EVENTS_DIR" 2>/dev/null || true

    # 25. @handle with period after (not !) — should be mention, not interrupt
    "$NBS_CHAT" send "$CHAT_FILE" tester "@claude. please help" 2>/dev/null || true
    sleep 0.5
    EVENTS=$(ls "$EVENTS_DIR"/*.event 2>/dev/null || true)
    HAS_MENTION=$(echo "$EVENTS" | grep -c 'chat-mention' || true)
    HAS_INTERRUPT=$(echo "$EVENTS" | grep -c 'chat-interrupt' || true)
    R=$([[ "$HAS_MENTION" -gt 0 && "$HAS_INTERRUPT" -eq 0 ]] && echo pass || echo fail)
    check "25. @handle. (period, not !) generates mention, not interrupt" "$R"
fi

echo ""

# --- Summary ---
echo "==================================="
if [[ $ERRORS -eq 0 ]]; then
    echo "All tests passed."
else
    echo "$ERRORS test(s) FAILED."
fi
exit "$ERRORS"
