#!/bin/bash
# Test: chat/main.c audit violation fixes
#
# Integration tests for the 12 violations fixed in chat/main.c:
#   1.  Path too long rejection (Violation 5: SECURITY)
#   2.  snprintf truncation detection via resolve_path (Violation 1: BUG)
#   3.  Empty argument value warnings (Violation 7: HARDENING)
#   4.  TOCTOU removal — file-not-found via chat_read errno (Violation 6: SECURITY)
#   5.  Error messages include path and errno context (Violation 9: HARDENING)
#   6.  Consistent path resolution across all commands (Violation 11: HARDENING)
#
# Note: Violations 2 (chat_poll assert), 3 (chat_read after poll), 4 (bounds
# assert), 8 (edge docs), 10 (void cast), 12 (postcondition assert) are
# compile-time or runtime-only checks that cannot be tested without building.
# They are verified by code inspection.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

# Verify binary exists
if [[ ! -x "$NBS_CHAT" ]]; then
    echo "SKIP: nbs-chat binary not found at $NBS_CHAT (not yet built?)"
    exit 0
fi

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

echo "=== chat/main.c Audit Fix Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Path too long rejection ---
echo "1. Path too long rejection (Violation 5: SECURITY)..."

# Generate a path exceeding MAX_PATH_LEN (4096)
LONG_DIR="$TEST_DIR"
while [[ ${#LONG_DIR} -lt 4200 ]]; do
    LONG_DIR="${LONG_DIR}/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
done
LONG_PATH="${LONG_DIR}/test.chat"

set +e

# Test create with too-long path
CREATE_ERR=$("$NBS_CHAT" create "$LONG_PATH" 2>&1)
CREATE_RC=$?
check "create rejects path >MAX_PATH_LEN" "$( [[ "$CREATE_RC" -ne 0 ]] && echo pass || echo fail )"
check "create error mentions 'too long'" "$( echo "$CREATE_ERR" | grep -qi 'too long' && echo pass || echo fail )"

# Test read with too-long path
READ_ERR=$("$NBS_CHAT" read "$LONG_PATH" 2>&1)
READ_RC=$?
check "read rejects path >MAX_PATH_LEN" "$( [[ "$READ_RC" -ne 0 ]] && echo pass || echo fail )"
check "read error mentions 'too long'" "$( echo "$READ_ERR" | grep -qi 'too long' && echo pass || echo fail )"

# Test send with too-long path
SEND_ERR=$("$NBS_CHAT" send "$LONG_PATH" "handle" "msg" 2>&1)
SEND_RC=$?
check "send rejects path >MAX_PATH_LEN" "$( [[ "$SEND_RC" -ne 0 ]] && echo pass || echo fail )"
check "send error mentions 'too long'" "$( echo "$SEND_ERR" | grep -qi 'too long' && echo pass || echo fail )"

# Test search with too-long path
SEARCH_ERR=$("$NBS_CHAT" search "$LONG_PATH" "pattern" 2>&1)
SEARCH_RC=$?
check "search rejects path >MAX_PATH_LEN" "$( [[ "$SEARCH_RC" -ne 0 ]] && echo pass || echo fail )"

# Test participants with too-long path
PART_ERR=$("$NBS_CHAT" participants "$LONG_PATH" 2>&1)
PART_RC=$?
check "participants rejects path >MAX_PATH_LEN" "$( [[ "$PART_RC" -ne 0 ]] && echo pass || echo fail )"

# Test poll with too-long path
POLL_ERR=$("$NBS_CHAT" poll "$LONG_PATH" "handle" --timeout=0 2>&1)
POLL_RC=$?
check "poll rejects path >MAX_PATH_LEN" "$( [[ "$POLL_RC" -ne 0 ]] && echo pass || echo fail )"

set -e
echo ""

# --- Test 2: snprintf truncation detection ---
echo "2. snprintf truncation detection (Violation 1: BUG)..."

# Create a relative path that, when combined with cwd, exceeds MAX_PATH_LEN.
# cwd is typically ~50-200 chars. We need cwd + "/" + relative >= 4096.
CWD_LEN=${#PWD}
# We need a relative path of length (4096 - CWD_LEN - 1) to just exceed the limit
NEEDED=$((4096 - CWD_LEN))
REL_PATH=""
while [[ ${#REL_PATH} -lt $NEEDED ]]; do
    REL_PATH="${REL_PATH}x"
done
REL_PATH="${REL_PATH}.chat"

set +e
TRUNC_ERR=$("$NBS_CHAT" create "$REL_PATH" 2>&1)
TRUNC_RC=$?
set -e

# Should fail with a path-too-long error, not silently truncate
check "snprintf truncation detected (exit != 0)" "$( [[ "$TRUNC_RC" -ne 0 ]] && echo pass || echo fail )"
check "snprintf truncation error message present" "$( echo "$TRUNC_ERR" | grep -qi 'too long\|Resolved path' && echo pass || echo fail )"

echo ""

# --- Test 3: Empty argument value warnings ---
echo "3. Empty argument value warnings (Violation 7: HARDENING)..."

CHAT="$TEST_DIR/test3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" "alice" "Hello" >/dev/null

set +e

# --since= with empty value should warn
SINCE_ERR=$("$NBS_CHAT" read "$CHAT" --since= 2>&1)
SINCE_RC=$?
check "--since= empty warns" "$( echo "$SINCE_ERR" | grep -qi 'empty' && echo pass || echo fail )"
check "--since= empty still exits 0" "$( [[ "$SINCE_RC" -eq 0 ]] && echo pass || echo fail )"

# --unread= with empty value should warn
UNREAD_ERR=$("$NBS_CHAT" read "$CHAT" --unread= 2>&1)
UNREAD_RC=$?
check "--unread= empty warns" "$( echo "$UNREAD_ERR" | grep -qi 'empty' && echo pass || echo fail )"
check "--unread= empty still exits 0" "$( [[ "$UNREAD_RC" -eq 0 ]] && echo pass || echo fail )"

# --handle= with empty value should warn
HANDLE_ERR=$("$NBS_CHAT" search "$CHAT" "Hello" --handle= 2>&1)
HANDLE_RC=$?
check "--handle= empty warns" "$( echo "$HANDLE_ERR" | grep -qi 'empty' && echo pass || echo fail )"
check "--handle= empty still exits 0" "$( [[ "$HANDLE_RC" -eq 0 ]] && echo pass || echo fail )"

set -e
echo ""

# --- Test 4: TOCTOU removal — error messages for missing files ---
echo "4. TOCTOU removal — file-not-found errors (Violation 6: SECURITY)..."

MISSING="$TEST_DIR/does_not_exist.chat"

set +e

# read on missing file should return exit code 2
"$NBS_CHAT" read "$MISSING" >/dev/null 2>&1
check "read missing file exits 2" "$( [[ $? -eq 2 ]] && echo pass || echo fail )"

# participants on missing file should return exit code 2
"$NBS_CHAT" participants "$MISSING" >/dev/null 2>&1
check "participants missing file exits 2" "$( [[ $? -eq 2 ]] && echo pass || echo fail )"

# search on missing file should return exit code 2
"$NBS_CHAT" search "$MISSING" "pattern" >/dev/null 2>&1
check "search missing file exits 2" "$( [[ $? -eq 2 ]] && echo pass || echo fail )"

set -e
echo ""

# --- Test 5: Error messages include path context ---
echo "5. Error messages include path context (Violation 9: HARDENING)..."

MISSING="$TEST_DIR/error_context_test.chat"

set +e
ERR_OUTPUT=$("$NBS_CHAT" read "$MISSING" 2>&1)
set -e

# The error message should mention the path
check "Error includes path" "$( echo "$ERR_OUTPUT" | grep -qF "error_context_test.chat" && echo pass || echo fail )"

echo ""

# --- Test 6: Consistent path resolution ---
echo "6. Consistent path resolution across commands (Violation 11: HARDENING)..."

# Create a chat file using a relative path
CHAT_NAME="test6_resolve.chat"
CHAT_ABS="$TEST_DIR/$CHAT_NAME"

# Create using absolute path
"$NBS_CHAT" create "$CHAT_ABS" >/dev/null

# Send via absolute path
"$NBS_CHAT" send "$CHAT_ABS" "alice" "Test message" >/dev/null

# Read via absolute path
READ_OUTPUT=$("$NBS_CHAT" read "$CHAT_ABS")
check "Read via absolute path works" "$( echo "$READ_OUTPUT" | grep -qF 'alice: Test message' && echo pass || echo fail )"

# Participants via absolute path
PART_OUTPUT=$("$NBS_CHAT" participants "$CHAT_ABS")
check "Participants via absolute path works" "$( echo "$PART_OUTPUT" | grep -qF 'alice' && echo pass || echo fail )"

# Search via absolute path
SEARCH_OUTPUT=$("$NBS_CHAT" search "$CHAT_ABS" "Test")
check "Search via absolute path works" "$( echo "$SEARCH_OUTPUT" | grep -qF 'alice' && echo pass || echo fail )"

echo ""

# --- Test 7: Existing lifecycle tests still pass ---
echo "7. Regression: basic lifecycle still works..."

REG_CHAT="$TEST_DIR/regression.chat"
"$NBS_CHAT" create "$REG_CHAT" >/dev/null
check "Create succeeds" "$( [[ -f "$REG_CHAT" ]] && echo pass || echo fail )"

"$NBS_CHAT" send "$REG_CHAT" "bob" "Regression test message" >/dev/null
REG_READ=$("$NBS_CHAT" read "$REG_CHAT")
check "Send+read round-trip" "$( echo "$REG_READ" | grep -qF 'bob: Regression test message' && echo pass || echo fail )"

# --last still works
"$NBS_CHAT" send "$REG_CHAT" "bob" "Second message" >/dev/null
LAST_READ=$("$NBS_CHAT" read "$REG_CHAT" --last=1)
check "--last=1 returns one message" "$( echo "$LAST_READ" | wc -l | awk '{print ($1 == 1) ? "pass" : "fail"}' )"
check "--last=1 returns latest" "$( echo "$LAST_READ" | grep -qF 'Second message' && echo pass || echo fail )"

# --since still works
"$NBS_CHAT" send "$REG_CHAT" "alice" "After bob" >/dev/null
SINCE_READ=$("$NBS_CHAT" read "$REG_CHAT" --since=bob)
check "--since=bob returns message after bob" "$( echo "$SINCE_READ" | grep -qF 'alice: After bob' && echo pass || echo fail )"

# poll timeout still returns 3
set +e
"$NBS_CHAT" poll "$REG_CHAT" "watcher" --timeout=1 >/dev/null 2>&1
check "Poll timeout returns 3" "$( [[ $? -eq 3 ]] && echo pass || echo fail )"
set -e

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
