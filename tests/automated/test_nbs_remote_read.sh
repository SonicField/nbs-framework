#!/bin/bash
# Test: nbs-remote-read argument validation and input sanitisation
# These tests do NOT require SSH — they test the tool's local behaviour
# (argument parsing, input validation, and output filtering).
#
# Covers audit violations:
#   V1 (BUG): trap quoting
#   V2 (BUG): MODE_ARG validation for numeric/pattern correctness
#   V3 (SECURITY): head/tail flag injection rejection
#   V4 (SECURITY): grep pattern-as-flag injection rejection
#   V5 (HARDENING): grep error vs no-match distinction
#   V9 (HARDENING): session name uniqueness (verified structurally)
#   V10 (HARDENING): reversed --lines range rejection
#   V12 (HARDENING): empty/malformed HOST and REMOTE_PATH rejection
#
# Falsification: each test defines a specific exit code or output pattern.
# If the tool accepts input it should reject, the test fails.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
REMOTE_READ="${PROJECT_ROOT}/bin/nbs-remote-read"

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

echo "Test: nbs-remote-read (static tests, no SSH)"

# --- Basic dispatch tests ---
echo ""
echo "Section: basic dispatch"

# help exits 0
"$REMOTE_READ" help >/dev/null 2>&1
check "help exits 0" "$?"

"$REMOTE_READ" --help >/dev/null 2>&1
check "--help exits 0" "$?"

# no args shows help (exits 0)
rc=0
"$REMOTE_READ" >/dev/null 2>&1 || rc=$?
check "no args exits 0 (shows help)" "$rc"

# too few args returns 4
rc=0
"$REMOTE_READ" onearg 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "one arg returns 4" "0"
else
    check "one arg returns 4" "1"
fi

# unknown option returns 4
rc=0
"$REMOTE_READ" somehost /some/file --badopt 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "unknown option returns 4" "0"
else
    check "unknown option returns 4" "1"
fi

# --- V12 (HARDENING): Empty/malformed HOST and REMOTE_PATH ---
echo ""
echo "Section: V12 — input validation for HOST and REMOTE_PATH"

# Empty hostname
rc=0
"$REMOTE_READ" "" /some/file 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V12: empty hostname rejected (rc=4)" "0"
else
    check "V12: empty hostname rejected (rc=4)" "1"
fi

# Empty remote path
rc=0
"$REMOTE_READ" somehost "" 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V12: empty remote path rejected (rc=4)" "0"
else
    check "V12: empty remote path rejected (rc=4)" "1"
fi

# Hostname with shell metacharacters
rc=0
"$REMOTE_READ" 'host;whoami' /some/file 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V12: hostname with semicolon rejected (rc=4)" "0"
else
    check "V12: hostname with semicolon rejected (rc=4)" "1"
fi

rc=0
"$REMOTE_READ" 'host$(cmd)' /some/file 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V12: hostname with subshell rejected (rc=4)" "0"
else
    check "V12: hostname with subshell rejected (rc=4)" "1"
fi

# Relative remote path
rc=0
"$REMOTE_READ" somehost 'relative/path' 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V12: relative remote path rejected (rc=4)" "0"
else
    check "V12: relative remote path rejected (rc=4)" "1"
fi

# Remote path with shell metacharacters
rc=0
"$REMOTE_READ" somehost '/path/$(cmd)/file' 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V12: path with subshell rejected (rc=4)" "0"
else
    check "V12: path with subshell rejected (rc=4)" "1"
fi

# Valid user@host format should NOT be rejected at validation stage
rc=0
"$REMOTE_READ" user@validhost.example.com /some/file 2>/dev/null || rc=$?
if [[ "$rc" -ne 4 ]]; then
    check "V12: user@host format accepted by validation" "0"
else
    check "V12: user@host format accepted by validation" "1"
fi

# --- V2/V3 (BUG/SECURITY): MODE_ARG validation ---
echo ""
echo "Section: V2/V3 — MODE_ARG validation"

# --head with non-numeric
rc=0
"$REMOTE_READ" somehost /some/file --head=abc 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: --head=abc rejected (rc=4)" "0"
else
    check "V2: --head=abc rejected (rc=4)" "1"
fi

# --head with zero
rc=0
"$REMOTE_READ" somehost /some/file --head=0 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: --head=0 rejected (rc=4)" "0"
else
    check "V2: --head=0 rejected (rc=4)" "1"
fi

# --tail with non-numeric
rc=0
"$REMOTE_READ" somehost /some/file --tail=xyz 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: --tail=xyz rejected (rc=4)" "0"
else
    check "V2: --tail=xyz rejected (rc=4)" "1"
