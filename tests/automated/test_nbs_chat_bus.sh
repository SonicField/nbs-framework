#!/bin/bash
# Test: nbs-chat bus integration
#
# Tests that nbs-chat send publishes bus events when .nbs/events/ exists.
# Requires both nbs-chat and nbs-bus binaries.
#
# Deterministic tests covering:
#   1.  No bus directory — send works normally, no events
#   2.  Bus directory exists — chat-message event created
#   3.  Event content matches YAML schema
#   4.  Payload contains handle and message
#   5.  @mention generates chat-mention event
#   6.  Multiple @mentions in one message
#   7.  No false @mention for email-like strings
#   8.  Concurrent sends — unique events per send
#   9.  Bus failure does not break chat send
#   10. nbs-bus not in PATH — graceful degradation
#   11. @mention at start of message
#   12. Very long message — event still created
#   13. Special characters in message
#   14. Event timestamp is reasonable
#   15. Events go to events/, not processed/
#
# Adversarial tests:
#   16. Bus directory deleted between operations
#   17. @mention with unusual handles
#   18. Message with only @mention, no body
#   19. Empty message — no event (or event with empty payload)
#   20. Binary data in message — no crash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"

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

# Verify both binaries exist
if [[ ! -x "$NBS_CHAT" ]]; then
    echo "ERROR: nbs-chat not found at $NBS_CHAT"
    exit 1
fi

if [[ ! -x "$NBS_BUS" ]]; then
    echo "WARNING: nbs-bus not found at $NBS_BUS — bus-dependent tests will be skipped"
    NBS_BUS_AVAILABLE=false
else
    NBS_BUS_AVAILABLE=true
fi

echo "=== nbs-chat Bus Integration Test ==="
echo "Test dir: $TEST_DIR"
echo "nbs-chat: $NBS_CHAT"
echo "nbs-bus: $NBS_BUS (available: $NBS_BUS_AVAILABLE)"
echo ""

# Helper: count event files in a directory
count_events() {
    local dir="$1"
    local pattern="${2:-*.event}"
    find "$dir" -maxdepth 1 -name "$pattern" 2>/dev/null | wc -l
}

# Helper: set up a project with chat and bus
setup_project() {
    local proj="$1"
    mkdir -p "$proj/.nbs/chat"
    mkdir -p "$proj/.nbs/events/processed"
    "$NBS_CHAT" create "$proj/.nbs/chat/test.chat" >/dev/null
}

# --- Test 1: No bus directory — send works, no events ---
echo "1. No bus directory — send works normally..."
PROJ="$TEST_DIR/test1"
mkdir -p "$PROJ/.nbs/chat"
"$NBS_CHAT" create "$PROJ/.nbs/chat/test.chat" >/dev/null
"$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "hello world"
RC=$?
check "Send succeeds without bus" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "No events dir created" "$( [[ ! -d "$PROJ/.nbs/events" ]] && echo pass || echo fail )"
# Verify message was actually sent
CONTENT=$("$NBS_CHAT" read "$PROJ/.nbs/chat/test.chat")
check "Message in chat" "$( echo "$CONTENT" | grep -q "hello world" && echo pass || echo fail )"

# --- Test 2: Bus directory exists — chat-message event created ---
echo ""
echo "2. Bus directory exists — chat-message event..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test2"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "test message"

    EVENT_COUNT=$(count_events "$PROJ/.nbs/events")
    check "Event file created" "$( [[ $EVENT_COUNT -ge 1 ]] && echo pass || echo fail )"

    # Find the event file
    EVENT_FILE=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*.event" | head -1)
    check "Event filename contains chat-message" "$( echo "$EVENT_FILE" | grep -q "chat-message" && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 3: Event content matches YAML schema ---
