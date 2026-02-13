#!/bin/bash
# Test: nbs-chat lifecycle with falsification tests
#
# Deterministic tests covering:
#   1.  Create and verify header format
#   2.  Send and read round-trip
#   3.  Multiple senders, correct decode
#   4.  --last=N filter
#   5.  --since=<handle> filter
#   6.  Poll timeout (exit code 3)
#   7.  Poll success (background send)
#   8.  Participants and counts
#   9.  Header integrity (file-length matches wc -c)
#   10. Lock contention (concurrent sends)
#   11. Base64 round-trip with special characters
#   12. Empty channel read (clean exit 0)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"

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

echo "=== nbs-chat Lifecycle Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Create and verify header ---
echo "1. Create and verify header format..."
CHAT="$TEST_DIR/test1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Verify header structure
FIRST_LINE=$(head -1 "$CHAT")
check "Header marker" "$( [[ "$FIRST_LINE" == "=== nbs-chat ===" ]] && echo pass || echo fail )"

HAS_LAST_WRITER=$(grep -c '^last-writer:' "$CHAT" || true)
check "Has last-writer" "$( [[ "$HAS_LAST_WRITER" -eq 1 ]] && echo pass || echo fail )"

HAS_LAST_WRITE=$(grep -c '^last-write:' "$CHAT" || true)
check "Has last-write" "$( [[ "$HAS_LAST_WRITE" -eq 1 ]] && echo pass || echo fail )"

HAS_FILE_LENGTH=$(grep -c '^file-length:' "$CHAT" || true)
check "Has file-length" "$( [[ "$HAS_FILE_LENGTH" -eq 1 ]] && echo pass || echo fail )"

HAS_PARTICIPANTS=$(grep -c '^participants:' "$CHAT" || true)
check "Has participants" "$( [[ "$HAS_PARTICIPANTS" -eq 1 ]] && echo pass || echo fail )"

HAS_DELIMITER=$(grep -c '^---$' "$CHAT" || true)
check "Has delimiter" "$( [[ "$HAS_DELIMITER" -eq 1 ]] && echo pass || echo fail )"

echo ""

# --- Test 2: Send and read round-trip ---
echo "2. Send and read round-trip..."
CHAT="$TEST_DIR/test2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "alice" "Hello from Alice"
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "Message decoded" "$( echo "$OUTPUT" | grep -qF 'alice: Hello from Alice' && echo pass || echo fail )"

echo ""

# --- Test 3: Multiple senders ---
echo "3. Multiple senders, correct decode..."
CHAT="$TEST_DIR/test3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "parser-worker" "Found 3 failing tests"
"$NBS_CHAT" send "$CHAT" "test-runner" "Confirmed - test_parse_int fails"
"$NBS_CHAT" send "$CHAT" "supervisor" "Both of you focus on parse_int first"
OUTPUT=$("$NBS_CHAT" read "$CHAT")
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "3 messages returned" "$( [[ "$LINE_COUNT" -eq 3 ]] && echo pass || echo fail )"
check "First message correct" "$( echo "$OUTPUT" | head -1 | grep -qF 'parser-worker: Found 3 failing tests' && echo pass || echo fail )"
check "Last message correct" "$( echo "$OUTPUT" | tail -1 | grep -qF 'supervisor: Both of you focus on parse_int first' && echo pass || echo fail )"

echo ""

# --- Test 4: --last=N filter ---
echo "4. --last=N filter..."
CHAT="$TEST_DIR/test4.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
for i in $(seq 1 10); do
    "$NBS_CHAT" send "$CHAT" "worker" "Message $i"
done
OUTPUT=$("$NBS_CHAT" read "$CHAT" --last=3)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "Only 3 messages" "$( [[ "$LINE_COUNT" -eq 3 ]] && echo pass || echo fail )"
check "First is msg 8" "$( echo "$OUTPUT" | head -1 | grep -qF 'worker: Message 8' && echo pass || echo fail )"
check "Last is msg 10" "$( echo "$OUTPUT" | tail -1 | grep -qF 'worker: Message 10' && echo pass || echo fail )"

echo ""

