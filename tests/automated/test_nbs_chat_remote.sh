#!/bin/bash
# test_nbs_chat_remote.sh — Tests for nbs-chat-remote SSH proxy
#
# Tests the remote wrapper by SSH'ing to localhost (loopback).
# This exercises real SSH auth, command execution, argument quoting,
# and exit code propagation without needing a second machine.
#
# Prerequisites:
#   - ssh localhost works without password prompt
#   - nbs-chat and nbs-chat-remote are installed in bin/
#
# Exit code: 0 on success, 1 on any failure

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"
NBS_CHAT="$BIN_DIR/nbs-chat"
NBS_REMOTE="$BIN_DIR/nbs-chat-remote"
TEST_DIR=$(mktemp -d)

PASS=0
FAIL=0

pass() {
    PASS=$((PASS + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    echo "  FAIL: $1" >&2
}

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== nbs-chat-remote tests ==="
echo ""

# --- Prerequisite check: ssh connectivity ---
echo "Checking SSH connectivity..."
SSH_TEST_CMD="ssh -o BatchMode=yes -o ConnectTimeout=5"
if [ -n "${NBS_CHAT_PORT:-}" ]; then SSH_TEST_CMD="$SSH_TEST_CMD -p $NBS_CHAT_PORT"; fi
if [ -n "${NBS_CHAT_KEY:-}" ]; then SSH_TEST_CMD="$SSH_TEST_CMD -i $NBS_CHAT_KEY"; fi
if [ -n "${NBS_CHAT_OPTS:-}" ]; then
    IFS=',' read -ra _OPTS <<< "$NBS_CHAT_OPTS"
    for _opt in "${_OPTS[@]}"; do SSH_TEST_CMD="$SSH_TEST_CMD -o $_opt"; done
fi
SSH_TEST_HOST="${NBS_CHAT_HOST:-localhost}"
if ! $SSH_TEST_CMD "$SSH_TEST_HOST" "echo ok" >/dev/null 2>&1; then
    echo "SKIP: SSH connection to $SSH_TEST_HOST does not work"
    echo "For localhost: ssh-keygen && ssh-copy-id localhost"
    echo "For mock server: run test_nbs_chat_remote_mock.py"
    exit 0
fi
pass "SSH connectivity to $SSH_TEST_HOST"

# --- Configuration ---
# Use environment values if set (e.g. from mock harness), else defaults
export NBS_CHAT_HOST="${NBS_CHAT_HOST:-localhost}"
export NBS_CHAT_BIN="${NBS_CHAT_BIN:-$NBS_CHAT}"

# --- Test 1: Help (local, no SSH) ---
echo ""
echo "Test 1: Help command (local)"
OUTPUT=$("$NBS_REMOTE" help 2>&1)
if echo "$OUTPUT" | grep -q "SSH proxy for nbs-chat"; then
    pass "help output"
else
    fail "help output"
fi

# --- Test 2: Missing NBS_CHAT_HOST ---
echo ""
echo "Test 2: Missing configuration"
(
    unset NBS_CHAT_HOST
    set +e
    "$NBS_REMOTE" create "$TEST_DIR/should_fail.chat" >/dev/null 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 4 ]; then
        pass "exit 4 without NBS_CHAT_HOST"
    else
        fail "exit 4 without NBS_CHAT_HOST (got $rc)"
    fi
)

# --- Test 3: Create via remote ---
echo ""
echo "Test 3: Create via remote"
CHAT="$TEST_DIR/remote_test.chat"
if "$NBS_REMOTE" create "$CHAT" >/dev/null 2>&1; then
    if [ -f "$CHAT" ]; then
        pass "create via remote"
    else
        fail "create via remote (file not found)"
    fi
else
    fail "create via remote (non-zero exit)"
fi

# --- Test 4: Send via remote, read via local ---
echo ""
echo "Test 4: Send remote, read local"
"$NBS_REMOTE" send "$CHAT" "remote-alice" "Hello from remote"
OUTPUT=$("$NBS_CHAT" read "$CHAT")
if echo "$OUTPUT" | grep -qF "remote-alice: Hello from remote"; then
    pass "remote send readable locally"
else
    fail "remote send readable locally"
fi

# --- Test 5: Send via local, read via remote ---
echo ""
echo "Test 5: Send local, read remote"
"$NBS_CHAT" send "$CHAT" "local-bob" "Hello from local"
OUTPUT=$("$NBS_REMOTE" read "$CHAT")
if echo "$OUTPUT" | grep -qF "local-bob: Hello from local"; then
    pass "local send readable remotely"
else
    fail "local send readable remotely"
fi

# --- Test 6: Exit code passthrough ---
echo ""
echo "Test 6: Exit code passthrough"
set +e

"$NBS_REMOTE" read "$TEST_DIR/nonexistent.chat" >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 2 ]; then
    pass "exit 2 for missing file"
