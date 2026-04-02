#!/bin/bash
# Test: nbs-chat-edit regression after libchatview refactor
#
# Verifies editor key handler callbacks still work correctly:
#   ER1. Editor opens with correct message count and cursor at end
#   ER2. d marks message for deletion (dirty indicator appears)
#   ER3. u undoes the deletion
#   ER4. d + w writes changes (message removed from file)
#   ER5. Navigation (Home/End/Down) works in editor
#   ER6. q with unsaved changes warns instead of quitting
#   ER7. Q force-quits without saving

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_EDIT="${NBS_EDIT_BIN:-$PROJECT_ROOT/bin/nbs-chat-edit}"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_TS_RENDER="$PROJECT_ROOT/bin/nbs-ts-render"

[ -x "$NBS_EDIT" ] || { echo "SKIP: nbs-chat-edit not found"; exit 0; }
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

echo "=== nbs-chat-edit Regression Test ==="
echo ""

TMPDIR=$(mktemp -d)

# Create chat with known messages
CHAT="$TMPDIR/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" alice "First message from Alice"
"$NBS_CHAT" send "$CHAT" bob "Second message from Bob"
"$NBS_CHAT" send "$CHAT" charlie "Third message from Charlie"
"$NBS_CHAT" send "$CHAT" alice "Fourth message from Alice"
"$NBS_CHAT" send "$CHAT" bob "Fifth message from Bob"