# --- Test 5: --since=<handle> filter ---
echo "5. --since=<handle> filter..."
CHAT="$TEST_DIR/test5.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "alice" "First from alice"
"$NBS_CHAT" send "$CHAT" "bob" "First from bob"
"$NBS_CHAT" send "$CHAT" "alice" "Second from alice"
"$NBS_CHAT" send "$CHAT" "charlie" "First from charlie"
"$NBS_CHAT" send "$CHAT" "bob" "Second from bob"
OUTPUT=$("$NBS_CHAT" read "$CHAT" --since=alice)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "2 messages after alice" "$( [[ "$LINE_COUNT" -eq 2 ]] && echo pass || echo fail )"
check "Contains charlie" "$( echo "$OUTPUT" | grep -qF 'charlie: First from charlie' && echo pass || echo fail )"
check "Contains bob second" "$( echo "$OUTPUT" | grep -qF 'bob: Second from bob' && echo pass || echo fail )"

echo ""

# --- Test 6: Poll timeout ---
echo "6. Poll timeout (exit code 3)..."
CHAT="$TEST_DIR/test6.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
set +e
"$NBS_CHAT" poll "$CHAT" "watcher" --timeout=2 >/dev/null 2>&1
POLL_RC=$?
set -e
check "Exit code 3 on timeout" "$( [[ "$POLL_RC" -eq 3 ]] && echo pass || echo fail )"

echo ""

# --- Test 7: Poll success ---
echo "7. Poll success (background send)..."
CHAT="$TEST_DIR/test7.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Send a message in the background after 2 seconds
(
    sleep 2
    "$NBS_CHAT" send "$CHAT" "sender" "Background message"
) &
BG_PID=$!

set +e
POLL_OUTPUT=$("$NBS_CHAT" poll "$CHAT" "watcher" --timeout=10 2>/dev/null)
POLL_RC=$?
set -e
wait "$BG_PID" 2>/dev/null || true

check "Poll exit code 0" "$( [[ "$POLL_RC" -eq 0 ]] && echo pass || echo fail )"
check "Poll got message" "$( echo "$POLL_OUTPUT" | grep -qF 'sender: Background message' && echo pass || echo fail )"

echo ""

# --- Test 8: Participants and counts ---
echo "8. Participants and counts..."
CHAT="$TEST_DIR/test8.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "alice" "msg1"
"$NBS_CHAT" send "$CHAT" "alice" "msg2"
"$NBS_CHAT" send "$CHAT" "alice" "msg3"
"$NBS_CHAT" send "$CHAT" "bob" "msg1"
"$NBS_CHAT" send "$CHAT" "bob" "msg2"
"$NBS_CHAT" send "$CHAT" "charlie" "msg1"
OUTPUT=$("$NBS_CHAT" participants "$CHAT")
check "Alice has 3" "$( echo "$OUTPUT" | grep 'alice' | grep -q '3 messages' && echo pass || echo fail )"
check "Bob has 2" "$( echo "$OUTPUT" | grep 'bob' | grep -q '2 messages' && echo pass || echo fail )"
check "Charlie has 1" "$( echo "$OUTPUT" | grep 'charlie' | grep -q '1 messages' && echo pass || echo fail )"

echo ""

# --- Test 9: Header integrity (file-length matches actual) ---
echo "9. Header integrity..."
CHAT="$TEST_DIR/test9.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "worker-a" "Some test message"
"$NBS_CHAT" send "$CHAT" "worker-b" "Another test message"

HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$CHAT")
check "file-length matches wc -c" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"

# Header fields updated
LAST_WRITER=$(grep '^last-writer:' "$CHAT" | sed 's/^last-writer:[[:space:]]*//')
check "last-writer is worker-b" "$( [[ "$LAST_WRITER" == "worker-b" ]] && echo pass || echo fail )"

PARTICIPANTS=$(grep '^participants:' "$CHAT" | sed 's/^participants:[[:space:]]*//')
check "Participants has worker-a" "$( echo "$PARTICIPANTS" | grep -qF 'worker-a' && echo pass || echo fail )"
check "Participants has worker-b" "$( echo "$PARTICIPANTS" | grep -qF 'worker-b' && echo pass || echo fail )"

echo ""

# --- Test 10: Lock contention (concurrent sends) ---
echo "10. Lock contention (50 concurrent sends)..."
CHAT="$TEST_DIR/test10.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

PIDS=()
for i in $(seq 1 50); do
    "$NBS_CHAT" send "$CHAT" "worker-$((i % 5))" "Concurrent message $i" &
    PIDS+=($!)
