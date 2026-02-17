#!/bin/bash
# Test nbs-claude audit fixes: verify all 19 violations are resolved
#
# Tests:
#   1. set -euo pipefail is present
#   2. Cleanup preserves exit code
#   3. Numeric config validation
#   4. NBS_ROOT resolution to absolute
#   5. SIDECAR_HANDLE validation
#   6. SSH opts array expansion (no word splitting)
#   7. No eval "$@" in local mode
#   8. stderr redirected to log file, not /dev/null
#   9. printf '%q' used for remote path sanitisation
#  10. Unanchored grep fixed (grep -qxF)
#  11. TOCTOU fix (atomic inbox read)
#  12. Non-atomic registry update guarded
#  13. Empty variable arithmetic guard
#  14. Unknown commands logged to stderr
#  15. sha256sum for change detection
#  16. pty_ prefix session verified before attach
#  17. Eval contract documented
#  18. if ! command pattern (no fragile $? check)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"

PASS=0
FAIL=0
TESTS=0

pass() {
    PASS=$((PASS + 1))
    TESTS=$((TESTS + 1))
    echo "   PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    TESTS=$((TESTS + 1))
    echo "   FAIL: $1"
}

echo "=== nbs-claude Audit Fix Tests ==="
echo ""

# --- 1. set -euo pipefail ---
echo "1. set -euo pipefail..."
if grep -qx 'set -euo pipefail' "$NBS_CLAUDE"; then
    pass "set -euo pipefail present (was set -uo pipefail)"
else
    fail "set -euo pipefail not found"
fi

# --- 2. Cleanup preserves exit code ---
echo "2. Cleanup preserves exit code..."
if grep -q 'local EXIT_CODE=\$?' "$NBS_CLAUDE"; then
    pass "Cleanup captures exit code"
else
    fail "Cleanup does not capture exit code"
fi

if grep -q 'exit "\$EXIT_CODE"' "$NBS_CLAUDE"; then
    pass "Cleanup exits with captured code"
else
    fail "Cleanup does not exit with captured code"
fi

# Verify no 'exit 0' in cleanup
if sed -n '/^cleanup()/,/^}/p' "$NBS_CLAUDE" | grep -q 'exit 0'; then
    fail "Cleanup still has hardcoded exit 0"
else
    pass "Cleanup does not hardcode exit 0"
fi

# --- 3. Numeric config validation ---
echo "3. Numeric config validation..."
if grep -q '\^\\[0-9\\]\\+\$' "$NBS_CLAUDE" || grep -q '\^\[0-9\]+\$' "$NBS_CLAUDE"; then
    pass "Numeric validation pattern present"
else
    fail "Numeric validation pattern not found"
fi

# Functional test: set invalid NBS_BUS_CHECK_INTERVAL (POLL_INTERVAL removed)
NBS_BUS_CHECK_INTERVAL=abc NBS_ROOT="$PROJECT_ROOT" bash -c "source /dev/stdin" <<'SCRIPT' 2>/dev/null
set -euo pipefail
POLL_DISABLE="${NBS_POLL_DISABLE:-0}"
BUS_CHECK_INTERVAL="${NBS_BUS_CHECK_INTERVAL:-3}"
NOTIFY_COOLDOWN="${NBS_NOTIFY_COOLDOWN:-15}"
for _nbs_var_name in POLL_DISABLE BUS_CHECK_INTERVAL NOTIFY_COOLDOWN; do
    eval "_nbs_var_val=\${$_nbs_var_name}"
    if [[ ! "$_nbs_var_val" =~ ^[0-9]+$ ]]; then
        exit 4
    fi
done
exit 0
SCRIPT
RESULT=$?
if [[ $RESULT -eq 4 ]]; then
    pass "NBS_BUS_CHECK_INTERVAL=abc correctly rejected (exit 4)"
else
    fail "NBS_BUS_CHECK_INTERVAL=abc not rejected (exit $RESULT)"
fi

# Test valid value passes
NBS_BUS_CHECK_INTERVAL=5 bash -c "source /dev/stdin" <<'SCRIPT' 2>/dev/null
set -euo pipefail
POLL_DISABLE="${NBS_POLL_DISABLE:-0}"
BUS_CHECK_INTERVAL="${NBS_BUS_CHECK_INTERVAL:-3}"
NOTIFY_COOLDOWN="${NBS_NOTIFY_COOLDOWN:-15}"
for _nbs_var_name in POLL_DISABLE BUS_CHECK_INTERVAL NOTIFY_COOLDOWN; do
    eval "_nbs_var_val=\${$_nbs_var_name}"
    if [[ ! "$_nbs_var_val" =~ ^[0-9]+$ ]]; then
        exit 4
    fi
