#!/bin/bash
# Test: nbs-chat-terminal /browse navigation
#
# Verifies:
#   BN1. Home goes to first message (1/N)
#   BN2. Down arrow moves cursor down
#   BN3. End goes to last message (N/N)
#   BN4. Up arrow moves cursor up
#   BN5. Page Down scrolls forward
#   BN6. Status bar shows cursor position

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

echo "=== nbs-chat-terminal /browse Navigation Test ==="
echo ""

TMPDIR=$(mktemp -d)

CHAT="$TMPDIR/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
for i in $(seq 1 30); do
    "$NBS_CHAT" send "$CHAT" "user$((i % 5))" "Test message number $i unique"
done

HANDLE=$("$NBS_TS" create --name=browse-nav "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2

# Enter browse mode (nbs-ts send appends \r for submit)
"$NBS_TS" send "$HANDLE" "/browse"
sleep 3

# BN1: Home goes to first message
echo "BN1. Home goes to first message..."
raw_send "$HANDLE" '\033[H'
sleep 1
OUTPUT=$(capture "$HANDLE")
if echo "$OUTPUT" | grep -q " 1/30"; then
    pass "Home: position 1/30"
else
    fail "Home did not reach first message"
    echo "   Status: $(echo "$OUTPUT" | grep -oE '[0-9]+/30')"
fi

# BN2: Down arrow moves cursor
echo "BN2. Down arrow moves cursor..."
raw_send "$HANDLE" '\033[B'
sleep 0.5
raw_send "$HANDLE" '\033[B'
sleep 0.5
raw_send "$HANDLE" '\033[B'
sleep 1
OUTPUT2=$(capture "$HANDLE")
if echo "$OUTPUT2" | grep -q " 4/30"; then
    pass "Down x3: position 4/30"
else
    fail "Down arrow did not advance"
    echo "   Status: $(echo "$OUTPUT2" | grep -oE '[0-9]+/30')"
fi

# BN3: End goes to last message
echo "BN3. End goes to last message..."
raw_send "$HANDLE" '\033[F'
sleep 1
OUTPUT3=$(capture "$HANDLE")
if echo "$OUTPUT3" | grep -q " 30/30"; then
    pass "End: position 30/30"
else
    fail "End did not reach last message"
    echo "   Status: $(echo "$OUTPUT3" | grep -oE '[0-9]+/30')"
fi

# BN4: Up arrow moves cursor up
echo "BN4. Up arrow moves cursor up..."
raw_send "$HANDLE" '\033[A'
sleep 1
OUTPUT4=$(capture "$HANDLE")
if echo "$OUTPUT4" | grep -q " 29/30"; then
    pass "Up: position 29/30"
else
    fail "Up arrow did not move back"
    echo "   Status: $(echo "$OUTPUT4" | grep -oE '[0-9]+/30')"
fi

# BN5: Page Down scrolls forward
echo "BN5. Page Down scrolls forward..."
raw_send "$HANDLE" '\033[H'
sleep 1
raw_send "$HANDLE" '\033[6~'
sleep 1
OUTPUT5=$(capture "$HANDLE")
POS=$(echo "$OUTPUT5" | grep -oE '[0-9]+/30' | head -1)
if [ -n "$POS" ] && [ "$POS" != "1/30" ]; then
    pass "Page Down: moved to $POS"
else
    fail "Page Down did not advance"
    echo "   Status: $POS"
fi

# BN6: Status bar position format
echo "BN6. Status bar shows N/30..."
if echo "$OUTPUT5" | grep -qE "[0-9]+/30"; then
    pass "Status bar shows N/30"
else
    fail "Status bar missing position"
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
