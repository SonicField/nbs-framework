#!/bin/bash
# Test: nbs-chat gap coverage
#
# Closes 5 testing gaps:
#   Gap 1: Delete subcommand (--dry-run, actual delete, header integrity, no-op, missing --after)
#   Gap 2: Time-based filtering (--after epoch, --before epoch, relative, search+after, invalid)
#   Gap 3: Cursor-on-write (send advances sender cursor, unread for other handles)
#   Gap 4: Message timestamp wire format (base64 decode, epoch, ISO prefix)
#   Gap 5: Web POST /api/send (valid, empty handle, empty body, cleanup)
#
# Exit codes:
#   0 - All tests passed
#   1 - One or more tests failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_WEB="${NBS_WEB_BIN:-$PROJECT_ROOT/bin/nbs-chat-web}"

TEST_DIR=$(mktemp -d)
PASS=0
FAIL=0
WEB_PID=0

killbg() {
    local pid="$1"
    if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    killbg "$WEB_PID"
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $label"
        FAIL=$((FAIL + 1))
    fi
}

TEST_NUM=0
next_test() {
    TEST_NUM=$((TEST_NUM + 1))
    echo ""
    echo "$TEST_NUM. $1"
}

echo "=== nbs-chat Gap Coverage Tests ==="
echo "Test dir: $TEST_DIR"

# =====================================================================
# Gap 1: Delete subcommand
# =====================================================================

next_test "Delete: create chat with 5 messages, record epoch between msg 3 and msg 4"
CHAT="$TEST_DIR/gap1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" alice "delete-msg-1"
sleep 1
"$NBS_CHAT" send "$CHAT" bob "delete-msg-2"
sleep 1
"$NBS_CHAT" send "$CHAT" alice "delete-msg-3"
sleep 1
# Record an epoch AFTER msg 3, BEFORE msg 4
DELETE_EPOCH=$(date +%s)
sleep 1
"$NBS_CHAT" send "$CHAT" bob "delete-msg-4"
sleep 1
"$NBS_CHAT" send "$CHAT" alice "delete-msg-5"

# Verify all 5 present
ALL_COUNT=$("$NBS_CHAT" read "$CHAT" | wc -l)
check "5 messages present before delete" "$( [[ "$ALL_COUNT" -eq 5 ]] && echo pass || echo fail )"

next_test "Delete: --dry-run shows count but does not delete"
DRY_OUTPUT=$("$NBS_CHAT" delete "$CHAT" --after="$DELETE_EPOCH" --dry-run 2>&1)
check "Dry-run mentions 2 messages" "$( echo "$DRY_OUTPUT" | grep -q '2 message' && echo pass || echo fail )"
# Verify file untouched
POST_DRY_COUNT=$("$NBS_CHAT" read "$CHAT" | wc -l)
check "Dry-run did not modify file" "$( [[ "$POST_DRY_COUNT" -eq 5 ]] && echo pass || echo fail )"

next_test "Delete: actual delete removes correct messages"
"$NBS_CHAT" delete "$CHAT" --after="$DELETE_EPOCH"
REMAINING=$("$NBS_CHAT" read "$CHAT")
REMAINING_COUNT=$(echo "$REMAINING" | wc -l)
check "3 messages remain after delete" "$( [[ "$REMAINING_COUNT" -eq 3 ]] && echo pass || echo fail )"
check "msg-1 survives" "$( echo "$REMAINING" | grep -qF 'delete-msg-1' && echo pass || echo fail )"
check "msg-2 survives" "$( echo "$REMAINING" | grep -qF 'delete-msg-2' && echo pass || echo fail )"
check "msg-3 survives" "$( echo "$REMAINING" | grep -qF 'delete-msg-3' && echo pass || echo fail )"
check "msg-4 deleted" "$( ! echo "$REMAINING" | grep -qF 'delete-msg-4' && echo pass || echo fail )"
check "msg-5 deleted" "$( ! echo "$REMAINING" | grep -qF 'delete-msg-5' && echo pass || echo fail )"

next_test "Delete: file-length header matches actual file size after delete"
HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$CHAT")
check "file-length == wc -c after delete" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"

next_test "Delete: no matching messages is a no-op"
FUTURE_EPOCH=$(($(date +%s) + 99999))
set +e
NOOP_OUTPUT=$("$NBS_CHAT" delete "$CHAT" --after="$FUTURE_EPOCH" 2>&1)
NOOP_RC=$?
set -e
check "No-op delete exits 0" "$( [[ "$NOOP_RC" -eq 0 ]] && echo pass || echo fail )"
check "No-op output mentions 0" "$( echo "$NOOP_OUTPUT" | grep -q '0 message' && echo pass || echo fail )"
# Verify file unchanged
POST_NOOP_COUNT=$("$NBS_CHAT" read "$CHAT" | wc -l)
check "File unchanged after no-op delete" "$( [[ "$POST_NOOP_COUNT" -eq 3 ]] && echo pass || echo fail )"