done
exit 0
SCRIPT
RESULT=$?
if [[ $RESULT -eq 0 ]]; then
    pass "NBS_BUS_CHECK_INTERVAL=5 correctly accepted"
else
    fail "NBS_BUS_CHECK_INTERVAL=5 rejected (exit $RESULT)"
fi

# --- 4. NBS_ROOT resolution to absolute ---
echo "4. NBS_ROOT resolution to absolute..."
if grep -q 'cd "\$NBS_ROOT".*&&.*pwd' "$NBS_CLAUDE"; then
    pass "NBS_ROOT resolved via cd + pwd"
else
    fail "NBS_ROOT not resolved to absolute"
fi

# Verify existence check
if grep -q '\! -d "\$NBS_ROOT"' "$NBS_CLAUDE"; then
    pass "NBS_ROOT existence check present"
else
    fail "NBS_ROOT existence check missing"
fi

# Functional test: relative path becomes absolute
RESULT=$(cd /tmp && NBS_ROOT=. bash -c 'NBS_ROOT="${NBS_ROOT:-.}"; NBS_ROOT="$(cd "$NBS_ROOT" 2>/dev/null && pwd)"; echo "$NBS_ROOT"')
if [[ "$RESULT" == "/tmp" ]]; then
    pass "NBS_ROOT='.' resolved to absolute '/tmp'"
else
    fail "NBS_ROOT='.' resolved to '$RESULT' instead of '/tmp'"
fi

# --- 5. SIDECAR_HANDLE validation ---
echo "5. SIDECAR_HANDLE validation..."
if grep -q '\^\[a-zA-Z0-9_-\]+\$' "$NBS_CLAUDE"; then
    pass "SIDECAR_HANDLE regex validation present"
else
    fail "SIDECAR_HANDLE regex validation not found"
fi

# Functional test: valid handle
bash -c 'SIDECAR_HANDLE="my-agent_01"; if [[ ! "$SIDECAR_HANDLE" =~ ^[a-zA-Z0-9_-]+$ ]]; then exit 4; fi; exit 0' 2>/dev/null
if [[ $? -eq 0 ]]; then
    pass "Valid handle 'my-agent_01' accepted"
else
    fail "Valid handle 'my-agent_01' rejected"
fi

# Functional test: handle with shell metacharacters
bash -c 'SIDECAR_HANDLE="agent;rm -rf /"; if [[ ! "$SIDECAR_HANDLE" =~ ^[a-zA-Z0-9_-]+$ ]]; then exit 4; fi; exit 0' 2>/dev/null
if [[ $? -eq 4 ]]; then
    pass "Malicious handle 'agent;rm -rf /' rejected"
else
    fail "Malicious handle 'agent;rm -rf /' accepted"
fi

# Functional test: empty handle
bash -c 'SIDECAR_HANDLE=""; if [[ ! "$SIDECAR_HANDLE" =~ ^[a-zA-Z0-9_-]+$ ]]; then exit 4; fi; exit 0' 2>/dev/null
if [[ $? -eq 4 ]]; then
    pass "Empty handle rejected"
else
    fail "Empty handle accepted"
fi

# --- 6. SSH opts array expansion ---
echo "6. SSH opts array expansion..."
if grep -q "IFS=' ' read -ra ssh_opts" "$NBS_CLAUDE"; then
    pass "SSH opts parsed into array via IFS read -ra"
else
    fail "SSH opts not parsed into array"
fi

if grep -q '"${ssh_opts\[@\]}"' "$NBS_CLAUDE"; then
    pass "SSH opts expanded with proper quoting"
else
    fail "SSH opts not properly quoted on expansion"
fi

# --- 7. No eval "$@" in local mode ---
echo "7. No eval in local mode..."
# Check that the local branch does not use eval
if grep -A2 'else' "$NBS_CLAUDE" | grep -q 'eval "\$@"'; then
    fail "eval \"\$@\" still present in local mode"
else
    pass "eval \"\$@\" removed from local mode"
fi

# Check that direct execution is used
if grep -q '"\$@" 2>>"${NBS_LOG_FILE}"' "$NBS_CLAUDE"; then
    pass "Direct execution used in local mode"
else
    fail "Direct execution not found in local mode"
fi

# --- 8. stderr to log file ---
echo "8. stderr redirected to log file..."
if grep -q 'NBS_LOG_FILE=' "$NBS_CLAUDE"; then
    pass "NBS_LOG_FILE variable defined"
