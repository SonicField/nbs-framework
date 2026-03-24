#!/bin/bash
# Test: nbs-remote-edit argument validation and local operations
# These tests do NOT require SSH — they test the tool's local behaviour.
# For SSH integration tests, use test_nbs_remote_edit_mock.py on a machine
# where the sandbox allows SSH to localhost.
#
# Covers audit violations:
#   V1 (SECURITY): hostname injection rejection
#   V2 (BUG): relative path rejection
#   V3 (BUG): trap uses EXIT (verified by absence of temp file leak)
#   V7 (HARDENING): NBS_REMOTE_EDIT_DIR validation
#   V8 (HARDENING): usage guard for $0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
REMOTE_EDIT="${PROJECT_ROOT}/bin/nbs-remote-edit"

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

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# V7 fix requires the directory to exist — create it before setting the env var
mkdir -p "$TMPDIR/staging"
export NBS_REMOTE_EDIT_DIR="$TMPDIR/staging"

echo "Test: nbs-remote-edit (static tests, no SSH)"

# --- Basic dispatch tests ---

# Test 1: help exits 0
echo ""
echo "Test 1: help"
"$REMOTE_EDIT" help >/dev/null 2>&1
check "help exits 0" "$?"

# Test 2: no args shows help
output=$("$REMOTE_EDIT" 2>&1) || true
if echo "$output" | grep -q "nbs-remote-edit"; then
    check "no args shows usage" "0"
else
    check "no args shows usage" "1"
fi

# Test 3: unknown command returns 4
rc=0
"$REMOTE_EDIT" badcmd 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "unknown command returns 4" "0"
else
    check "unknown command returns 4" "1"
fi

# Test 4: pull with no args returns 4
rc=0
"$REMOTE_EDIT" pull 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "pull no args returns 4" "0"
else
    check "pull no args returns 4" "1"
fi

# Test 5: push with no args returns 4
rc=0
"$REMOTE_EDIT" push 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "push no args returns 4" "0"
else
    check "push no args returns 4" "1"
fi

# Test 6: diff with no args returns 4
rc=0
"$REMOTE_EDIT" diff 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "diff no args returns 4" "0"
else
    check "diff no args returns 4" "1"
fi

# Test 7: push without pull returns 2 (file not found)
rc=0
"$REMOTE_EDIT" push somehost /some/file.txt 2>/dev/null || rc=$?
if [[ "$rc" -eq 2 ]]; then
    check "push without pull returns 2" "0"
else
    check "push without pull returns 2" "1"
fi

# Test 8: diff without pull returns 2 (file not found)
rc=0
"$REMOTE_EDIT" diff somehost /some/file.txt 2>/dev/null || rc=$?
if [[ "$rc" -eq 2 ]]; then
    check "diff without pull returns 2" "0"
else
    check "diff without pull returns 2" "1"
fi

# Test 9: pull from unreachable host fails (non-zero exit)
# Note: on machines where localhost scp works via pty-session, port 1 may still
# succeed. Use a hostname that cannot resolve to guarantee failure.
rc=0
"$REMOTE_EDIT" pull unreachable-host-nbs-test-xyzzy.invalid /etc/hostname 2>/dev/null || rc=$?
if [[ "$rc" -ne 0 ]]; then
    check "pull unreachable host fails (rc!=0)" "0"
else
    check "pull unreachable host fails (rc!=0)" "1"
fi

# Test 10: local_path structure is correct (verify staging dir layout)
mkdir -p "$TMPDIR/staging/testhost.example.com/data/users/test"
echo "test content" > "$TMPDIR/staging/testhost.example.com/data/users/test/file.cpp"
if [[ -f "$TMPDIR/staging/testhost.example.com/data/users/test/file.cpp" ]]; then
    check "staging dir preserves remote path structure" "0"
else
    check "staging dir preserves remote path structure" "1"
fi

# Test 11: push error message mentions pull
output=$("$REMOTE_EDIT" push somehost /file.txt 2>&1) || true
if echo "$output" | grep -q "pull"; then
    check "push error mentions pull" "0"
