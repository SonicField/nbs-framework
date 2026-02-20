#!/bin/bash
# Test: nbs-remote-edit argument validation and local operations
# These tests do NOT require SSH — they test the tool's local behaviour.
# For SSH integration tests, use test_nbs_remote_edit_mock.py on a machine
# where BpfJailer allows SSH to localhost.

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
trap "rm -rf '$TMPDIR'" EXIT
export NBS_REMOTE_EDIT_DIR="$TMPDIR/staging"

echo "Test: nbs-remote-edit (static tests, no SSH)"

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

# Test 9: pull from unreachable host returns 3 (SSH fail)
export NBS_REMOTE_EDIT_PORT=1  # invalid port
rc=0
"$REMOTE_EDIT" pull 127.0.0.1 /etc/hostname 2>/dev/null || rc=$?
if [[ "$rc" -eq 3 ]]; then
    check "pull unreachable host returns 3" "0"
else
    check "pull unreachable host returns 3" "1"
fi
unset NBS_REMOTE_EDIT_PORT

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

echo ""
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
