#!/bin/bash
# Test: chat terminal desync fix and message format grep patterns
#
# Falsifiable tests covering:
#   1. Message format grep pattern: '] handle:' matches, '^handle:' does not
#   2. No message loss during rapid interleaved sends from multiple handles
#   3. Stale ^handle: pattern check across all test files
#
# The read output format is: [timestamp] handle: message
# The old broken pattern ^handle: would never match this format.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

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

echo "=== Chat Desync and Format Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Message format grep pattern ---
echo "1. Message format grep pattern..."
CHAT="$TEST_DIR/format.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" agent-a "hello from a"
"$NBS_CHAT" send "$CHAT" agent-b "hello from b"
OUTPUT=$("$NBS_CHAT" read "$CHAT")

# The correct pattern: '] agent-a:' matches the [timestamp] handle: format
A_COUNT=$(echo "$OUTPUT" | grep -c '] agent-a:' || true)
check "Correct pattern '] agent-a:' matches 1 message" "$( [[ "$A_COUNT" -eq 1 ]] && echo pass || echo fail )"

B_COUNT=$(echo "$OUTPUT" | grep -c '] agent-b:' || true)
check "Correct pattern '] agent-b:' matches 1 message" "$( [[ "$B_COUNT" -eq 1 ]] && echo pass || echo fail )"

# The OLD broken pattern: ^agent-a: should match 0 lines (timestamps come first)
OLD_A_COUNT=$(echo "$OUTPUT" | grep -c '^agent-a:' || true)
check "Old broken pattern '^agent-a:' matches 0" "$( [[ "$OLD_A_COUNT" -eq 0 ]] && echo pass || echo fail )"

OLD_B_COUNT=$(echo "$OUTPUT" | grep -c '^agent-b:' || true)
check "Old broken pattern '^agent-b:' matches 0" "$( [[ "$OLD_B_COUNT" -eq 0 ]] && echo pass || echo fail )"

# Verify the output format is [timestamp] handle: message
FORMAT_OK=$(echo "$OUTPUT" | grep -cE '^\[.*\] agent-[ab]: hello from [ab]$' || true)
check "All lines match [timestamp] handle: message format" "$( [[ "$FORMAT_OK" -eq 2 ]] && echo pass || echo fail )"

echo ""

# --- Test 2: No message loss during rapid interleaved sends ---
echo "2. No message loss during rapid interleaved sends..."
CHAT="$TEST_DIR/interleaved.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

for i in $(seq 1 5); do
    "$NBS_CHAT" send "$CHAT" alice "msg $i"
    "$NBS_CHAT" send "$CHAT" bob "msg $i"
    "$NBS_CHAT" send "$CHAT" carol "msg $i"
done

OUTPUT=$("$NBS_CHAT" read "$CHAT")
TOTAL_COUNT=$(echo "$OUTPUT" | grep -c '.' || true)
check "15 total messages present" "$( [[ "$TOTAL_COUNT" -eq 15 ]] && echo pass || echo fail )"

ALICE_COUNT=$(echo "$OUTPUT" | grep -c '] alice:' || true)
check "alice has exactly 5 messages" "$( [[ "$ALICE_COUNT" -eq 5 ]] && echo pass || echo fail )"

BOB_COUNT=$(echo "$OUTPUT" | grep -c '] bob:' || true)
check "bob has exactly 5 messages" "$( [[ "$BOB_COUNT" -eq 5 ]] && echo pass || echo fail )"

CAROL_COUNT=$(echo "$OUTPUT" | grep -c '] carol:' || true)
check "carol has exactly 5 messages" "$( [[ "$CAROL_COUNT" -eq 5 ]] && echo pass || echo fail )"

# Verify message ordering: within each handle, msg 1 appears before msg 5
ALICE_FIRST=$(echo "$OUTPUT" | grep '] alice:' | head -1)
ALICE_LAST=$(echo "$OUTPUT" | grep '] alice:' | tail -1)
check "alice messages ordered (first is msg 1)" "$( echo "$ALICE_FIRST" | grep -qF 'msg 1' && echo pass || echo fail )"
check "alice messages ordered (last is msg 5)" "$( echo "$ALICE_LAST" | grep -qF 'msg 5' && echo pass || echo fail )"

# Verify interleaving order: alice msg 1, bob msg 1, carol msg 1, alice msg 2, ...
FIRST_THREE=$(echo "$OUTPUT" | head -3)
check "Interleave order: line 1 is alice" "$( echo "$FIRST_THREE" | head -1 | grep -qF '] alice: msg 1' && echo pass || echo fail )"
check "Interleave order: line 2 is bob" "$( echo "$FIRST_THREE" | sed -n '2p' | grep -qF '] bob: msg 1' && echo pass || echo fail )"
check "Interleave order: line 3 is carol" "$( echo "$FIRST_THREE" | tail -1 | grep -qF '] carol: msg 1' && echo pass || echo fail )"

echo ""

# --- Test 3: Stale ^handle: pattern check across test files ---
echo "3. Stale ^handle: pattern check..."
# Search for the old broken patterns in all test scripts (excluding this file).
# These patterns would silently fail to match the [timestamp] handle: format.
SELF="$(basename "${BASH_SOURCE[0]}")"
STALE_COUNT=0

# Check for "^${agent}:" pattern used in grep/code (not comments)
STALE_AGENT=$(grep -rn '"\^\${agent}:' "$SCRIPT_DIR"/*.sh 2>/dev/null | grep -v "$SELF" | wc -l || true)
STALE_COUNT=$((STALE_COUNT + STALE_AGENT))

# Check for "^${sender}:" pattern
STALE_SENDER=$(grep -rn '"\^\${sender}:' "$SCRIPT_DIR"/*.sh 2>/dev/null | grep -v "$SELF" | wc -l || true)
STALE_COUNT=$((STALE_COUNT + STALE_SENDER))

# Check for "^${handle}:" pattern
STALE_HANDLE=$(grep -rn '"\^\${handle}:' "$SCRIPT_DIR"/*.sh 2>/dev/null | grep -v "$SELF" | wc -l || true)
STALE_COUNT=$((STALE_COUNT + STALE_HANDLE))

# Check for "'^worker-" pattern (anchored to start of line)
STALE_WORKER=$(grep -rn "'\^worker-" "$SCRIPT_DIR"/*.sh 2>/dev/null | grep -v "$SELF" | wc -l || true)
STALE_COUNT=$((STALE_COUNT + STALE_WORKER))

check "No stale '^agent:' patterns" "$( [[ "$STALE_AGENT" -eq 0 ]] && echo pass || echo fail )"
check "No stale '^sender:' patterns" "$( [[ "$STALE_SENDER" -eq 0 ]] && echo pass || echo fail )"
check "No stale '^handle:' patterns" "$( [[ "$STALE_HANDLE" -eq 0 ]] && echo pass || echo fail )"
check "No stale '^worker-' patterns" "$( [[ "$STALE_WORKER" -eq 0 ]] && echo pass || echo fail )"
check "Zero total stale patterns found" "$( [[ "$STALE_COUNT" -eq 0 ]] && echo pass || echo fail )"

echo ""

# --- Summary ---
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
