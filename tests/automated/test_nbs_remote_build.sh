#!/bin/bash
# Test: nbs-remote-build argument validation, bug fixes, and hardening
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
TEST_SESSION_2="test-build-2-$$"
cleanup() {
    "$PTY_SESSION" kill "$TEST_SESSION" 2>/dev/null || true
    "$PTY_SESSION" kill "$TEST_SESSION_2" 2>/dev/null || true
}
trap cleanup EXIT

echo "Test: nbs-remote-build"

# ============================================================
# Section 1: Original tests (argument validation + functional)
# ============================================================

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
    --prompt='\$' --timeout=15 --poll=1 --quiet 2>/dev/null) || true

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

# ============================================================
# Section 2: Fix #1 (HARDENING) — Improved pty-session assertion message
# ============================================================
echo ""
echo "Test 8: pty-session assertion message includes install URL"
# Temporarily set PTY_SESSION to a nonexistent path via env override
# We test this by examining the source rather than actually breaking the binary
err_msg=$("$REMOTE_BUILD" --help 2>&1 | head -1) || true
# The real test: if pty-session is missing, does the message contain actionable info?
# We can't uninstall pty-session, so we verify the source contains the right pattern
if grep -q "Install from https://github.com/SonicField/nbs-framework" "$REMOTE_BUILD"; then
    check "pty-session assertion includes install URL" "0"
else
    check "pty-session assertion includes install URL" "1"
fi

if grep -q "ASSERTION FAILED.*pty-session.*not found or not executable" "$REMOTE_BUILD"; then
    check "pty-session assertion explains what/why" "0"
else
    check "pty-session assertion explains what/why" "1"
fi

# ============================================================
# Section 3: Fix #5 (BUG) — Zero rejected for TIMEOUT and POLL_INTERVAL
# ============================================================
echo ""
echo "Test 9: zero timeout rejected"
rc=0
"$REMOTE_BUILD" somesession 'echo hello' --timeout=0 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "timeout=0 rejected with exit 4" "0"
else
    check "timeout=0 rejected with exit 4 (got $rc)" "1"
fi

echo ""
echo "Test 10: zero poll interval rejected"
rc=0
"$REMOTE_BUILD" somesession 'echo hello' --poll=0 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "poll=0 rejected with exit 4" "0"
else
    check "poll=0 rejected with exit 4 (got $rc)" "1"
fi

# Also test that error message mentions > 0
err_output=$("$REMOTE_BUILD" somesession 'echo hello' --timeout=0 2>&1) || true
if echo "$err_output" | grep -q "> 0"; then
    check "timeout=0 error message mentions > 0" "0"
else
    check "timeout=0 error message mentions > 0" "1"
fi

err_output=$("$REMOTE_BUILD" somesession 'echo hello' --poll=0 2>&1) || true
if echo "$err_output" | grep -q "> 0"; then
    check "poll=0 error message mentions > 0" "0"
else
    check "poll=0 error message mentions > 0" "1"
fi

# Negative and non-numeric still rejected
rc=0
"$REMOTE_BUILD" somesession 'echo hello' --timeout=-5 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "negative timeout rejected" "0"
else
    check "negative timeout rejected (got $rc)" "1"
fi

rc=0
"$REMOTE_BUILD" somesession 'echo hello' --poll=abc 2>/dev/null || rc=$?
if [[ "$rc" -eq 4 ]]; then
    check "non-numeric poll rejected" "0"
else
    check "non-numeric poll rejected (got $rc)" "1"
fi

# ============================================================
# Section 4: Fix #2 (BUG) — nbs-chat precondition check
# ============================================================
echo ""
echo "Test 11: nbs-chat precondition in source"
# Verify the source code checks for nbs-chat executability when --chat is set
if grep -q 'ASSERTION FAILED.*nbs-chat.*not found.*required when --chat' "$REMOTE_BUILD"; then
    check "nbs-chat assertion present for --chat" "0"
else
    check "nbs-chat assertion present for --chat" "1"