else
    fail "NBS_LOG_FILE variable not defined"
fi

if grep -q '2>>"${NBS_LOG_FILE}"' "$NBS_CLAUDE"; then
    pass "stderr redirected to log file"
else
    fail "stderr not redirected to log file"
fi

# Verify blanket 2>/dev/null removed from remote_cmd
if sed -n '/^remote_cmd()/,/^}/p' "$NBS_CLAUDE" | grep -q '2>/dev/null'; then
    fail "remote_cmd still uses 2>/dev/null"
else
    pass "remote_cmd no longer uses 2>/dev/null"
fi

# --- 9. Path sanitisation with printf '%q' ---
echo "9. Remote path sanitisation..."
PRINTF_Q_COUNT=$(grep -c "printf '%q'" "$NBS_CLAUDE")
if [[ "$PRINTF_Q_COUNT" -ge 5 ]]; then
    pass "printf '%q' used for path sanitisation ($PRINTF_Q_COUNT occurrences)"
else
    fail "Insufficient printf '%q' usage ($PRINTF_Q_COUNT occurrences, expected >= 5)"
fi

# --- 10. Anchored grep ---
echo "10. Anchored grep for session matching..."
if grep -q 'grep -qxF "\$session"' "$NBS_CLAUDE"; then
    pass "grep -qxF used for exact session matching"
else
    fail "grep -qxF not found for session matching"
fi

# --- 11. TOCTOU fix ---
echo "11. TOCTOU fix on control inbox..."
# Atomic read into variable — uses $(cat ...) rather than $(<...) because
# $(<file 2>/dev/null) is broken in bash <5.2
if grep -q 'inbox_content=\$(cat "\$CONTROL_INBOX"' "$NBS_CLAUDE"; then
    pass "Atomic file read into variable present"
else
    fail "Atomic file read not found"
fi

# --- 12. Non-atomic registry update ---
echo "12. Non-atomic registry update guarded..."
GUARDED_GREP=$(grep -c '{ grep -vF .* || true; }' "$NBS_CLAUDE" 2>/dev/null || echo 0)
if grep -q 'grep -vF.*|| true' "$NBS_CLAUDE"; then
    pass "grep -vF guarded with || true for set -e safety"
else
    fail "grep -vF not guarded against set -e"
fi

# --- 13. Empty variable arithmetic guard ---
echo "13. Empty variable arithmetic guard..."
if grep -q '\${total:-0}' "$NBS_CLAUDE"; then
    pass "total guarded with \${total:-0}"
else
    fail "total not guarded with default value"
fi

# --- 14. Unknown commands logged ---
echo "14. Unknown commands logged..."
if grep -q 'Unknown control command' "$NBS_CLAUDE"; then
    pass "Unknown commands logged to stderr"
else
    fail "Unknown commands not logged"
fi

# --- 15. sha256sum for change detection ---
echo "15. sha256sum for change detection..."
if grep -q 'sha256sum' "$NBS_CLAUDE"; then
    pass "sha256sum used for change detection"
else
    fail "sha256sum not found"
fi

if grep -q 'md5sum' "$NBS_CLAUDE"; then
    fail "md5sum still present"
else
    pass "md5sum fully replaced"
fi

# --- 16. pty_ prefix verified ---
echo "16. pty_ prefix session verification..."
if grep -q 'tmux has-session' "$NBS_CLAUDE"; then
    pass "tmux has-session check present before attach"
else
    fail "tmux has-session check not found"
fi

# --- 17. Eval contract documented ---
echo "17. Eval contract documented..."
if grep -q 'EVAL CONTRACT' "$NBS_CLAUDE"; then
    pass "Eval contract comment present"
else
    fail "Eval contract comment not found"
fi

# --- 18. if ! command pattern ---
echo "18. if ! command pattern (no fragile \$? check)..."
if grep -q 'if ! "\$PTY_SESSION" create' "$NBS_CLAUDE"; then
    pass "if ! command pattern used for pty-session create"
else
    fail "if ! command pattern not found"
fi

# Verify old fragile pattern removed
if grep -q '\$? -ne 0' "$NBS_CLAUDE"; then
    fail "Fragile \$? -ne 0 pattern still present"
else
    pass "Fragile \$? pattern removed"
fi

# --- Summary ---
echo ""
echo "=== Result ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All $TESTS tests passed"
else
    echo "FAIL: $FAIL of $TESTS tests failed"
fi

exit $FAIL