done

# Wait for all background sends
SEND_FAILURES=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid" 2>/dev/null; then
        SEND_FAILURES=$((SEND_FAILURES + 1))
    fi
done

check "No send failures" "$( [[ "$SEND_FAILURES" -eq 0 ]] && echo pass || echo fail )"

# Verify message count
OUTPUT=$("$NBS_CHAT" read "$CHAT")
MSG_COUNT=$(echo "$OUTPUT" | grep -c '.' || true)
check "50 messages present" "$( [[ "$MSG_COUNT" -eq 50 ]] && echo pass || echo fail )"

# Verify no corruption — every line must decode cleanly
RAW_MESSAGES=$(sed -n '/^---$/,$ p' "$CHAT" | tail -n +2)
CORRUPT=0
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    if ! echo "$line" | base64 -d >/dev/null 2>&1; then
        CORRUPT=$((CORRUPT + 1))
    fi
done <<< "$RAW_MESSAGES"
check "No corrupt messages" "$( [[ "$CORRUPT" -eq 0 ]] && echo pass || echo fail )"

# Verify header integrity after concurrent writes
HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$CHAT")
check "Header length correct after contention" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"

echo ""

# --- Test 11: Base64 round-trip with special characters ---
echo "11. Base64 round-trip with special characters..."
CHAT="$TEST_DIR/test11.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Message with colons, quotes, backslashes, equals
"$NBS_CHAT" send "$CHAT" "tester" 'Key: value="foo\\bar" (test=1)'
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "Special chars preserved" "$( echo "$OUTPUT" | grep -qF 'tester: Key: value="foo\\bar" (test=1)' && echo pass || echo fail )"

# Message with unicode
"$NBS_CHAT" send "$CHAT" "tester" "Héllo wörld — dashes and «quotes»"
OUTPUT=$("$NBS_CHAT" read "$CHAT" --last=1)
check "Unicode preserved" "$( echo "$OUTPUT" | grep -qF "Héllo wörld — dashes" && echo pass || echo fail )"

echo ""

# --- Test 12: Empty channel read ---
echo "12. Empty channel read (clean exit 0)..."
CHAT="$TEST_DIR/test12.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
set +e
OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>&1)
READ_RC=$?
set -e
check "Exit code 0" "$( [[ "$READ_RC" -eq 0 ]] && echo pass || echo fail )"
check "Empty output" "$( [[ -z "$OUTPUT" ]] && echo pass || echo fail )"

echo ""

# --- Test 13: Error handling ---
echo "13. Error handling..."
set +e

# Read non-existent file
"$NBS_CHAT" read "$TEST_DIR/nonexistent.chat" >/dev/null 2>&1
check "Exit 2 for missing file" "$( [[ $? -eq 2 ]] && echo pass || echo fail )"

# Create already-existing file
"$NBS_CHAT" create "$TEST_DIR/test12.chat" >/dev/null 2>&1
check "Exit 1 for existing file" "$( [[ $? -eq 1 ]] && echo pass || echo fail )"

# Send with missing args
"$NBS_CHAT" send "$TEST_DIR/test12.chat" >/dev/null 2>&1
check "Exit 4 for missing args" "$( [[ $? -eq 4 ]] && echo pass || echo fail )"

# Unknown command
"$NBS_CHAT" foobar >/dev/null 2>&1
check "Exit 4 for unknown command" "$( [[ $? -eq 4 ]] && echo pass || echo fail )"

set -e

echo ""

# --- Test 14: Poll ignores self-messages ---
echo "14. Poll ignores self-messages..."
CHAT="$TEST_DIR/test14.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Send a self-message in background, then a message from other handle
(
    sleep 1
    "$NBS_CHAT" send "$CHAT" "watcher" "My own message"
    sleep 1
    "$NBS_CHAT" send "$CHAT" "other" "External message"
) &
BG_PID=$!

set +e
POLL_OUTPUT=$("$NBS_CHAT" poll "$CHAT" "watcher" --timeout=10 2>/dev/null)
POLL_RC=$?
set -e
wait "$BG_PID" 2>/dev/null || true

check "Poll ignores self, gets other" "$( [[ "$POLL_RC" -eq 0 ]] && echo "$POLL_OUTPUT" | grep -qF 'other: External message' && echo pass || echo fail )"
check "Self-message not in output" "$( ! echo "$POLL_OUTPUT" | grep -qF 'watcher: My own message' && echo pass || echo fail )"