fi

# Verify the check only fires when CHAT_FILE is non-empty (not unconditionally)
if grep -B3 'nbs-chat.*not found' "$REMOTE_BUILD" | grep -q 'CHAT_FILE'; then
    check "nbs-chat check is conditional on --chat" "0"
else
    check "nbs-chat check is conditional on --chat" "1"
fi

# ============================================================
# Section 5: Fix #3 (BUG) — Chat read error not silently swallowed
# ============================================================
echo ""
echo "Test 12: chat read failure warning in source"
# Verify the source reports chat read failures rather than using || true
if grep -q 'nbs-chat read failed.*chat monitoring may be impaired' "$REMOTE_BUILD"; then
    check "chat read failure produces warning" "0"
else
    check "chat read failure produces warning" "1"
fi

# Verify || true is no longer used for chat read
if grep 'NBS_CHAT.*read.*|| true' "$REMOTE_BUILD" | grep -qv '#'; then
    check "chat read no longer uses || true" "1"
else
    check "chat read no longer uses || true" "0"
fi

# ============================================================
# Section 6: Fix #4 (HARDENING) — Consecutive read failure tracking
# ============================================================
echo ""
echo "Test 13: consecutive read failure tracking in source"
if grep -q 'consecutive_read_failures' "$REMOTE_BUILD"; then
    check "consecutive_read_failures variable exists" "0"
else
    check "consecutive_read_failures variable exists" "1"
fi

if grep -q 'consecutive_read_failures >= 3' "$REMOTE_BUILD"; then
    check "abort after 3 consecutive read failures" "0"
else
    check "abort after 3 consecutive read failures" "1"
fi

if grep -q 'session may be dead' "$REMOTE_BUILD"; then
    check "dead session error message present" "0"
else
    check "dead session error message present" "1"
fi

# Also verify the counter resets on success
if grep -q 'consecutive_read_failures=0' "$REMOTE_BUILD"; then
    check "read failure counter resets on success" "0"
else
    check "read failure counter resets on success" "1"
fi

# ============================================================
# Section 7: Fix #6 (HARDENING) — Timeout path reports read failure
# ============================================================
echo ""
echo "Test 14: timeout path warns on read failure"
# Verify the timeout path checks the read exit code
if grep -A1 'Print whatever output we have' "$REMOTE_BUILD" | grep -q 'if !'; then
    check "timeout path checks pty-session read exit code" "0"
else
    check "timeout path checks pty-session read exit code" "1"
fi

if grep -q 'could not read final output' "$REMOTE_BUILD"; then
    check "timeout path has read failure warning" "0"
else
    check "timeout path has read failure warning" "1"
fi

# ============================================================
# Section 8: Fix #7 (SECURITY) — Sentinel-based prompt verification
# ============================================================
echo ""
echo "Test 15: sentinel verification in source"
if grep -q 'NBS_SENTINEL' "$REMOTE_BUILD"; then
    check "sentinel mechanism present" "0"
else
    check "sentinel mechanism present" "1"
fi

if grep -q 'RANDOM.*RANDOM' "$REMOTE_BUILD"; then
    check "sentinel uses random values" "0"
else
    check "sentinel uses random values" "1"
fi

# Verify that a false prompt match triggers continue (skip to next poll)
# The comment and the 'continue' statement are on separate lines, so check both exist
if grep -q 'false positive' "$REMOTE_BUILD" && grep -A1 'false positive' "$REMOTE_BUILD" | grep -q 'continue'; then
    check "false positive causes continue" "0"
else
    check "false positive causes continue" "1"
fi

# ============================================================
# Section 9: Fix #7 (SECURITY) — Integration test: build output
#   containing prompt-like text does not cause false completion
# ============================================================
echo ""
echo "Test 16: false-positive prompt detection (integration)"
# Kill old session, create fresh one
"$PTY_SESSION" kill "$TEST_SESSION" 2>/dev/null || true
sleep 1
"$PTY_SESSION" create "$TEST_SESSION" 'bash'
sleep 1

