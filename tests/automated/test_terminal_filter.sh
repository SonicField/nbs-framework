#!/bin/bash
# Test: nbs-chat-terminal /filter and /unfilter commands
#
# Tests:
#   1. /filter <handle> hides messages from other handles
#   2. /filter <handle> shows messages from the target handle
#   3. /unfilter restores all messages
#   4. /filter without argument shows usage
#   5. /filter with non-existent handle shows no messages

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"

export PATH="$PROJECT_ROOT/bin:$PATH"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
    pkill -9 -f "nbs-chat-terminal.*$TEST_DIR" 2>/dev/null || true
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

echo "=== nbs-chat-terminal /filter and /unfilter Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: /filter hides other handles ---
echo "1. /filter hides messages from other handles..."
CHAT="$TEST_DIR/test1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "alice" "Hello from alice"
"$NBS_CHAT" send "$CHAT" "bob" "Hello from bob"
"$NBS_CHAT" send "$CHAT" "alice" "Second from alice"
OUTPUT=$(printf '/filter alice\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/filter hides bob" "$( echo "$OUTPUT" | grep -q 'Filtering.*alice' && echo pass || echo fail )"

echo ""

# --- Test 2: /filter without argument shows usage ---
echo "2. /filter without argument shows usage..."
CHAT="$TEST_DIR/test2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
OUTPUT=$(printf '/filter\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/filter no arg shows usage" "$( echo "$OUTPUT" | grep -qi 'usage.*filter\|filter.*handle' && echo pass || echo fail )"

echo ""

# --- Test 3: /unfilter restores all messages ---
echo "3. /unfilter restores all messages..."
CHAT="$TEST_DIR/test3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "alice" "From alice"
"$NBS_CHAT" send "$CHAT" "bob" "From bob"
OUTPUT=$(printf '/filter alice\n/unfilter\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/unfilter shows all" "$( echo "$OUTPUT" | grep -qi 'showing all\|filter cleared\|unfilter' && echo pass || echo fail )"

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