next_test "Delete: without --after exits with code 4"
set +e
"$NBS_CHAT" delete "$CHAT" >/dev/null 2>&1
DELETE_NO_AFTER_RC=$?
set -e
check "Delete without --after exits 4" "$( [[ "$DELETE_NO_AFTER_RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# Gap 2: Time-based filtering (--after / --before)
# =====================================================================

next_test "Time filtering: create chat with timed messages"
CHAT="$TEST_DIR/gap2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" alice "time-old"
sleep 3
TIME_EPOCH=$(date +%s)
sleep 1
"$NBS_CHAT" send "$CHAT" bob "time-new"

next_test "Time filtering: --after=EPOCH shows only newer messages"
AFTER_OUT=$("$NBS_CHAT" read "$CHAT" --after="$TIME_EPOCH")
AFTER_COUNT=$(echo "$AFTER_OUT" | grep -c '.' || true)
check "--after=EPOCH returns 1 message" "$( [[ "$AFTER_COUNT" -eq 1 ]] && echo pass || echo fail )"
check "--after=EPOCH returns the newer message" "$( echo "$AFTER_OUT" | grep -qF 'time-new' && echo pass || echo fail )"

next_test "Time filtering: --before=EPOCH shows only older messages"
BEFORE_OUT=$("$NBS_CHAT" read "$CHAT" --before="$TIME_EPOCH")
BEFORE_COUNT=$(echo "$BEFORE_OUT" | grep -c '.' || true)
check "--before=EPOCH returns 1 message" "$( [[ "$BEFORE_COUNT" -eq 1 ]] && echo pass || echo fail )"
check "--before=EPOCH returns the older message" "$( echo "$BEFORE_OUT" | grep -qF 'time-old' && echo pass || echo fail )"

next_test "Time filtering: --after with relative time (2s)"
# time-new was sent ~1s ago, time-old ~4s ago
# --after=2s means "after 2 seconds ago" so should show time-new
REL_OUT=$("$NBS_CHAT" read "$CHAT" --after=2s)
REL_COUNT=$(echo "$REL_OUT" | grep -c '.' || true)
check "--after=2s returns 1 message" "$( [[ "$REL_COUNT" -eq 1 ]] && echo pass || echo fail )"
check "--after=2s returns newer message" "$( echo "$REL_OUT" | grep -qF 'time-new' && echo pass || echo fail )"

next_test "Time filtering: search with --after"
SEARCH_AFTER=$("$NBS_CHAT" search "$CHAT" "time" --after="$TIME_EPOCH" 2>&1)
SEARCH_AFTER_COUNT=$(echo "$SEARCH_AFTER" | grep -c '^\[' || true)
check "Search --after filters to 1 match" "$( [[ "$SEARCH_AFTER_COUNT" -eq 1 ]] && echo pass || echo fail )"
check "Search --after returns newer match" "$( echo "$SEARCH_AFTER" | grep -qF 'time-new' && echo pass || echo fail )"

next_test "Time filtering: invalid time format exits with code 4"
set +e
"$NBS_CHAT" read "$CHAT" --after=not_a_time >/dev/null 2>&1
INVALID_TIME_RC=$?
set -e
check "Invalid --after value exits 4" "$( [[ "$INVALID_TIME_RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# Gap 3: Cursor-on-write
# =====================================================================

next_test "Cursor-on-write: send advances sender's own cursor"
CHAT="$TEST_DIR/gap3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Alice sends a message
"$NBS_CHAT" send "$CHAT" alice "hello from alice"

# Alice's cursor should have been advanced by her send
ALICE_UNREAD=$("$NBS_CHAT" read "$CHAT" --unread=alice)
check "Alice unread is empty after her own send" "$( [[ -z "$ALICE_UNREAD" ]] && echo pass || echo fail )"

next_test "Cursor-on-write: bob sends, alice sees bob's message only"
"$NBS_CHAT" send "$CHAT" bob "hello from bob"
ALICE_UNREAD2=$("$NBS_CHAT" read "$CHAT" --unread=alice)
ALICE_COUNT2=$(echo "$ALICE_UNREAD2" | grep -c '.' || true)
check "Alice sees 1 unread (bob's message)" "$( [[ "$ALICE_COUNT2" -eq 1 ]] && echo pass || echo fail )"
check "Alice's unread is bob's message" "$( echo "$ALICE_UNREAD2" | grep -qF 'bob: hello from bob' && echo pass || echo fail )"

next_test "Cursor-on-write: bob's cursor is empty after his send"
BOB_UNREAD=$("$NBS_CHAT" read "$CHAT" --unread=bob)
check "Bob unread is empty after his own send" "$( [[ -z "$BOB_UNREAD" ]] && echo pass || echo fail )"

# =====================================================================
# Gap 4: Message timestamp wire format
# =====================================================================

next_test "Wire format: decode raw base64 line from chat file"
CHAT="$TEST_DIR/gap4.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
BEFORE_SEND=$(date +%s)
"$NBS_CHAT" send "$CHAT" wiretest "wire format test message"
AFTER_SEND=$(date +%s)