# The build command echoes text matching the prompt pattern, then sleeps.
# Without sentinel verification, the tool would falsely detect completion
# after seeing the "$ " in the echo output. With sentinel verification,
# the tool should wait until the actual shell prompt returns.
# We use a short timeout — if the tool times out, the sentinel is working
# (it didn't get fooled by the echo output).
rc=0
output=$("$REMOTE_BUILD" "$TEST_SESSION" \
    'echo "fake prompt $ "; sleep 3; echo REAL_DONE' \
    --prompt='\$' --timeout=15 --poll=1 --quiet 2>/dev/null) || rc=$?

# The build should complete (exit 0) and the output should contain REAL_DONE
if [[ "$rc" -eq 0 ]]; then
    check "sentinel: build completes normally (exit 0)" "0"
else
    check "sentinel: build completes normally (exit 0, got $rc)" "1"
fi

if echo "$output" | grep -q "REAL_DONE"; then
    check "sentinel: output contains REAL_DONE" "0"
else
    check "sentinel: output contains REAL_DONE" "1"
fi

# ============================================================
# Section 10: Fix #8 (HARDENING) — Postcondition on build output
# ============================================================
echo ""
echo "Test 17: postcondition on build output in source"
if grep -q 'build completed but could not read output' "$REMOTE_BUILD"; then
    check "postcondition warning on failed output read" "0"
else
    check "postcondition warning on failed output read" "1"
fi

# ============================================================
# Section 11: Fix #9 (HARDENING) — Wall-clock elapsed time
# ============================================================
echo ""
echo "Test 18: wall-clock elapsed time"
if grep -q 'start_time=$(date +%s)' "$REMOTE_BUILD"; then
    check "uses date +%s for start time" "0"
else
    check "uses date +%s for start time" "1"
fi

if grep -q 'now=$(date +%s)' "$REMOTE_BUILD"; then
    check "uses date +%s for current time in loop" "0"
else
    check "uses date +%s for current time in loop" "1"
fi

if grep -q 'elapsed=$((now - start_time))' "$REMOTE_BUILD"; then
    check "elapsed is now minus start_time" "0"
else
    check "elapsed is now minus start_time" "1"
fi

# Verify the old pattern (elapsed += POLL_INTERVAL) is gone
if grep -q 'elapsed=$((elapsed + POLL_INTERVAL))' "$REMOTE_BUILD"; then
    check "old elapsed accumulation pattern removed" "1"
else
    check "old elapsed accumulation pattern removed" "0"
fi

# ============================================================
# Section 12: Integration test — wall-clock timing accuracy
# ============================================================
echo ""
echo "Test 19: wall-clock timing accuracy (integration)"
"$PTY_SESSION" kill "$TEST_SESSION" 2>/dev/null || true
sleep 1
"$PTY_SESSION" create "$TEST_SESSION" 'bash'
sleep 1

# Timeout of 4s with poll of 1s. The build command sleeps for 10s.
# Wall-clock should report ~4s, not accumulate differently.
before=$(date +%s)
rc=0
"$REMOTE_BUILD" "$TEST_SESSION" 'sleep 10' \
    --prompt='NEVER_MATCH_THIS_XYZ' --timeout=4 --poll=1 --quiet 2>/dev/null || rc=$?
after=$(date +%s)
wall_elapsed=$((after - before))

if [[ "$rc" -eq 3 ]]; then
    check "wall-clock: times out with exit 3" "0"
else
    check "wall-clock: times out with exit 3 (got $rc)" "1"
fi

# Wall-clock time should be between 4 and 8 seconds (allowing overhead)
if (( wall_elapsed >= 3 && wall_elapsed <= 10 )); then
    check "wall-clock: elapsed ${wall_elapsed}s is in [3,10]" "0"
else
    check "wall-clock: elapsed ${wall_elapsed}s is in [3,10]" "1"
fi

# ============================================================
# Summary
# ============================================================

echo ""
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