else
    fail "exit 2 for missing file (got $rc)"
fi

"$NBS_REMOTE" poll "$CHAT" "watcher" --timeout=1 >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 3 ]; then
    pass "exit 3 on timeout"
else
    fail "exit 3 on timeout (got $rc)"
fi

"$NBS_REMOTE" >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 4 ]; then
    pass "exit 4 for no command"
else
    fail "exit 4 for no command (got $rc)"
fi

set -e

# --- Test 7: Special characters ---
echo ""
echo "Test 7: Special characters survive SSH"

"$NBS_REMOTE" send "$CHAT" "tester" 'Quotes: "double" and end'
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --last=1)
if echo "$OUTPUT" | grep -qF 'tester: Quotes: "double" and end'; then
    pass "double quotes through SSH"
else
    fail "double quotes through SSH"
fi

"$NBS_REMOTE" send "$CHAT" "tester" "Apostrophe: it's fine"
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --last=1)
if echo "$OUTPUT" | grep -qF "tester: Apostrophe: it's fine"; then
    pass "single quotes through SSH"
else
    fail "single quotes through SSH"
fi

"$NBS_REMOTE" send "$CHAT" "tester" 'Backslash: path\to\file'
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --last=1)
if echo "$OUTPUT" | grep -qF 'tester: Backslash: path\to\file'; then
    pass "backslashes through SSH"
else
    fail "backslashes through SSH"
fi

"$NBS_REMOTE" send "$CHAT" "tester" 'Dollar: $HOME and more'
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --last=1)
if echo "$OUTPUT" | grep -qF 'tester: Dollar: $HOME and more'; then
    pass "dollar signs through SSH"
else
    fail "dollar signs through SSH"
fi

"$NBS_REMOTE" send "$CHAT" "tester" 'Spaces:    lots   of   them'
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --last=1)
if echo "$OUTPUT" | grep -qF 'tester: Spaces:    lots   of   them'; then
    pass "multiple spaces through SSH"
else
    fail "multiple spaces through SSH"
fi

# --- Test 8: Poll success ---
echo ""
echo "Test 8: Poll success through remote"
(
    sleep 2
    "$NBS_CHAT" send "$CHAT" "bg-sender" "Background message"
) &
BG_PID=$!

set +e
POLL_OUTPUT=$("$NBS_REMOTE" poll "$CHAT" "watcher" --timeout=10 2>/dev/null)
POLL_RC=$?
set -e
wait "$BG_PID" 2>/dev/null || true

if [ "$POLL_RC" -eq 0 ]; then
    pass "remote poll exit 0 on new message"
else
    fail "remote poll exit 0 on new message (got $POLL_RC)"
fi
if echo "$POLL_OUTPUT" | grep -qF "bg-sender: Background message"; then
    pass "remote poll got message content"
else
    fail "remote poll got message content"
fi

# --- Test 9: file-length integrity ---
echo ""
echo "Test 9: File-length integrity after remote operations"
HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$CHAT" | tr -d ' ')
if [ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]; then
    pass "file-length matches actual size"
else
    fail "file-length matches actual size (header=$HEADER_LENGTH actual=$ACTUAL_LENGTH)"
fi

# --- Test 10: Participants via remote ---
echo ""
echo "Test 10: Participants via remote"
OUTPUT=$("$NBS_REMOTE" participants "$CHAT")
if echo "$OUTPUT" | grep -qF "remote-alice"; then
    pass "participants shows remote-alice"
else
    fail "participants shows remote-alice"
fi
if echo "$OUTPUT" | grep -qF "local-bob"; then
    pass "participants shows local-bob"
else
    fail "participants shows local-bob"
fi

# --- Test 11: Read with --unread ---
echo ""
echo "Test 11: Read with --unread via remote"
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --unread=new-reader)
MSG_COUNT=$(echo "$OUTPUT" | wc -l | tr -d ' ')
if [ "$MSG_COUNT" -gt 0 ]; then
    pass "--unread shows messages for new reader"
else
    fail "--unread shows messages for new reader"
fi

# Second read should show nothing (cursor advanced)
OUTPUT=$("$NBS_REMOTE" read "$CHAT" --unread=new-reader)
if [ -z "$OUTPUT" ]; then
    pass "--unread shows nothing after cursor advance"
else
    fail "--unread shows nothing after cursor advance"
fi

# --- Summary ---
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