echo ""

# --- Test 15: Assert strings present in binaries ---
echo "15. Assert verification..."
CLI_ASSERT_STRINGS=$(strings "$NBS_CHAT" | grep -c "ASSERT FAILED" || true)
check "CLI binary contains ASSERT_MSG strings" "$( [[ "$CLI_ASSERT_STRINGS" -gt 0 ]] && echo pass || echo fail )"

TERM_ASSERT_STRINGS=$(strings "$NBS_TERMINAL" | grep -c "ASSERT FAILED" || true)
check "Terminal binary contains ASSERT_MSG strings" "$( [[ "$TERM_ASSERT_STRINGS" -gt 0 ]] && echo pass || echo fail )"

# --- Test 16: Search command ---
echo "16. Search command..."

# Set up a dedicated chat for search tests
SEARCH_CHAT="$TEST_DIR/search.chat"
"$NBS_CHAT" create "$SEARCH_CHAT" >/dev/null
"$NBS_CHAT" send "$SEARCH_CHAT" alice "The parser fails on negative inputs like -42" >/dev/null
"$NBS_CHAT" send "$SEARCH_CHAT" bob "I can confirm the parser bug with -100 too" >/dev/null
"$NBS_CHAT" send "$SEARCH_CHAT" alice "Fix deployed, all tests passing now" >/dev/null
"$NBS_CHAT" send "$SEARCH_CHAT" carol "Unrelated message about the deployment pipeline" >/dev/null

# Basic search — should find messages containing "parser"
SEARCH_RESULT=$("$NBS_CHAT" search "$SEARCH_CHAT" "parser" 2>&1)
PARSER_MATCHES=$(echo "$SEARCH_RESULT" | grep -c '^\[' || true)
check "Search finds parser matches" "$( [[ "$PARSER_MATCHES" -eq 2 ]] && echo pass || echo fail )"

# Case-insensitive search
SEARCH_UPPER=$("$NBS_CHAT" search "$SEARCH_CHAT" "PARSER" 2>&1)
UPPER_MATCHES=$(echo "$SEARCH_UPPER" | grep -c '^\[' || true)
check "Search is case-insensitive" "$( [[ "$UPPER_MATCHES" -eq 2 ]] && echo pass || echo fail )"

# Search with handle filter
SEARCH_ALICE=$("$NBS_CHAT" search "$SEARCH_CHAT" "parser" --handle=alice 2>&1)
ALICE_MATCHES=$(echo "$SEARCH_ALICE" | grep -c '^\[' || true)
check "Search with --handle filter" "$( [[ "$ALICE_MATCHES" -eq 1 ]] && echo pass || echo fail )"

# Search with no matches
SEARCH_NONE=$("$NBS_CHAT" search "$SEARCH_CHAT" "xyzzy_no_match" 2>&1)
check "Search with no matches says so" "$( echo "$SEARCH_NONE" | grep -q "No matches" && echo pass || echo fail )"

# Search output includes message index
check "Search shows message index" "$( echo "$SEARCH_RESULT" | grep -q '^\[0\]' && echo pass || echo fail )"

# Search exit code is 0 even with no matches
"$NBS_CHAT" search "$SEARCH_CHAT" "xyzzy_no_match" >/dev/null 2>&1
check "Search exit 0 on no matches" "$( [[ $? -eq 0 ]] && echo pass || echo fail )"

# Search on missing file
SEARCH_MISSING_RC=0
"$NBS_CHAT" search "$TEST_DIR/nonexistent.chat" "test" >/dev/null 2>&1 || SEARCH_MISSING_RC=$?
check "Search exit 2 on missing file" "$( [[ "$SEARCH_MISSING_RC" -eq 2 ]] && echo pass || echo fail )"

# Search with too few args
SEARCH_NOARGS_RC=0
"$NBS_CHAT" search "$SEARCH_CHAT" >/dev/null 2>&1 || SEARCH_NOARGS_RC=$?
check "Search exit 4 on missing pattern" "$( [[ "$SEARCH_NOARGS_RC" -eq 4 ]] && echo pass || echo fail )"

echo ""


