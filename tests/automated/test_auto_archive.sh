#!/bin/bash
# Test: Auto-archive feature for nbs-chat
#
# When a chat file exceeds 2000 messages after a send, nbs-chat should
# automatically cleave the first 1000 messages into an archive file and
# rewrite the main file with the remaining messages.
#
# Falsification: if auto-archive is disabled or broken, a chat file with
# 2001 messages will NOT produce an archive file, and the main file will
# retain all 2001 messages.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"

TEST_DIR=$(mktemp -d)
ERRORS=0
TESTS=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    TESTS=$((TESTS + 1))
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

# Bulk-populate a chat file with N messages using direct base64 encoding.
# Much faster than calling nbs-chat send N times (seconds vs minutes).
bulk_populate() {
    local chat_file="$1"
    local count="$2"
    python3 -c "
import base64, time
path = '$chat_file'
with open(path, 'r') as f:
    header = []
    for line in f:
        header.append(line)
        if line.strip() == '---': break
msgs = []
epoch = int(time.time())
for i in range(1, $count + 1):
    handle = f'agent-{i % 5}'
    raw = f'{handle}|{epoch}: message number {i}'
    msgs.append(base64.b64encode(raw.encode()).decode())
with open(path, 'w') as f:
    for h in header: f.write(h)
    for m in msgs: f.write(m + '\n')
"
}

echo "Test: nbs-chat auto-archive"
echo "==========================="
echo ""

# --- Setup: create a chat file and populate it with 2000 messages ---
CHAT_FILE="$TEST_DIR/live.chat"
"$NBS_CHAT" create "$CHAT_FILE"

echo "Populating chat with 2000 messages..."
bulk_populate "$CHAT_FILE" 2000

# Set up cursors for several agents
echo "# Read cursors — last-read message index per handle" > "${CHAT_FILE}.cursors"
echo "agent-0=500" >> "${CHAT_FILE}.cursors"
echo "agent-1=1500" >> "${CHAT_FILE}.cursors"
echo "agent-2=50" >> "${CHAT_FILE}.cursors"
echo "agent-3=999" >> "${CHAT_FILE}.cursors"

# 1. Verify we have 2000 messages before archive trigger
MSG_COUNT=$("$NBS_CHAT" read "$CHAT_FILE" | wc -l)
R=$([[ "$MSG_COUNT" -eq 2000 ]] && echo pass || echo fail)
check "1. Pre-archive: 2000 messages present (got $MSG_COUNT)" "$R"

# 2. No archive file should exist yet
ARCHIVE_COUNT=$(find "$TEST_DIR" -name '*-archive.chat' 2>/dev/null | wc -l)
R=$([[ "$ARCHIVE_COUNT" -eq 0 ]] && echo pass || echo fail)
check "2. Pre-archive: no archive file exists" "$R"

# 3. Send message 2001 — this should trigger auto-archive
"$NBS_CHAT" send "$CHAT_FILE" "trigger" "message 2001 triggers archive" 2>/dev/null