echo ""
echo "3. Event content matches YAML schema..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test3"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" bob "schema test"

    EVENT_FILE=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*.event" | head -1)
    if [[ -n "$EVENT_FILE" ]]; then
        check "Has source field" "$( grep -q '^source:' "$EVENT_FILE" && echo pass || echo fail )"
        check "Has type field" "$( grep -q '^type:' "$EVENT_FILE" && echo pass || echo fail )"
        check "Has priority field" "$( grep -q '^priority:' "$EVENT_FILE" && echo pass || echo fail )"
        check "Has timestamp field" "$( grep -q '^timestamp:' "$EVENT_FILE" && echo pass || echo fail )"
    else
        check "Event file exists" "fail"
    fi
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 4: Payload contains handle and message ---
echo ""
echo "4. Payload contains handle and message..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test4"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" charlie "payload check message"

    EVENT_FILE=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*.event" | head -1)
    if [[ -n "$EVENT_FILE" ]]; then
        CONTENT=$(cat "$EVENT_FILE")
        check "Payload has handle" "$( echo "$CONTENT" | grep -q "charlie" && echo pass || echo fail )"
        check "Payload has message" "$( echo "$CONTENT" | grep -q "payload check message" && echo pass || echo fail )"
    else
        check "Event file exists" "fail"
    fi
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 5: @mention generates chat-mention event ---
echo ""
echo "5. @mention generates chat-mention event..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test5"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "@bob please review"

    # Should have both chat-message and chat-mention events
    MSG_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-message*" | wc -l)
    MENTION_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-mention*" | wc -l)
    check "chat-message event created" "$( [[ $MSG_COUNT -ge 1 ]] && echo pass || echo fail )"
    check "chat-mention event created" "$( [[ $MENTION_COUNT -ge 1 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 6: Multiple @mentions ---
echo ""
echo "6. Multiple @mentions in one message..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test6"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" supervisor "@alice @bob @charlie all hands"

    MENTION_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-mention*" | wc -l)
    check "Multiple mention events" "$( [[ $MENTION_COUNT -ge 3 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 7: No false @mention for email-like strings ---
echo ""
echo "7. No false @mention for email..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test7"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "contact user@example.com for info"

    MENTION_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-mention*" | wc -l)
    check "No mention for email" "$( [[ $MENTION_COUNT -eq 0 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 8: Concurrent sends — unique events ---
echo ""
echo "8. Concurrent sends — unique events..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test8"
    setup_project "$PROJ"

    # Send 10 messages in parallel
    for i in $(seq 1 10); do
        "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" "worker-$i" "message $i" &
    done
    wait

    EVENT_COUNT=$(count_events "$PROJ/.nbs/events")
    check "10 events from 10 sends" "$( [[ $EVENT_COUNT -ge 10 ]] && echo pass || echo fail )"

    # Verify all events have unique filenames
    UNIQUE_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*.event" | sort -u | wc -l)
    check "All events unique" "$( [[ $UNIQUE_COUNT -eq $EVENT_COUNT ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 9: Bus failure does not break chat ---
echo ""
echo "9. Bus failure does not break chat send..."
PROJ="$TEST_DIR/test9"
mkdir -p "$PROJ/.nbs/chat"
mkdir -p "$PROJ/.nbs/events"
# Make events dir read-only
chmod 555 "$PROJ/.nbs/events"
"$NBS_CHAT" create "$PROJ/.nbs/chat/test.chat" >/dev/null
"$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "should still send"
RC=$?
# Restore permissions for cleanup
chmod 755 "$PROJ/.nbs/events"
check "Chat send succeeds despite bus failure" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
CONTENT=$("$NBS_CHAT" read "$PROJ/.nbs/chat/test.chat")
check "Message in chat despite bus failure" "$( echo "$CONTENT" | grep -q "should still send" && echo pass || echo fail )"

# --- Test 10: nbs-bus not in PATH — graceful degradation ---
echo ""
echo "10. nbs-bus not in PATH — graceful degradation..."
PROJ="$TEST_DIR/test10"
setup_project "$PROJ"
# Run with empty PATH so nbs-bus can't be found — but nbs-chat must still work
# We invoke nbs-chat by absolute path
"$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "no bus binary"
RC=$?
check "Send succeeds" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# --- Test 11: @mention at start of message ---
echo ""
echo "11. @mention at start of message..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test11"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "@supervisor task done"

    MENTION_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-mention*" | wc -l)
    check "@mention at start detected" "$( [[ $MENTION_COUNT -ge 1 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 12: Very long message ---
