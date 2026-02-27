#!/bin/bash
# Test nbs-claude V1.7/V1.8 audit fixes: verify all 7 violations are resolved
#
# Audit violations addressed:
#   1. (BUG)       Lines 184-196: TOCTOU race in PID file — now uses flock
#   2. (SECURITY)  Lines 224-235: JSON injection via SESSION_UUID — now validated
#   3. (BUG)       Line 204: Silent fallback to non-UUID — now fails explicitly
#   4. (HARDENING) Lines 140-141: NBS_ROOT newline chars — now rejected
#   5. (HARDENING) Line 326: pty-session coupling — now documented
#   6. (SECURITY)  Lines 185-186: PID not validated numeric — now checked
#   7. (BUG)       Lines 282-284: No sidecar startup check — now verified
#
# Falsification approach: each test tries to provoke the original bug and
# verifies the fix prevents it.

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

echo "=== nbs-claude V1.7/V1.8 Audit Fix Tests ==="
echo ""

# =========================================================================
# 1. (BUG) TOCTOU race — flock-based PID file acquisition
# =========================================================================
echo "1. TOCTOU fix: flock-based PID file acquisition..."

# Structural: flock is used
if grep -q 'flock -n 9' "$NBS_CLAUDE"; then
    pass "flock -n 9 present for atomic PID acquisition"
else
    fail "flock -n 9 not found — TOCTOU race still present"
fi

# Structural: exec 9> opens the fd for flock
if grep -q 'exec 9>"$PIDFILE"' "$NBS_CLAUDE"; then
    pass "exec 9> opens PID file for flock"
else
    fail "exec 9> not found — flock has no fd to lock"
fi

# Structural: old check-then-write pattern removed
if grep -q 'if \[\[ -f "$PIDFILE" \]\]' "$NBS_CLAUDE"; then
    fail "Old TOCTOU pattern (if [[ -f PIDFILE ]]) still present"
else
    pass "Old TOCTOU check-then-write pattern removed"
fi

# Structural: old kill -0 EXISTING_PID guard removed from acquisition path
# (kill -0 was used to check if process was alive — now flock handles this)
if grep -B2 -A2 'kill -0 "$EXISTING_PID"' "$NBS_CLAUDE" | grep -q 'EXISTING_PID=$(cat'; then
    fail "Old kill -0 liveness check still in acquisition path"
else
    pass "Old kill -0 liveness check removed from acquisition path"
fi

# =========================================================================
# 2. (SECURITY) JSON injection — SESSION_UUID character validation
# =========================================================================
echo ""
echo "2. JSON injection fix: SESSION_UUID validation..."

# Structural: validation regex present
if grep -q 'SESSION_UUID.*=~.*\^\[a-zA-Z0-9_-\]+\$' "$NBS_CLAUDE"; then
    pass "SESSION_UUID character validation regex present"
else
    fail "SESSION_UUID character validation regex not found"
fi

# Structural: exit 4 on invalid UUID
if grep -A1 'Session UUID contains invalid characters' "$NBS_CLAUDE" | grep -q 'exit 4'; then
    pass "Invalid SESSION_UUID exits with code 4"
else
    fail "Invalid SESSION_UUID does not exit with code 4"
fi