# 4. Archive file should now exist
ARCHIVE_FILES=$(ls "$TEST_DIR"/*-archive.chat 2>/dev/null || true)
ARCHIVE_COUNT=$(echo "$ARCHIVE_FILES" | grep -c '.' || true)
R=$([[ "$ARCHIVE_COUNT" -eq 1 ]] && echo pass || echo fail)
check "3. Archive file created after message 2001" "$R"

if [[ "$ARCHIVE_COUNT" -ge 1 ]]; then
    ARCHIVE_FILE=$(echo "$ARCHIVE_FILES" | head -1)

    # 5. Archive file should contain 1000 messages
    ARCHIVE_MSG_COUNT=$("$NBS_CHAT" read "$ARCHIVE_FILE" | wc -l)
    R=$([[ "$ARCHIVE_MSG_COUNT" -eq 1000 ]] && echo pass || echo fail)
    check "4. Archive contains 1000 messages (got $ARCHIVE_MSG_COUNT)" "$R"

    # 6. Archive file should start with message 1
    FIRST_ARCHIVE=$("$NBS_CHAT" read "$ARCHIVE_FILE" --last=1000 | head -1)
    R=$(echo "$FIRST_ARCHIVE" | grep -qF "message number 1" && echo pass || echo fail)
    check "5. Archive starts with message 1" "$R"

    # 7. Archive file should end with message 1000
    LAST_ARCHIVE=$("$NBS_CHAT" read "$ARCHIVE_FILE" --last=1 | head -1)
    R=$(echo "$LAST_ARCHIVE" | grep -qF "message number 1000" && echo pass || echo fail)
    check "6. Archive ends with message 1000" "$R"

    # 8. Archive filename matches pattern
    ARCHIVE_BASENAME=$(basename "$ARCHIVE_FILE")
    R=$(echo "$ARCHIVE_BASENAME" | grep -qE '^live-[0-9]{8}-[0-9]{6}-archive\.chat$' && echo pass || echo fail)
    check "7. Archive filename matches pattern (got $ARCHIVE_BASENAME)" "$R"
fi

# 9. Main file should contain 1001 messages (2001 - 1000)
MAIN_MSG_COUNT=$("$NBS_CHAT" read "$CHAT_FILE" | wc -l)
R=$([[ "$MAIN_MSG_COUNT" -eq 1001 ]] && echo pass || echo fail)
check "8. Main file has 1001 messages after archive (got $MAIN_MSG_COUNT)" "$R"

# 10. Main file should start with message 1001
FIRST_MAIN=$("$NBS_CHAT" read "$CHAT_FILE" --last=1001 | head -1)
R=$(echo "$FIRST_MAIN" | grep -qF "message number 1001" && echo pass || echo fail)
check "9. Main file starts with message 1001" "$R"

# 11. Main file should end with message 2001
LAST_MAIN=$("$NBS_CHAT" read "$CHAT_FILE" --last=1 | head -1)
R=$(echo "$LAST_MAIN" | grep -qF "message 2001 triggers archive" && echo pass || echo fail)
check "10. Main file ends with message 2001 (trigger message)" "$R"

# 12. Cursor adjustments
CURSOR_0=$(grep "^agent-0=" "${CHAT_FILE}.cursors" | cut -d= -f2)
CURSOR_1=$(grep "^agent-1=" "${CHAT_FILE}.cursors" | cut -d= -f2)
CURSOR_2=$(grep "^agent-2=" "${CHAT_FILE}.cursors" | cut -d= -f2)
CURSOR_3=$(grep "^agent-3=" "${CHAT_FILE}.cursors" | cut -d= -f2)

R=$([[ "$CURSOR_0" -eq 0 ]] && echo pass || echo fail)
check "11. Cursor agent-0: 500 -> 0 (clamped) (got $CURSOR_0)" "$R"

R=$([[ "$CURSOR_1" -eq 500 ]] && echo pass || echo fail)
check "12. Cursor agent-1: 1500 -> 500 (got $CURSOR_1)" "$R"

R=$([[ "$CURSOR_2" -eq 0 ]] && echo pass || echo fail)
check "13. Cursor agent-2: 50 -> 0 (clamped) (got $CURSOR_2)" "$R"

R=$([[ "$CURSOR_3" -eq 0 ]] && echo pass || echo fail)
check "14. Cursor agent-3: 999 -> 0 (clamped) (got $CURSOR_3)" "$R"

# 13. Main file is still a valid chat file (can read, send, and read again)
"$NBS_CHAT" send "$CHAT_FILE" "verify" "post-archive verification message" 2>/dev/null
POST_COUNT=$("$NBS_CHAT" read "$CHAT_FILE" | wc -l)
R=$([[ "$POST_COUNT" -eq 1002 ]] && echo pass || echo fail)
check "15. Post-archive send works (got $POST_COUNT messages)" "$R"

# 14. Boundary test: populate up to exactly 2000 (should NOT trigger)
echo "Populating to boundary (2000 messages)..."
"$NBS_CHAT" create "$TEST_DIR/boundary.chat" 2>/dev/null
bulk_populate "$TEST_DIR/boundary.chat" 1999
"$NBS_CHAT" send "$TEST_DIR/boundary.chat" "boundary" "message 2000" 2>/dev/null
BOUNDARY_ARCHIVES=$(find "$TEST_DIR" -name 'boundary*-archive.chat' 2>/dev/null | wc -l)
R=$([[ "$BOUNDARY_ARCHIVES" -eq 0 ]] && echo pass || echo fail)
check "16. No archive at exactly 2000 messages" "$R"

# 15. One more sends it over
"$NBS_CHAT" send "$TEST_DIR/boundary.chat" "boundary" "message 2001" 2>/dev/null
BOUNDARY_ARCHIVES_2=$(find "$TEST_DIR" -name 'boundary*-archive.chat' 2>/dev/null | wc -l)
R=$([[ "$BOUNDARY_ARCHIVES_2" -eq 1 ]] && echo pass || echo fail)
check "17. Archive triggered at 2001 messages" "$R"

echo ""
echo "==========================="
if [[ $ERRORS -eq 0 ]]; then
    echo "All $TESTS tests passed."
else
    echo "$ERRORS of $TESTS tests FAILED."
fi
exit "$ERRORS"