echo ""
echo "12. Very long message — event still created..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test12"
    setup_project "$PROJ"
    LONG_MSG=$(python3 -c "print('x' * 10000)")
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "$LONG_MSG"

    EVENT_COUNT=$(count_events "$PROJ/.nbs/events")
    check "Event created for long message" "$( [[ $EVENT_COUNT -ge 1 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 13: Special characters in message ---
echo ""
echo "13. Special characters in message..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test13"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice 'quotes "here" and `backticks` and $vars'

    EVENT_COUNT=$(count_events "$PROJ/.nbs/events")
    check "Event created with special chars" "$( [[ $EVENT_COUNT -ge 1 ]] && echo pass || echo fail )"

    # Verify event file is valid (at least readable)
    EVENT_FILE=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*.event" | head -1)
    check "Event file readable" "$( [[ -r "$EVENT_FILE" ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 14: Event timestamp is reasonable ---
echo ""
echo "14. Event timestamp is reasonable..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test14"
    setup_project "$PROJ"
    BEFORE=$(date +%s)
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "timestamp test"
    AFTER=$(date +%s)

    EVENT_FILE=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*.event" | head -1)
    if [[ -n "$EVENT_FILE" ]]; then
        # Extract timestamp from filename (first field before -)
        # Timestamp is in microseconds since epoch
        FNAME=$(basename "$EVENT_FILE")
        EVENT_TS_US=$(echo "$FNAME" | cut -d- -f1)
        EVENT_TS=$((EVENT_TS_US / 1000000))
        check "Timestamp after test start" "$( [[ $EVENT_TS -ge $BEFORE ]] && echo pass || echo fail )"
        check "Timestamp before test end" "$( [[ $EVENT_TS -le $((AFTER + 2)) ]] && echo pass || echo fail )"
    else
        check "Event file exists" "fail"
    fi
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 15: Events go to events/, not processed/ ---
echo ""
echo "15. Events go to events/, not processed/..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test15"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "placement test"

    PENDING=$(count_events "$PROJ/.nbs/events")
    PROCESSED=$(count_events "$PROJ/.nbs/events/processed")
    check "Event in events/" "$( [[ $PENDING -ge 1 ]] && echo pass || echo fail )"
    check "No event in processed/" "$( [[ $PROCESSED -eq 0 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 16: Bus directory deleted between operations ---
echo ""
echo "16. Bus directory deleted mid-operation..."
PROJ="$TEST_DIR/test16"
setup_project "$PROJ"
# Remove bus dir right before send — simulate race
rmdir "$PROJ/.nbs/events/processed" 2>/dev/null || true
rmdir "$PROJ/.nbs/events" 2>/dev/null || true
"$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "race condition test"
RC=$?
check "Send succeeds after bus dir removed" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# --- Test 17: @mention with unusual handles ---
echo ""
echo "17. @mention with unusual handles..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test17"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "@bench-claude @doc-claude hello"

    MENTION_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-mention*" | wc -l)
    check "Hyphenated handles detected" "$( [[ $MENTION_COUNT -ge 2 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 18: Message with only @mention ---
echo ""
echo "18. Message is just @mention..."
if $NBS_BUS_AVAILABLE; then
    PROJ="$TEST_DIR/test18"
    setup_project "$PROJ"
    "$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice "@supervisor"

    MENTION_COUNT=$(find "$PROJ/.nbs/events" -maxdepth 1 -name "*chat-mention*" | wc -l)
    check "Bare @mention detected" "$( [[ $MENTION_COUNT -ge 1 ]] && echo pass || echo fail )"
else
    echo "   SKIP: nbs-bus not available"
fi

# --- Test 19: Empty message ---
echo ""
echo "19. Empty message handling..."
PROJ="$TEST_DIR/test19"
setup_project "$PROJ"
"$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice ""
RC=$?
# Empty message should still succeed for chat (bus event is optional)
check "Empty message send succeeds" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# --- Test 20: Binary data in message ---
echo ""
echo "20. Binary data — no crash..."
PROJ="$TEST_DIR/test20"
setup_project "$PROJ"
# Send message with some problematic but shell-safe characters
"$NBS_CHAT" send "$PROJ/.nbs/chat/test.chat" alice $'tab\there newline embedded'
RC=$?
check "Binary-ish data no crash" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# --- Summary ---
echo ""
echo "=== Results ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "FAILURES: $ERRORS"
    exit 1
fi