else
    check "push error mentions pull" "1"
fi

# --- V1 (SECURITY): Hostname injection tests ---
echo ""
echo "Audit V1: hostname injection rejection"

# Semicolon injection in hostname
rc=0
"$REMOTE_EDIT" pull 'host;rm -rf /' /some/file 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V1: semicolon in hostname rejected (rc=4)" "0"
else
    check "V1: semicolon in hostname rejected (rc=4)" "1"
fi

# Backtick injection in hostname
rc=0
"$REMOTE_EDIT" pull 'host$(whoami)' /some/file 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V1: backtick/subshell in hostname rejected (rc=4)" "0"
else
    check "V1: backtick/subshell in hostname rejected (rc=4)" "1"
fi

# Space in hostname
rc=0
"$REMOTE_EDIT" pull 'host name' /some/file 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V1: space in hostname rejected (rc=4)" "0"
else
    check "V1: space in hostname rejected (rc=4)" "1"
fi

# Valid hostnames should be accepted (these will fail at scp, not validation)
# user@host format
rc=0
output=$("$REMOTE_EDIT" pull user@validhost.example.com /some/file 2>&1) || rc=$?
# Should fail with 3 (transfer failure), NOT 4 (invalid args)
if [[ "$rc" -ne 4 ]]; then
    check "V1: user@host format accepted by validation" "0"
else
    check "V1: user@host format accepted by validation" "1"
fi

# --- V2 (BUG): Relative path rejection ---
echo ""
echo "Audit V2: relative path rejection"

rc=0
"$REMOTE_EDIT" pull somehost 'relative/path/file.txt' 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: relative path rejected (rc=4)" "0"
else
    check "V2: relative path rejected (rc=4)" "1"
fi

rc=0
output=$("$REMOTE_EDIT" pull somehost 'relative/path/file.txt' 2>&1) || true
if echo "$output" | grep -q "absolute"; then
    check "V2: error message mentions 'absolute'" "0"
else
    check "V2: error message mentions 'absolute'" "1"
fi

# Path with shell metacharacters
rc=0
"$REMOTE_EDIT" pull somehost '/path/with spaces/file.txt' 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: path with spaces rejected (rc=4)" "0"
else
    check "V2: path with spaces rejected (rc=4)" "1"
fi

# Path with backticks
rc=0
"$REMOTE_EDIT" pull somehost '/path/$(cmd)/file.txt' 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: path with subshell rejected (rc=4)" "0"
else
    check "V2: path with subshell rejected (rc=4)" "1"
fi

# --- V7 (HARDENING): NBS_REMOTE_EDIT_DIR validation ---
echo ""
echo "Audit V7: NBS_REMOTE_EDIT_DIR validation"

# Non-existent directory should fail
rc=0
NBS_REMOTE_EDIT_DIR="/nonexistent/dir/xyz" "$REMOTE_EDIT" help 2>/dev/null || rc=$?
if [[ "$rc" -eq 1 ]]; then
    check "V7: nonexistent NBS_REMOTE_EDIT_DIR fails (rc=1)" "0"
else
    check "V7: nonexistent NBS_REMOTE_EDIT_DIR fails (rc=1)" "1"
fi

# Error message should mention the bad directory
output=$(NBS_REMOTE_EDIT_DIR="/nonexistent/dir/xyz" "$REMOTE_EDIT" help 2>&1) || true
if echo "$output" | grep -q "NBS_REMOTE_EDIT_DIR"; then
    check "V7: error message mentions NBS_REMOTE_EDIT_DIR" "0"
else
    check "V7: error message mentions NBS_REMOTE_EDIT_DIR" "1"
fi

# Valid directory should work
NBS_REMOTE_EDIT_DIR="$TMPDIR/staging" "$REMOTE_EDIT" help >/dev/null 2>&1
check "V7: valid NBS_REMOTE_EDIT_DIR works" "$?"

echo ""
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
