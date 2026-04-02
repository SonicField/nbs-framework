#!/bin/bash
# Test: nbs-chat-terminal /browse live update during browsing
#
# Verifies:
#   BU1. New messages sent while browsing are detected (message count increases)
#   BU2. Header shows "(N new)" indicator after new messages arrive
#   BU3. End key reveals the new messages

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_TS_RENDER="$PROJECT_ROOT/bin/nbs-ts-render"

[ -x "$NBS_TERMINAL" ] || { echo "SKIP: nbs-chat-terminal not found"; exit 0; }
[ -x "$NBS_CHAT" ] || { echo "SKIP: nbs-chat not found"; exit 0; }
[ -x "$NBS_TS" ] || { echo "SKIP: nbs-ts not found"; exit 0; }
[ -x "$NBS_TS_RENDER" ] || { echo "SKIP: nbs-ts-render not found"; exit 0; }

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

raw_send() {
    local handle="$1"; shift
    printf "$@" > ~/.nbs-ts/sessions/$handle/input.fifo
}

capture() {
    cat ~/.nbs-ts/sessions/$1/output.log | "$NBS_TS_RENDER" --no-strip --width=80 --height=24
}

echo "=== nbs-chat-terminal /browse Live Update Test ==="
echo ""

TMPDIR=$(mktemp -d)

CHAT="$TMPDIR/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" alice "Message 1"
"$NBS_CHAT" send "$CHAT" bob "Message 2"
"$NBS_CHAT" send "$CHAT" alice "Message 3"

# Enter browse mode
HANDLE=$("$NBS_TS" create --name=browse-update "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2

"$NBS_TS" send "$HANDLE" "/browse"
sleep 3

# Verify initial state
OUTPUT_BEFORE=$(capture "$HANDLE")
if echo "$OUTPUT_BEFORE" | grep -q "3 messages"; then
    pass "Initial: 3 messages"
else
    fail "Initial state wrong"
    echo "   Header: $(echo "$OUTPUT_BEFORE" | head -1)"
fi

# BU1: Send new messages while browsing
echo "BU1. New messages detected during browse..."
"$NBS_CHAT" send "$CHAT" charlie "New message 4 during browse"
"$NBS_CHAT" send "$CHAT" dave "New message 5 during browse"

# Wait for poll callback to fire (~1.5s poll interval in chatview)
sleep 5

OUTPUT_AFTER=$(capture "$HANDLE")
if echo "$OUTPUT_AFTER" | grep -q "5 messages"; then
    pass "Message count updated to 5"
else
    fail "Message count not updated"
    echo "   Header: $(echo "$OUTPUT_AFTER" | head -1)"
fi

# BU2: Header shows new message indicator
echo "BU2. Header shows new message indicator..."
if echo "$OUTPUT_AFTER" | grep -q "new\|2 new"; then
    pass "Header shows new message indicator"
else
    fail "No new message indicator in header"
    echo "   Header: $(echo "$OUTPUT_AFTER" | head -1)"
fi

# BU3: End key shows new messages
echo "BU3. End key reveals new messages..."
raw_send "$HANDLE" '\033[F'
sleep 1

OUTPUT_END=$(capture "$HANDLE")
if echo "$OUTPUT_END" | grep -q "5/5"; then
    pass "End: position 5/5 (new messages visible)"
else
    fail "End did not reach new messages"
    echo "   Status: $(echo "$OUTPUT_END" | grep -oE '[0-9]+/[0-9]+')"
fi

# Verify new message content is visible
if echo "$OUTPUT_END" | grep -q "New message\|during browse"; then
    pass "New message content visible"
else
    fail "New message content not visible"
    echo "   Last 5 lines: $(echo "$OUTPUT_END" | tail -5)"
fi

# Cleanup
raw_send "$HANDLE" "q"
sleep 1
"$NBS_TS" send "$HANDLE" $'\x04'
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