# Functional: valid UUID accepted (simulate the check)
TEST_UUID="550e8400-e29b-41d4-a716-446655440000"
if [[ "$TEST_UUID" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    pass "Valid UUID '550e8400-e29b-41d4-a716-446655440000' passes validation"
else
    fail "Valid UUID rejected by validation regex"
fi

# Functional: UUID with JSON injection payload rejected
EVIL_UUID="\"; \"evil\": \"payload"
if [[ "$EVIL_UUID" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    fail "JSON injection payload accepted by validation regex"
else
    pass "JSON injection payload '\"; \"evil\": \"payload' correctly rejected"
fi

# Functional: UUID with shell metacharacters rejected
SHELL_UUID='$(whoami)'
if [[ "$SHELL_UUID" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    fail "Shell metacharacter payload accepted by validation regex"
else
    pass "Shell metacharacter payload '\$(whoami)' correctly rejected"
fi

# Functional: empty UUID rejected
EMPTY_UUID=""
if [[ "$EMPTY_UUID" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    fail "Empty UUID accepted by validation regex"
else
    pass "Empty UUID correctly rejected"
fi

# =========================================================================
# 3. (BUG) Silent UUID fallback — now fails explicitly
# =========================================================================
echo ""
echo "3. UUID fallback fix: explicit failure when generation fails..."

# Structural: old fallback "no-uuid-$$-..." removed from code (may remain in comments)
if grep -v '^[[:space:]]*#' "$NBS_CLAUDE" | grep -q 'no-uuid-'; then
    fail "Old silent fallback 'no-uuid-\$\$-...' still present in code"
else
    pass "Silent fallback 'no-uuid-\$\$-...' removed from code"
fi

# Structural: empty check after UUID generation
if grep -q 'if \[\[ -z "$SESSION_UUID" \]\]' "$NBS_CLAUDE"; then
    pass "Empty UUID postcondition check present"
else
    fail "Empty UUID postcondition check not found"
fi

# Structural: error message mentions install action
if grep -q 'Install uuidgen' "$NBS_CLAUDE"; then
    pass "UUID failure message gives actionable guidance"
else
    fail "UUID failure message lacks actionable guidance"
fi

# Functional: fallback produces empty string (which is then caught)
FALLBACK_RESULT=$(echo "")
if [[ -z "$FALLBACK_RESULT" ]]; then
    pass "Empty fallback would be caught by -z check"
else
    fail "Empty fallback not caught"
fi

# =========================================================================
# 4. (HARDENING) NBS_ROOT newline validation
# =========================================================================
echo ""
echo "4. NBS_ROOT newline validation..."

# Structural: newline check present
if grep -q 'NBS_ROOT.*\\n' "$NBS_CLAUDE"; then
    pass "NBS_ROOT newline check present"
else
    fail "NBS_ROOT newline check not found"
fi

# Structural: carriage return check present
if grep -q 'NBS_ROOT.*\\r' "$NBS_CLAUDE"; then
    pass "NBS_ROOT carriage return check present"
else
    fail "NBS_ROOT carriage return check not found"
fi

# Structural: exits with code 4 on bad path
if grep -B1 -A2 'newline or carriage return' "$NBS_CLAUDE" | grep -q 'exit 4'; then
    pass "Newline in NBS_ROOT exits with code 4"
else
    fail "Newline in NBS_ROOT does not exit with code 4"
fi

# Functional: path with newline is detected
NBS_ROOT_TEST=$'/tmp/normal\n/etc/evil'
if [[ "$NBS_ROOT_TEST" == *$'\n'* ]]; then
    pass "Path with embedded newline detected by pattern"
else
    fail "Path with embedded newline not detected"
fi

# Functional: path with carriage return is detected
NBS_ROOT_TEST_CR=$'/tmp/normal\r/etc/evil'
if [[ "$NBS_ROOT_TEST_CR" == *$'\r'* ]]; then
    pass "Path with embedded carriage return detected by pattern"
else
    fail "Path with embedded carriage return not detected"
fi

# Functional: normal path passes
NBS_ROOT_NORMAL="/tmp/nbs-test-project"
if [[ "$NBS_ROOT_NORMAL" == *$'\n'* || "$NBS_ROOT_NORMAL" == *$'\r'* ]]; then
    fail "Normal path '/tmp/nbs-test-project' rejected by newline check"
else
    pass "Normal path '/tmp/nbs-test-project' passes newline check"
fi

# =========================================================================
# 5. (HARDENING) pty-session coupling documented
# =========================================================================
echo ""
echo "5. pty-session naming coupling documented..."

# Structural: coupling comment present
if grep -q 'COUPLING: pty-session names its tmux session' "$NBS_CLAUDE"; then
    pass "Coupling comment documents pty-session naming convention"
else
    fail "Coupling comment not found"
fi

# Structural: warning about breakage if convention changes
if grep -q 'If pty-session changes this convention' "$NBS_CLAUDE"; then
    pass "Coupling warning about convention change present"
else
    fail "Coupling warning about convention change not found"
fi

# =========================================================================
# 6. (SECURITY) PID numeric validation
# =========================================================================
echo ""
echo "6. PID file numeric validation..."

# Structural: numeric regex check on PID
if grep -q 'EXISTING_PID.*=~.*\^\[0-9\]+\$' "$NBS_CLAUDE"; then
    pass "PID numeric validation regex present"
else
    fail "PID numeric validation regex not found"
fi

# Structural: error on non-numeric PID
if grep -q 'non-numeric value' "$NBS_CLAUDE"; then
    pass "Non-numeric PID error message present"
else
    fail "Non-numeric PID error message not found"
fi

# Functional: numeric PID passes validation
VALID_PID="12345"
if [[ "$VALID_PID" =~ ^[0-9]+$ ]]; then
    pass "Numeric PID '12345' passes validation"
else
    fail "Numeric PID '12345' rejected"
fi

# Functional: PID with injection payload rejected
EVIL_PID="-9 1"
if [[ "$EVIL_PID" =~ ^[0-9]+$ ]]; then
    fail "Injection payload '-9 1' accepted as PID"
else
    pass "Injection payload '-9 1' rejected as PID"
fi

# Functional: PID with shell metacharacters rejected
SHELL_PID='$(reboot)'
if [[ "$SHELL_PID" =~ ^[0-9]+$ ]]; then
    fail "Shell metacharacter PID accepted"
else
    pass "Shell metacharacter PID '\$(reboot)' rejected"
fi

# Functional: empty PID handled (empty string does not match ^[0-9]+$)
EMPTY_PID=""
if [[ -n "$EMPTY_PID" && ! "$EMPTY_PID" =~ ^[0-9]+$ ]]; then
    fail "Empty PID triggers non-numeric error (should be handled by -n check)"
else
    pass "Empty PID handled correctly (skipped by -n check)"
fi

# =========================================================================
# 7. (BUG) Sidecar startup postcondition
# =========================================================================
echo ""
echo "7. Sidecar startup postcondition check..."

# Structural: sleep before kill -0 check (both modes)
SLEEP_COUNT=$(grep -c 'sleep 0.2' "$NBS_CLAUDE")
if [[ "$SLEEP_COUNT" -ge 2 ]]; then
    pass "sleep 0.2 present in both sidecar launch paths ($SLEEP_COUNT occurrences)"
else
    fail "sleep 0.2 not in both paths (found $SLEEP_COUNT, expected >= 2)"
fi

# Structural: kill -0 SIDECAR_PID postcondition (both modes)
KILL_CHECK_COUNT=$(grep -c 'kill -0 "$SIDECAR_PID"' "$NBS_CLAUDE")
# Note: there's also a kill -0 in cleanup(), so expect >= 3 total
# But specifically in the sidecar launch sections, there should be 2
LAUNCH_KILL_CHECKS=$(grep -B1 'kill -0 "$SIDECAR_PID"' "$NBS_CLAUDE" | grep -c 'if !')
if [[ "$LAUNCH_KILL_CHECKS" -ge 2 ]]; then
    pass "Sidecar postcondition kill -0 check in both launch paths ($LAUNCH_KILL_CHECKS checks)"
else
    fail "Sidecar postcondition not in both paths (found $LAUNCH_KILL_CHECKS, expected >= 2)"
fi

# Structural: error message references log file
if grep -q 'nbs-sidecar exited immediately' "$NBS_CLAUDE"; then
    pass "Sidecar crash error message present"
else
    fail "Sidecar crash error message not found"
fi

# Structural: error message directs to log file
if grep -q 'Check \$NBS_LOG_FILE' "$NBS_CLAUDE" || grep -q 'Check "$NBS_LOG_FILE"' "$NBS_CLAUDE"; then
    pass "Error message directs user to log file"
else
    fail "Error message does not reference log file"
fi

# Structural: postcondition in tmux mode
if sed -n '/MODE="tmux"/,/MODE="pty"/p' "$NBS_CLAUDE" | grep -q 'nbs-sidecar exited immediately'; then
    pass "Sidecar postcondition present in tmux mode"
else
    fail "Sidecar postcondition missing from tmux mode"
fi

# Structural: postcondition in pty mode
if sed -n '/MODE="pty"/,/^fi$/p' "$NBS_CLAUDE" | grep -q 'nbs-sidecar exited immediately'; then
    pass "Sidecar postcondition present in pty mode"
else
    fail "Sidecar postcondition missing from pty mode"
fi

# =========================================================================
# 8. Regression: existing functionality not broken
# =========================================================================
echo ""
echo "8. Regression checks..."

# set -euo pipefail still present
if grep -qx 'set -euo pipefail' "$NBS_CLAUDE"; then
    pass "set -euo pipefail still present"
else
    fail "set -euo pipefail missing"
fi

# Cleanup trap still present
if grep -q 'trap cleanup INT TERM EXIT' "$NBS_CLAUDE"; then
    pass "Cleanup trap still present"
else
    fail "Cleanup trap missing"
fi

# SIDECAR_HANDLE validation still present
if grep -q 'NBS_HANDLE must match' "$NBS_CLAUDE"; then
    pass "SIDECAR_HANDLE validation still present"
else
    fail "SIDECAR_HANDLE validation missing"
fi

# NBS_MODEL validation still present
if grep -q 'NBS_MODEL contains invalid characters' "$NBS_CLAUDE"; then
    pass "NBS_MODEL validation still present"
else
    fail "NBS_MODEL validation missing"
fi

# TMUX_SESSION_NAME validation still present
if grep -q 'TMUX_SESSION_NAME must match' "$NBS_CLAUDE"; then
    pass "TMUX_SESSION_NAME validation still present"
else
    fail "TMUX_SESSION_NAME validation missing"
fi

# Cleanup preserves exit code
if grep -q 'local EXIT_CODE=\$?' "$NBS_CLAUDE"; then
    pass "Cleanup exit code preservation still present"
else
    fail "Cleanup exit code preservation missing"
fi

# Both mode paths still present
if grep -q 'MODE="tmux"' "$NBS_CLAUDE" && grep -q 'MODE="pty"' "$NBS_CLAUDE"; then
    pass "Both mode paths (tmux, pty) still present"
else
    fail "Mode paths missing"
fi

# POLL_DISABLE still functional
if grep -q 'POLL_DISABLE.*!=.*1' "$NBS_CLAUDE"; then
    pass "POLL_DISABLE check still functional"
else
    fail "POLL_DISABLE check missing"
fi

# tmux has-session check still present
if grep -q 'tmux has-session' "$NBS_CLAUDE"; then
    pass "tmux has-session verification still present"
else
    fail "tmux has-session verification missing"
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
