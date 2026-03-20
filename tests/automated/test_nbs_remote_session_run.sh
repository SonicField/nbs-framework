#!/bin/bash
# Test: nbs-remote-session and nbs-remote-run (static tests, no SSH)
#
# Tests argument validation, help text, error handling.
# Does NOT require SSH connectivity — tests local behaviour only.
# For integration tests, run manually against a real SSH target.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
REMOTE_SESSION="${PROJECT_ROOT}/bin/nbs-remote-session"
REMOTE_RUN="${PROJECT_ROOT}/bin/nbs-remote-run"

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
        echo "  FAIL: $name (exit $result)"
        FAIL=$((FAIL + 1))
    fi
}

check_fail() {
    local name="$1"
    local expected_rc="$2"
    local actual_rc="$3"
    TOTAL=$((TOTAL + 1))
    if [[ "$actual_rc" == "$expected_rc" ]]; then
        echo "  PASS: $name (correctly exited $actual_rc)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected exit $expected_rc, got $actual_rc)"
        FAIL=$((FAIL + 1))
    fi
}

echo "Test: nbs-remote-session and nbs-remote-run (static, no SSH)"
echo ""

# ═══════════════════════════════════════════════════════════════
# nbs-remote-session tests
# ═══════════════════════════════════════════════════════════════

echo "=== nbs-remote-session ==="

# Test 1: scripts exist and are executable
echo ""
echo "Test 1: scripts exist and are executable"
[[ -x "$REMOTE_SESSION" ]]; check "nbs-remote-session is executable" $?
[[ -x "$REMOTE_RUN" ]]; check "nbs-remote-run is executable" $?

# Test 2: help exits 0
echo ""
echo "Test 2: help"
"$REMOTE_SESSION" --help >/dev/null 2>&1; check "nbs-remote-session --help" $?
"$REMOTE_SESSION" -h >/dev/null 2>&1; check "nbs-remote-session -h" $?

# Test 3: no arguments exits 2
echo ""
echo "Test 3: missing arguments"
rc=0; "$REMOTE_SESSION" >/dev/null 2>&1 || rc=$?
check_fail "nbs-remote-session (no args)" 2 "$rc"

# Test 4: unknown option exits 2
echo ""
echo "Test 4: unknown option"
rc=0; "$REMOTE_SESSION" --bogus=foo somehost >/dev/null 2>&1 || rc=$?
check_fail "nbs-remote-session --bogus" 2 "$rc"

# Test 5: help text contains usage
echo ""
echo "Test 5: help text content"
output=$("$REMOTE_SESSION" --help 2>&1)
echo "$output" | grep -q 'Usage:'; check "help contains Usage:" $?
echo "$output" | grep -q 'pty-session'; check "help mentions pty-session" $?
echo "$output" | grep -q -- '--name='; check "help mentions --name" $?
echo "$output" | grep -q -- '--cwd='; check "help mentions --cwd" $?

# ═══════════════════════════════════════════════════════════════
# nbs-remote-run tests
# ═══════════════════════════════════════════════════════════════

echo ""
echo "=== nbs-remote-run ==="

# Test 6: help exits 0
echo ""
echo "Test 6: help"
"$REMOTE_RUN" --help >/dev/null 2>&1; check "nbs-remote-run --help" $?
"$REMOTE_RUN" -h >/dev/null 2>&1; check "nbs-remote-run -h" $?

# Test 7: no arguments exits 2
echo ""
echo "Test 7: missing arguments"
rc=0; "$REMOTE_RUN" >/dev/null 2>&1 || rc=$?
check_fail "nbs-remote-run (no args)" 2 "$rc"

# Test 8: host but no command exits 2
echo ""
echo "Test 8: host but no command"
rc=0; "$REMOTE_RUN" somehost >/dev/null 2>&1 || rc=$?
check_fail "nbs-remote-run (no command)" 2 "$rc"

# Test 9: unknown option exits 2
echo ""
echo "Test 9: unknown option"
rc=0; "$REMOTE_RUN" --bogus somehost 'echo hi' >/dev/null 2>&1 || rc=$?
check_fail "nbs-remote-run --bogus" 2 "$rc"

# Test 10: help text contains usage
echo ""
echo "Test 10: help text content"
output=$("$REMOTE_RUN" --help 2>&1)
echo "$output" | grep -q 'Usage:'; check "help contains Usage:" $?
echo "$output" | grep -q -- '--cwd='; check "help mentions --cwd" $?
echo "$output" | grep -q -- '--timeout='; check "help mentions --timeout" $?
echo "$output" | grep -q 'Exit codes:'; check "help mentions exit codes" $?

# ═══════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════

echo ""
echo "────────────────────────────────────────"
echo "Results: ${PASS} pass, ${FAIL} fail, ${TOTAL} total"
echo "────────────────────────────────────────"

[[ "$FAIL" -eq 0 ]]