# ER1: Editor opens with correct state
echo "ER1. Editor opens with correct message count..."
HANDLE=$("$NBS_TS" create --name=edit-regr "$NBS_EDIT $CHAT" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2

OUTPUT=$(capture "$HANDLE")
if echo "$OUTPUT" | grep -q "5 messages" && echo "$OUTPUT" | grep -q "5/5"; then
    pass "Editor shows 5 messages, cursor at 5/5"
else
    fail "Editor initial state wrong"
    echo "   Header: $(echo "$OUTPUT" | head -1)"
    echo "   Status: $(echo "$OUTPUT" | grep -oE '[0-9]+/[0-9]+')"
fi

# ER5: Navigation works
echo "ER5. Navigation (Home/Down) works..."
raw_send "$HANDLE" '\033[H'
sleep 1
OUTPUT2=$(capture "$HANDLE")
if echo "$OUTPUT2" | grep -q "1/5"; then
    pass "Home: position 1/5"
else
    fail "Home did not work"
    echo "   Status: $(echo "$OUTPUT2" | grep -oE '[0-9]+/5')"
fi

raw_send "$HANDLE" '\033[B'
sleep 1
OUTPUT3=$(capture "$HANDLE")
if echo "$OUTPUT3" | grep -q "2/5"; then
    pass "Down: position 2/5"
else
    fail "Down did not work"
    echo "   Status: $(echo "$OUTPUT3" | grep -oE '[0-9]+/5')"
fi

# ER2: d marks message for deletion
echo "ER2. d marks message for deletion..."
raw_send "$HANDLE" "d"
sleep 1
OUTPUT4=$(capture "$HANDLE")
if echo "$OUTPUT4" | grep -q "modified\|delete\|1 to delete"; then
    pass "d marks message (dirty indicator visible)"
else
    fail "d did not mark message"
    echo "   Output: $(echo "$OUTPUT4" | grep -i "modif\|delete\|dirty")"
fi

# ER3: u undoes the deletion
echo "ER3. u undoes the deletion..."
raw_send "$HANDLE" "u"
sleep 1
OUTPUT5=$(capture "$HANDLE")
if echo "$OUTPUT5" | grep -qi "undo"; then
    pass "u undoes deletion (Undo status shown)"
else
    # Check if modified indicator is gone (also valid)
    if ! echo "$OUTPUT5" | grep -q "modified"; then
        pass "u undoes deletion (modified indicator gone)"
    else
        fail "u did not undo"
        echo "   Status: $(echo "$OUTPUT5" | tail -2)"
    fi
fi

# ER6: q with unsaved changes warns
echo "ER6. q with unsaved changes warns..."
# Mark a message for deletion first
raw_send "$HANDLE" '\033[H'
sleep 0.5
raw_send "$HANDLE" "d"
sleep 1
# Now try q
raw_send "$HANDLE" "q"
sleep 1
OUTPUT6=$(capture "$HANDLE")
if echo "$OUTPUT6" | grep -qi "unsaved\|force quit\|press Q"; then
    pass "q warns about unsaved changes"
else
    fail "q did not warn"
    echo "   Last 2 lines: $(echo "$OUTPUT6" | tail -2)"
fi

# ER7: Q force-quits
echo "ER7. Q force-quits without saving..."
raw_send "$HANDLE" "Q"
sleep 2

STATUS=$("$NBS_TS" status "$HANDLE" 2>&1)
if echo "$STATUS" | grep -q "dead"; then
    EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE" 2>&1) || EXIT_CODE="unknown"
    if [ "$EXIT_CODE" = "0" ]; then
        pass "Q force-quit with exit code 0"
    else
        pass "Q force-quit with exit code $EXIT_CODE"
    fi
else
    fail "Q did not quit editor"
fi

# Verify file unchanged (Q = no save)
MSG_COUNT=$("$NBS_CHAT" read "$CHAT" 2>&1 | grep -c "^\[")
if [ "$MSG_COUNT" -eq 5 ]; then
    pass "File unchanged after Q (still 5 messages)"
else
    fail "File was modified despite Q ($MSG_COUNT messages)"
fi

# ER4: d + w writes changes (new session)
echo "ER4. d + w writes changes to file..."
CHAT2="$TMPDIR/test2.chat"
cp "$CHAT" "$CHAT2"

HANDLE2=$("$NBS_TS" create --name=edit-write "$NBS_EDIT $CHAT2" | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 2

# Go to message 3, mark for deletion, write
raw_send "$HANDLE2" '\033[H'
sleep 0.5
raw_send "$HANDLE2" '\033[B'
sleep 0.5
raw_send "$HANDLE2" '\033[B'
sleep 0.5
# Now at position 3
raw_send "$HANDLE2" "d"
sleep 1
raw_send "$HANDLE2" "w"
sleep 2

OUTPUT7=$(capture "$HANDLE2")
if echo "$OUTPUT7" | grep -qi "written\|4 messages"; then
    pass "w wrote changes (message deleted)"
else
    fail "w did not write"
    echo "   Last 2 lines: $(echo "$OUTPUT7" | tail -2)"
fi

# Quit and verify file
raw_send "$HANDLE2" "q"
sleep 2

# Verify the file now has 4 messages
MSG_COUNT2=$("$NBS_CHAT" read "$CHAT2" 2>&1 | grep -c "^\[")
if [ "$MSG_COUNT2" -eq 4 ]; then
    pass "File has 4 messages after w (was 5, deleted 1)"
else
    fail "File message count wrong: expected 4, got $MSG_COUNT2"
fi

# Verify the deleted message (charlie's "Third message") is gone
if ! "$NBS_CHAT" read "$CHAT2" 2>&1 | grep -q "Third message"; then
    pass "Deleted message content not in file"
else
    fail "Deleted message still in file"
fi

# ER8: t truncates from cursor to end
echo "ER8. t truncates from cursor to end..."
CHAT3="$TMPDIR/test3.chat"
"$NBS_CHAT" create "$CHAT3" >/dev/null
for i in $(seq 1 5); do "$NBS_CHAT" send "$CHAT3" "user$i" "Trunc msg $i"; done

HANDLE3=$("$NBS_TS" create --name=edit-trunc "$NBS_EDIT $CHAT3" | tr -d '[:space:]')
HANDLES+=("$HANDLE3")
sleep 2

# Go to message 3, truncate (marks 3,4,5 for deletion)
raw_send "$HANDLE3" '\033[H'
sleep 0.5
raw_send "$HANDLE3" '\033[B'
sleep 0.5
raw_send "$HANDLE3" '\033[B'
sleep 0.5
raw_send "$HANDLE3" "t"
sleep 1

OUTPUT_T=$(capture "$HANDLE3")
if echo "$OUTPUT_T" | grep -qi "truncate\|3 messages for deletion\|delete"; then
    pass "t truncates from cursor (status shows truncation)"
else
    fail "t did not truncate"
    echo "   Last 2: $(echo "$OUTPUT_T" | tail -2)"
fi

# Write and verify only 2 messages remain
raw_send "$HANDLE3" "w"
sleep 2
raw_send "$HANDLE3" "q"
sleep 2

MSG_T=$("$NBS_CHAT" read "$CHAT3" 2>&1 | grep -c "^\[")
if [ "$MSG_T" -eq 2 ]; then
    pass "After t+w: 2 messages remain (truncated 3)"
else
    fail "Truncate+write: expected 2 messages, got $MSG_T"
fi

# ER9: Ctrl-R redoes after undo
echo "ER9. Ctrl-R redoes after undo..."
CHAT4="$TMPDIR/test4.chat"
"$NBS_CHAT" create "$CHAT4" >/dev/null
"$NBS_CHAT" send "$CHAT4" alice "Keep this"
"$NBS_CHAT" send "$CHAT4" bob "Delete this"
"$NBS_CHAT" send "$CHAT4" charlie "Keep this too"

HANDLE4=$("$NBS_TS" create --name=edit-redo "$NBS_EDIT $CHAT4" | tr -d '[:space:]')
HANDLES+=("$HANDLE4")
sleep 2

# Go to msg 2, delete, undo, redo
raw_send "$HANDLE4" '\033[H'
sleep 0.5
raw_send "$HANDLE4" '\033[B'
sleep 0.5
raw_send "$HANDLE4" "d"
sleep 1

# Verify deleted
OUTPUT_D=$(capture "$HANDLE4")
DEL_BEFORE=$(echo "$OUTPUT_D" | grep -ci "delete\|modified" || true)

# Undo
raw_send "$HANDLE4" "u"
sleep 1

# Redo (Ctrl-R = 0x12)
printf '\x12' > ~/.nbs-ts/sessions/$HANDLE4/input.fifo
sleep 1

OUTPUT_R=$(capture "$HANDLE4")
if echo "$OUTPUT_R" | grep -qi "redo\|modified\|delete"; then
    pass "Ctrl-R redoes the deletion"
else
    fail "Ctrl-R did not redo"
    echo "   Last 2: $(echo "$OUTPUT_R" | tail -2)"
fi

raw_send "$HANDLE4" "Q"
sleep 2

# ER10: Removed vim bindings (j/k/g/G) are inert
echo "ER10. Removed vim bindings are inert..."
CHAT5="$TMPDIR/test5.chat"
"$NBS_CHAT" create "$CHAT5" >/dev/null
for i in $(seq 1 5); do "$NBS_CHAT" send "$CHAT5" "user$i" "Vim test $i"; done

HANDLE5=$("$NBS_TS" create --name=edit-novim "$NBS_EDIT $CHAT5" | tr -d '[:space:]')
HANDLES+=("$HANDLE5")
sleep 2

# Cursor starts at 5/5. Send 'k' (should do nothing, not move up)
raw_send "$HANDLE5" "k"
sleep 1
OUTPUT_K=$(capture "$HANDLE5")
POS_K=$(echo "$OUTPUT_K" | grep -oE '[0-9]+/5' | head -1)
if [ "$POS_K" = "5/5" ]; then
    pass "k is inert (cursor stays at 5/5)"
else
    fail "k moved cursor to $POS_K (should stay at 5/5)"
fi

# Send 'g' (should do nothing, not go to start)
raw_send "$HANDLE5" "g"
sleep 1
OUTPUT_G=$(capture "$HANDLE5")
POS_G=$(echo "$OUTPUT_G" | grep -oE '[0-9]+/5' | head -1)
if [ "$POS_G" = "5/5" ]; then
    pass "g is inert (cursor stays at 5/5)"
else
    fail "g moved cursor to $POS_G (should stay at 5/5)"
fi

# Move to 1/5 via Home, then send 'j' (should do nothing)
raw_send "$HANDLE5" '\033[H'
sleep 1
raw_send "$HANDLE5" "j"
sleep 1
OUTPUT_J=$(capture "$HANDLE5")
POS_J=$(echo "$OUTPUT_J" | grep -oE '[0-9]+/5' | head -1)
if [ "$POS_J" = "1/5" ]; then
    pass "j is inert (cursor stays at 1/5)"
else
    fail "j moved cursor to $POS_J (should stay at 1/5)"
fi

raw_send "$HANDLE5" "Q"
sleep 2

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