# Extract the raw base64 line (first line after the --- delimiter)
RAW_B64=$(sed -n '/^---$/,$ p' "$CHAT" | tail -n +2 | head -1)
# Decode it
DECODED=$(echo "$RAW_B64" | base64 -d 2>/dev/null)
check "Base64 decodes successfully" "$( [[ -n "$DECODED" ]] && echo pass || echo fail )"

# Verify format: handle|EPOCH: content
check "Decoded matches handle|EPOCH: content" \
    "$( echo "$DECODED" | grep -qE '^wiretest\|[0-9]+: wire format test message$' && echo pass || echo fail )"

# Extract the epoch and verify it is within 5 seconds of BEFORE_SEND
MSG_EPOCH=$(echo "$DECODED" | sed 's/^[^|]*|\([0-9]*\):.*/\1/')
DELTA_LOW=$((MSG_EPOCH - BEFORE_SEND))
DELTA_HIGH=$((AFTER_SEND - MSG_EPOCH))
check "Message epoch >= send start" "$( [[ "$DELTA_LOW" -ge 0 ]] && echo pass || echo fail )"
check "Message epoch <= send end (within 5s)" "$( [[ "$DELTA_HIGH" -ge -5 ]] && echo pass || echo fail )"

next_test "Wire format: read output includes ISO timestamp prefix"
READ_OUTPUT=$("$NBS_CHAT" read "$CHAT")
# Format: [YYYY-MM-DDTHH:MM:SSZ] handle: content
check "Read output has ISO timestamp prefix" \
    "$( echo "$READ_OUTPUT" | grep -qE '^\[20[0-9]{2}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] wiretest: wire format test message$' && echo pass || echo fail )"

# =====================================================================
# Gap 5: Web POST /api/send
# =====================================================================

if [[ -x "$NBS_WEB" ]]; then

    # Bypass corporate proxy for localhost connections
    export no_proxy="localhost,127.0.0.1,::1,[::1]"
    export NO_PROXY="$no_proxy"

    next_test "Web POST: start server and POST valid message"
    CHAT="$TEST_DIR/gap5.chat"
    "$NBS_CHAT" create "$CHAT" >/dev/null

    # Start server on auto-assigned port
    "$NBS_WEB" "$CHAT" --port=0 > "$TEST_DIR/web.out" 2>&1 &
    WEB_PID=$!
    sleep 1

    # Extract port from server output
    PORT=$(grep -oP ':\K[0-9]+(?=/)' "$TEST_DIR/web.out")
    if [[ -z "$PORT" ]]; then
        echo "   FAIL: Could not extract port from web server output"
        FAIL=$((FAIL + 1))
    else
        # Determine base URL from server output
        BASE_URL=$(grep -oP 'http://\K[^\s]+(?=/)' "$TEST_DIR/web.out")
        BASE_URL="http://${BASE_URL}"
        echo "   Web server started on port $PORT (PID $WEB_PID)"

        # POST valid JSON
        HTTP_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
            "$BASE_URL/api/send" \
            -H "Content-Type: application/json" \
            -d '{"handle":"webuser","message":"hello from web"}')
        check "POST valid JSON returns 200" "$( [[ "$HTTP_CODE" == "200" ]] && echo pass || echo fail )"

        # Verify message appears in chat
        WEB_READ=$("$NBS_CHAT" read "$CHAT")
        check "Message appears in chat file" \
            "$( echo "$WEB_READ" | grep -qF 'webuser: hello from web' && echo pass || echo fail )"

        next_test "Web POST: empty handle returns 400"
        HTTP_EMPTY_HANDLE=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
            "$BASE_URL/api/send" \
            -H "Content-Type: application/json" \
            -d '{"handle":"","message":"should fail"}')
        check "POST empty handle returns 400" "$( [[ "$HTTP_EMPTY_HANDLE" == "400" ]] && echo pass || echo fail )"

        next_test "Web POST: empty message returns 400"
        HTTP_EMPTY_MSG=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
            "$BASE_URL/api/send" \
            -H "Content-Type: application/json" \
            -d '{"handle":"user","message":""}')
        check "POST empty message returns 400" "$( [[ "$HTTP_EMPTY_MSG" == "400" ]] && echo pass || echo fail )"

        # Only valid message should be in chat (the 2 invalid POSTs should not have added messages)
        FINAL_COUNT=$("$NBS_CHAT" read "$CHAT" | wc -l)
        check "Invalid POSTs did not add messages" "$( [[ "$FINAL_COUNT" -eq 1 ]] && echo pass || echo fail )"
    fi

    killbg "$WEB_PID"
    WEB_PID=0

else
    echo ""
    echo "SKIP: nbs-chat-web not found at $NBS_WEB — Gap 5 tests skipped"
fi

# =====================================================================
# Summary
# =====================================================================

echo ""
echo "=== Result: $PASS passed, $FAIL failed ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $FAIL test(s) failed"
    exit 1
fi
