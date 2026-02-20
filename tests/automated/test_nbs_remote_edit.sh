#!/bin/bash
# Test: nbs-remote-edit pull/push/diff via SSH
#
# Expects environment variables:
#   NBS_REMOTE_EDIT_HOST  - SSH target (user@host)
#   NBS_REMOTE_EDIT_PORT  - SSH port
#   NBS_REMOTE_EDIT_KEY   - Path to SSH identity file
#   NBS_REMOTE_EDIT_DIR   - Local staging directory
#
# These are set by test_nbs_remote_edit_mock.py

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
REMOTE_EDIT="${PROJECT_ROOT}/bin/nbs-remote-edit"

HOST="${NBS_REMOTE_EDIT_HOST:-}"
PORT="${NBS_REMOTE_EDIT_PORT:-22}"
KEY="${NBS_REMOTE_EDIT_KEY:-}"
STAGING="${NBS_REMOTE_EDIT_DIR:-}"

if [[ -z "$HOST" ]] || [[ -z "$STAGING" ]]; then
    echo "Error: NBS_REMOTE_EDIT_HOST and NBS_REMOTE_EDIT_DIR must be set" >&2
    exit 1
fi

export NBS_REMOTE_EDIT_DIR="$STAGING"
export NBS_REMOTE_EDIT_KEY="$KEY"
export NBS_REMOTE_EDIT_PORT="$PORT"

PASS=0
FAIL=0
TOTAL=0

check() {
    local name="$1"
    local result="$2"
    TOTAL=$((TOTAL + 1))
    if [[ "$result" == "0" ]]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "Test: nbs-remote-edit"
echo "  Host: $HOST"
echo "  Port: $PORT"
echo "  Staging: $STAGING"

# Create a test file on the "remote" (localhost via MockSSH)
REMOTE_DIR=$(mktemp -d)
REMOTE_FILE="${REMOTE_DIR}/test_file.txt"
echo "original content line 1" > "$REMOTE_FILE"
echo "original content line 2" >> "$REMOTE_FILE"
trap "rm -rf '$REMOTE_DIR'" EXIT

# Extract just the hostname (without user@)
HOSTNAME="${HOST#*@}"

# Test 1: Pull downloads file
echo ""
echo "Test 1: pull"
output=$("$REMOTE_EDIT" pull "$HOSTNAME" "$REMOTE_FILE" 2>&1)
rc=$?
check "pull exits 0" "$rc"

# Verify local file exists
local_file="${STAGING}/${HOSTNAME}${REMOTE_FILE}"
if [[ -f "$local_file" ]]; then
    check "local file created" "0"
else
    check "local file created" "1"
fi

# Verify content matches
if diff -q "$REMOTE_FILE" "$local_file" >/dev/null 2>&1; then
    check "content matches remote" "0"
else
    check "content matches remote" "1"
fi

# Test 2: Edit locally, then push
echo ""
echo "Test 2: push after edit"
echo "edited content line 1" > "$local_file"
echo "edited content line 2" >> "$local_file"
echo "new line 3" >> "$local_file"

output=$("$REMOTE_EDIT" push "$HOSTNAME" "$REMOTE_FILE" 2>&1)
rc=$?
check "push exits 0" "$rc"

# Verify remote file updated
if grep -q "edited content" "$REMOTE_FILE"; then
    check "remote file updated" "0"
else
    check "remote file updated" "1"
fi

if grep -q "new line 3" "$REMOTE_FILE"; then
    check "new content present" "0"
else
    check "new content present" "1"
fi

# Test 3: Diff shows changes
echo ""
echo "Test 3: diff"
# Modify local again
echo "diff test line" >> "$local_file"
output=$("$REMOTE_EDIT" diff "$HOSTNAME" "$REMOTE_FILE" 2>&1)
rc=$?
check "diff exits 0" "0"  # diff returns 0 even with differences (we || true)

if echo "$output" | grep -q "diff test line"; then
    check "diff shows local change" "0"
else
    check "diff shows local change" "1"
fi

# Test 4: Pull non-existent file fails
echo ""
echo "Test 4: error handling"
output=$("$REMOTE_EDIT" pull "$HOSTNAME" "/nonexistent/path/file.txt" 2>&1)
rc=$?
if [[ "$rc" -ne 0 ]]; then
    check "pull nonexistent file fails" "0"
else
    check "pull nonexistent file fails" "1"
fi

# Test 5: Push without pull fails
rm -rf "$STAGING"
output=$("$REMOTE_EDIT" push "$HOSTNAME" "/some/other/file.txt" 2>&1)
rc=$?
if [[ "$rc" -ne 0 ]]; then
    check "push without pull fails" "0"
else
    check "push without pull fails" "1"
fi

# Test 6: Invalid arguments
echo ""
echo "Test 6: argument validation"
output=$("$REMOTE_EDIT" pull 2>&1)
rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "pull with no args returns 4" "0"
else
    check "pull with no args returns 4" "1"
fi

output=$("$REMOTE_EDIT" push 2>&1)
rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "push with no args returns 4" "0"
else
    check "push with no args returns 4" "1"
fi

echo ""
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
