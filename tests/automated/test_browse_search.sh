#!/bin/bash
# Test: nbs-chat-terminal /browse search
#
# Verifies:
#   BS1. /browse <pattern> opens with cursor on first match
#   BS2. n moves to next match
#   BS3. / search inside browse finds pattern
#   BS4. Search pattern shown in status bar

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

echo "=== nbs-chat-terminal /browse Search Test ==="
echo ""

TMPDIR=$(mktemp -d)

CHAT="$TMPDIR/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" alice "The quick brown fox"
"$NBS_CHAT" send "$CHAT" bob "A boring message"
"$NBS_CHAT" send "$CHAT" charlie "Another boring one"
"$NBS_CHAT" send "$CHAT" alice "The quick brown fox again"
"$NBS_CHAT" send "$CHAT" bob "Something completely different"
"$NBS_CHAT" send "$CHAT" alice "The quick brown fox third"
"$NBS_CHAT" send "$CHAT" charlie "Final message here"

# BS1: /browse <pattern> opens at first match
echo "BS1. /browse <pattern> opens at first match..."
HANDLE=$("$NBS_TS" create --name=browse-search "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2

"$NBS_TS" send "$HANDLE" "/browse quick"
sleep 3

OUTPUT=$(capture "$HANDLE")
if echo "$OUTPUT" | grep -q " 1/7"; then
    pass "/browse quick: cursor at message 1"
else
    fail "/browse quick did not jump to first match"
    echo "   Status: $(echo "$OUTPUT" | grep -oE '[0-9]+/7')"
fi

# BS4: Search pattern in status bar
echo "BS4. Search pattern in status bar..."
if echo "$OUTPUT" | grep -q "quick"; then
    pass "Status bar shows 'quick'"
else
    fail "Status bar missing search pattern"
    echo "   Last 3: $(echo "$OUTPUT" | tail -3)"
fi

# BS2: n moves to next match
echo "BS2. n moves to next match..."
raw_send "$HANDLE" "n"
sleep 1
OUTPUT2=$(capture "$HANDLE")
if echo "$OUTPUT2" | grep -q " 4/7"; then
    pass "n: moved to second match at 4/7"
else
    fail "n did not move to next match"
    echo "   Status: $(echo "$OUTPUT2" | grep -oE '[0-9]+/7')"
fi

# n again — third match
raw_send "$HANDLE" "n"
sleep 1
OUTPUT3=$(capture "$HANDLE")
if echo "$OUTPUT3" | grep -q " 6/7"; then
    pass "n: moved to third match at 6/7"
else
    fail "n did not move to third match"
    echo "   Status: $(echo "$OUTPUT3" | grep -oE '[0-9]+/7')"
fi

raw_send "$HANDLE" "q"
sleep 1
"$NBS_TS" send "$HANDLE" $'\x04'
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true

# BS3: / search inside browse
echo "BS3. / search inside browse..."
HANDLE2=$("$NBS_TS" create --name=browse-slash "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 2

"$NBS_TS" send "$HANDLE2" "/browse"
sleep 3

# Type / to open search, then "different" and Enter (raw, no extra \r)
raw_send "$HANDLE2" "/"
sleep 1
raw_send "$HANDLE2" "different\r"
sleep 2

OUTPUT4=$(capture "$HANDLE2")
if echo "$OUTPUT4" | grep -q " 5/7"; then
    pass "/ search: found 'different' at 5/7"
else
    fail "/ search did not find pattern"
    echo "   Status: $(echo "$OUTPUT4" | grep -oE '[0-9]+/7')"
fi

raw_send "$HANDLE2" "q"
sleep 1
"$NBS_TS" send "$HANDLE2" $'\x04'
"$NBS_TS" wait-complete "$HANDLE2" --timeout=5 2>/dev/null || true

# BS5: ? backward search
echo "BS5. ? backward search..."
HANDLE3=$("$NBS_TS" create --name=browse-back "$NBS_TERMINAL $CHAT tester" | tr -d '[:space:]')
HANDLES+=("$HANDLE3")
sleep 2

"$NBS_TS" send "$HANDLE3" "/browse"
sleep 3

# Go to end first, then search backward for "quick"
raw_send "$HANDLE3" '\033[F'
sleep 1
raw_send "$HANDLE3" "?"
sleep 1
raw_send "$HANDLE3" "quick\r"
sleep 2

OUTPUT5=$(capture "$HANDLE3")
# Should find last occurrence of "quick" (message 6) searching backward from end
if echo "$OUTPUT5" | grep -q " 6/7"; then
    pass "? backward search: found last 'quick' at 6/7"
else
    fail "? backward search did not find pattern"
    echo "   Status: $(echo "$OUTPUT5" | grep -oE '[0-9]+/7')"
fi

# BS6: N after backward search goes to previous match
echo "BS6. N after ? goes to previous match..."
raw_send "$HANDLE3" "N"
sleep 1
OUTPUT6=$(capture "$HANDLE3")
if echo "$OUTPUT6" | grep -q " 4/7"; then
    pass "N after ?: moved to previous match at 4/7"
else
    fail "N did not move to previous match"
    echo "   Status: $(echo "$OUTPUT6" | grep -oE '[0-9]+/7')"
fi

# BS7: Hint bar shows search keys
echo "BS7. Hint bar shows search keys..."
if echo "$OUTPUT6" | grep -q "/:search" && echo "$OUTPUT6" | grep -q "n:next\|N:prev\|search-back"; then
    pass "Hint bar shows search keys"
else
    fail "Hint bar missing search keys"
    echo "   Last 2: $(echo "$OUTPUT6" | tail -2)"
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