# --- Test 17: --unread cursor tracking ---
echo "17. --unread cursor tracking..."
UNREAD_CHAT="$TEST_DIR/unread.chat"
"$NBS_CHAT" create "$UNREAD_CHAT" >/dev/null
"$NBS_CHAT" send "$UNREAD_CHAT" "alice" "Message one" >/dev/null
"$NBS_CHAT" send "$UNREAD_CHAT" "bob" "Message two" >/dev/null
"$NBS_CHAT" send "$UNREAD_CHAT" "alice" "Message three" >/dev/null

# First --unread read: should see all messages (no cursor exists yet)
UNREAD1=$("$NBS_CHAT" read "$UNREAD_CHAT" --unread=reader)
check "--unread first read shows all 3 messages" "$( echo "$UNREAD1" | wc -l | awk '{print ($1 == 3) ? "pass" : "fail"}' )"

# Second --unread read: should see nothing (cursor was advanced)
UNREAD2=$("$NBS_CHAT" read "$UNREAD_CHAT" --unread=reader)
check "--unread second read shows nothing" "$( [[ -z "$UNREAD2" ]] && echo pass || echo fail )"

# Send a new message, then --unread should show only the new one
"$NBS_CHAT" send "$UNREAD_CHAT" "bob" "Message four" >/dev/null
UNREAD3=$("$NBS_CHAT" read "$UNREAD_CHAT" --unread=reader)
check "--unread after new message shows only new" "$( echo "$UNREAD3" | grep -qF "Message four" && echo pass || echo fail )"
check "--unread shows exactly 1 new message" "$( echo "$UNREAD3" | wc -l | awk '{print ($1 == 1) ? "pass" : "fail"}' )"

# Different handles have independent cursors
UNREAD4=$("$NBS_CHAT" read "$UNREAD_CHAT" --unread=reader2)
check "--unread different handle sees all messages" "$( echo "$UNREAD4" | wc -l | awk '{print ($1 == 4) ? "pass" : "fail"}' )"

# Cursor file exists
check "Cursor file created" "$( [[ -f "${UNREAD_CHAT}.cursors" ]] && echo pass || echo fail )"

echo ""

# --- Test 18: --unread combined with --last ---
echo "18. --unread combined with --last..."
COMBO_CHAT="$TEST_DIR/combo.chat"
"$NBS_CHAT" create "$COMBO_CHAT" >/dev/null
for i in $(seq 1 10); do
    "$NBS_CHAT" send "$COMBO_CHAT" "sender" "Msg $i" >/dev/null
done

# --unread sees all 10, but --last=3 limits to last 3
COMBO1=$("$NBS_CHAT" read "$COMBO_CHAT" --unread=reader --last=3)
check "--unread with --last=3 shows 3 messages" "$( echo "$COMBO1" | wc -l | awk '{print ($1 == 3) ? "pass" : "fail"}' )"
check "--unread with --last shows most recent" "$( echo "$COMBO1" | grep -qF "Msg 10" && echo pass || echo fail )"

echo ""

# --- Test 19: --unread cursor survives multiple senders ---
echo "19. --unread cursor survives concurrent activity..."
MULTI_CHAT="$TEST_DIR/multi_cursor.chat"
"$NBS_CHAT" create "$MULTI_CHAT" >/dev/null
"$NBS_CHAT" send "$MULTI_CHAT" "a" "first" >/dev/null
"$NBS_CHAT" send "$MULTI_CHAT" "b" "second" >/dev/null

# reader1 reads (sees 2, cursor advances to index 1)
"$NBS_CHAT" read "$MULTI_CHAT" --unread=reader1 >/dev/null

# More messages arrive
"$NBS_CHAT" send "$MULTI_CHAT" "a" "third" >/dev/null
"$NBS_CHAT" send "$MULTI_CHAT" "b" "fourth" >/dev/null

# reader1 should see only third and fourth
MULTI1=$("$NBS_CHAT" read "$MULTI_CHAT" --unread=reader1)
check "--unread tracks correctly across senders" "$( echo "$MULTI1" | wc -l | awk '{print ($1 == 2) ? "pass" : "fail"}' )"
check "--unread first new message is correct" "$( echo "$MULTI1" | head -1 | grep -qF "third" && echo pass || echo fail )"
check "--unread second new message is correct" "$( echo "$MULTI1" | tail -1 | grep -qF "fourth" && echo pass || echo fail )"

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
