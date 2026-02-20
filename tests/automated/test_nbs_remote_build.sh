#!/bin/bash
# Test: nbs-remote-build argument validation and basic behaviour
# Does NOT require SSH — tests local behaviour only.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
REMOTE_BUILD="${PROJECT_ROOT}/bin/nbs-remote-build"
PTY_SESSION="${HOME}/.nbs/bin/pty-session"

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

TEST_SESSION="test-build-$$"
cleanup() {
    "$PTY_SESSION" kill "$TEST_SESSION" 2>/dev/null || true
}
trap cleanup EXIT

echo "Test: nbs-remote-build"

# Test 1: help exits 0
echo ""
echo "Test 1: help and argument validation"
"$REMOTE_BUILD" --help >/dev/null 2>&1
check "help exits 0" "$?"

# Test 2: no args returns 4
rc=0
"$REMOTE_BUILD" 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "no args returns 4" "0"
else
    check "no args returns 4" "1"
fi

# Test 3: missing build command returns 4
rc=0
"$REMOTE_BUILD" somesession 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "missing build cmd returns 4" "0"
else
    check "missing build cmd returns 4" "1"
fi

# Test 4: nonexistent session returns 2
rc=0
"$REMOTE_BUILD" nonexistent_session_xyz 'echo hello' 2>/dev/null || rc=$?
if [[ "$rc" -eq 2 ]]; then
    check "nonexistent session returns 2" "0"
else
    check "nonexistent session returns 2" "1"
fi

# Test 5: chat without handle returns 4
rc=0
"$REMOTE_BUILD" somesession 'echo hello' --chat=.nbs/chat/live.chat 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "chat without handle returns 4" "0"
else
    check "chat without handle returns 4" "1"
fi

# Test 6: functional test — run a quick command and detect completion
echo ""
echo "Test 6: functional test"
"$PTY_SESSION" create "$TEST_SESSION" 'bash'
sleep 1

output=$("$REMOTE_BUILD" "$TEST_SESSION" 'echo BUILD_DONE_12345' \
    --prompt='\$' --timeout=10 --poll=1 --quiet 2>/dev/null) || true

if echo "$output" | grep -q "BUILD_DONE_12345"; then
    check "detects build completion" "0"
else
    check "detects build completion" "1"
fi

# Test 7: timeout on command that doesn't produce prompt
echo ""
echo "Test 7: timeout"
rc=0
"$REMOTE_BUILD" "$TEST_SESSION" 'sleep 5' \
    --prompt='NEVER_MATCH_THIS_PATTERN' --timeout=3 --poll=1 --quiet 2>/dev/null || rc=$?
if [[ "$rc" -eq 3 ]]; then
    check "timeout returns 3" "0"
else
    check "timeout returns 3" "1"
fi

echo ""
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
