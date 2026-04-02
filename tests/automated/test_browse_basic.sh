#!/bin/bash
# Test: nbs-chat-terminal /browse command — basic open/exit
#
# Verifies:
#   BR1. /browse opens full-screen view (shows indexed message list)
#   BR2. q exits browse mode cleanly
#   BR3. Escape exits browse mode cleanly
#   BR4. /browse on empty chat does not crash
#   BR5. Header shows message count
#   BR6. Help hint bar visible
#
# Note: nbs-ts send appends \r to input. For chatview interaction we write
# raw bytes to the session input FIFO to avoid the stray Enter.

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

# Send raw bytes to chatview (no appended \r)
raw_send() {
    local handle="$1"
    shift
    printf "$@" > ~/.nbs-ts/sessions/$handle/input.fifo
}

# Capture current chatview screen via full output log
capture() {
    local handle="$1"
    cat ~/.nbs-ts/sessions/$handle/output.log | "$NBS_TS_RENDER" --no-strip --width=80 --height=24
}

# Enter browse mode from the terminal
enter_browse() {
    local handle="$1"
    # nbs-ts send appends \r which submits the /browse command
    "$NBS_TS" send "$handle" "/browse"
    sleep 3
}

echo "=== nbs-chat-terminal /browse Basic Test ==="
echo ""

TMPDIR=$(mktemp -d)

CHAT="$TMPDIR/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" alice "Hello from Alice"
"$NBS_CHAT" send "$CHAT" bob "Hello from Bob"
"$NBS_CHAT" send "$CHAT" alice "Third message here"
"$NBS_CHAT" send "$CHAT" charlie "Fourth from Charlie"
"$NBS_CHAT" send "$CHAT" bob "Fifth message from Bob"

# BR1: /browse opens and shows message list
echo "BR1. /browse opens and shows indexed message list..."
HANDLE=$("$NBS_TS" create --name=browse-basic "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2

enter_browse "$HANDLE"
OUTPUT=$(capture "$HANDLE")

if echo "$OUTPUT" | grep -q "\[  1\]" && echo "$OUTPUT" | grep -q "\[  2\]"; then
    pass "Browse shows indexed message list"
else
    fail "Browse missing indexed messages"
    echo "   Output: $(echo "$OUTPUT" | head -5)"
fi

# BR5: Header shows message count
echo "BR5. Header shows message count..."
if echo "$OUTPUT" | grep -q "5 messages"; then
    pass "Header shows '5 messages'"
else
    fail "Header missing message count"
    echo "   Header: $(echo "$OUTPUT" | head -1)"
fi

# BR6: Help hint bar
echo "BR6. Help hint bar visible..."
if echo "$OUTPUT" | grep -q "q:quit"; then
    pass "Help hint bar shows q:quit"
else
    fail "Help hint bar missing"
    echo "   Last 2 lines: $(echo "$OUTPUT" | tail -2)"
fi

# BR2: q exits browse mode
echo "BR2. q exits browse mode..."
raw_send "$HANDLE" "q"
sleep 2

STATUS=$("$NBS_TS" status "$HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "q exits browse (terminal alive)"
else
    fail "Terminal died after q"
fi

"$NBS_TS" send "$HANDLE" $'\x04'
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true

# BR3: Escape exits browse mode
echo "BR3. Escape exits browse mode..."
HANDLE2=$("$NBS_TS" create --name=browse-esc "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 2

enter_browse "$HANDLE2"
raw_send "$HANDLE2" '\033'
sleep 2

STATUS2=$("$NBS_TS" status "$HANDLE2" 2>&1)
if echo "$STATUS2" | grep -q "alive"; then
    pass "Escape exits browse (terminal alive)"
else
    fail "Terminal died after Escape"
fi

"$NBS_TS" send "$HANDLE2" $'\x04'
"$NBS_TS" wait-complete "$HANDLE2" --timeout=5 2>/dev/null || true

# BR4: /browse on empty chat
echo "BR4. /browse on empty chat does not crash..."
EMPTY="$TMPDIR/empty.chat"
"$NBS_CHAT" create "$EMPTY" >/dev/null

HANDLE3=$("$NBS_TS" create --name=browse-empty "$NBS_TERMINAL $EMPTY tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE3")
sleep 2

enter_browse "$HANDLE3"

STATUS3=$("$NBS_TS" status "$HANDLE3" 2>&1)
if echo "$STATUS3" | grep -q "alive"; then
    pass "Empty chat /browse did not crash"
else
    fail "Crashed on empty chat /browse"
fi

raw_send "$HANDLE3" "q"
sleep 1
"$NBS_TS" send "$HANDLE3" $'\x04'
"$NBS_TS" wait-complete "$HANDLE3" --timeout=5 2>/dev/null || true

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