fi

# V3 (SECURITY): --head with flag injection attempt
rc=0
"$REMOTE_READ" somehost /some/file --head=--version 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V3: --head=--version rejected (rc=4)" "0"
else
    check "V3: --head=--version rejected (rc=4)" "1"
fi

# V3 (SECURITY): --tail with -f injection attempt
rc=0
"$REMOTE_READ" somehost /some/file --tail=-f 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V3: --tail=-f rejected (rc=4)" "0"
else
    check "V3: --tail=-f rejected (rc=4)" "1"
fi

# --lines with bad format
rc=0
"$REMOTE_READ" somehost /some/file --lines=abc 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: --lines=abc rejected (rc=4)" "0"
else
    check "V2: --lines=abc rejected (rc=4)" "1"
fi

rc=0
"$REMOTE_READ" somehost /some/file --lines=10 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V2: --lines=10 (no range) rejected (rc=4)" "0"
else
    check "V2: --lines=10 (no range) rejected (rc=4)" "1"
fi

# V10 (HARDENING): reversed range
rc=0
"$REMOTE_READ" somehost /some/file --lines=20-10 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V10: --lines=20-10 (reversed) rejected (rc=4)" "0"
else
    check "V10: --lines=20-10 (reversed) rejected (rc=4)" "1"
fi

# Valid range should not be rejected at validation
rc=0
"$REMOTE_READ" somehost /some/file --lines=10-20 2>/dev/null || rc=$?
if [[ "$rc" -ne 4 ]]; then
    check "V10: --lines=10-20 accepted by validation" "0"
else
    check "V10: --lines=10-20 accepted by validation" "1"
fi

# --grep with empty pattern
rc=0
"$REMOTE_READ" somehost /some/file --grep= 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "V4: --grep= (empty) rejected (rc=4)" "0"
else
    check "V4: --grep= (empty) rejected (rc=4)" "1"
fi

# --- V1 (BUG): Trap quoting verification ---
# We verify structurally: the script source must use single-quoted trap
echo ""
echo "Section: V1 — trap quoting"

if grep -q "trap 'rm -f \"\$TMP\"' EXIT" "$REMOTE_READ"; then
    check "V1: trap uses single-quoted deferred expansion" "0"
else
    check "V1: trap uses single-quoted deferred expansion" "1"
fi

# --- V6/V7 (BUG): pty-session error checking ---
# Verify structurally: the fetch_file function must check create/send return codes
echo ""
echo "Section: V6/V7 — pty-session error checking (structural)"

if grep -q 'if ! "\$PTY_SESSION" create' "$REMOTE_READ"; then
    check "V6: pty-session create failure checked" "0"
elif grep -q 'if ! .*pty-session.*create' "$REMOTE_READ"; then
    check "V6: pty-session create failure checked" "0"
else
    # Check for the pattern with the variable
    if grep -q '! "\$PTY_SESSION" create\|! "$PTY_SESSION" create' "$REMOTE_READ"; then
        check "V6: pty-session create failure checked" "0"
    else
        check "V6: pty-session create failure checked" "1"
    fi
fi

if grep -q '! "\$PTY_SESSION" send\|! "$PTY_SESSION" send' "$REMOTE_READ"; then
    check "V7: pty-session send failure checked" "0"
else
    check "V7: pty-session send failure checked" "1"
fi

# --- V9 (HARDENING): Session name uniqueness ---
echo ""
echo "Section: V9 — session name uniqueness (structural)"

if grep -q 'RANDOM' "$REMOTE_READ"; then
    check "V9: session name includes RANDOM component" "0"
else
    check "V9: session name includes RANDOM component" "1"
fi

# --- V4 (SECURITY): grep -- separator (structural) ---
echo ""
echo "Section: V4 — grep uses -- separator (structural)"

if grep -q 'grep.*-- "\$MODE_ARG"' "$REMOTE_READ"; then
    check "V4: grep uses -- to separate pattern from flags" "0"
else
    check "V4: grep uses -- to separate pattern from flags" "1"
fi

# --- V5 (HARDENING): grep error handling (structural) ---
echo ""
echo "Section: V5 — grep error handling (structural)"

if grep -q 'grep_rc' "$REMOTE_READ"; then
    check "V5: grep exit code captured for analysis" "0"
else
    check "V5: grep exit code captured for analysis" "1"
fi

# Check that || true is NOT used for grep in the filter section
if grep -q 'grep.*|| true' "$REMOTE_READ"; then
    check "V5: grep || true removed (no silent swallow)" "1"
else
    check "V5: grep || true removed (no silent swallow)" "0"
fi

echo ""
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
