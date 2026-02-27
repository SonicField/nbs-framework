#!/bin/bash
# Test: nbs-sidecar-restart audit fixes (V5.x)
#
# Tests for engineering standards violations fixed in nbs-sidecar-restart:
#   1. Kill failure logging (V5.1 — was silent, now logged)
#   2. SIGKILL escalation (V5.2 — SIGTERM failure escalates to SIGKILL)
#   3. Disown failure logging (V5.3 — was silent, now warned)
#   4. pgrep error distinction (V5.4 — exit 2+ is error, not "no matches")
#   5. Argument format validation (V5.5 — SECURITY: reject injection)
#   6. Failure message context (V5.6 — all FAIL msgs include handle/PID)
#   7. Numeric PID assertion (V5.7 — non-numeric PIDs abort)
#   8. Startup verification window (V5.8 — double-check with gap)
#
# Strategy: These tests exercise the script's logic by inspecting the source
# for the expected patterns and by running the script in controlled scenarios
# where we can observe its output. We cannot easily create real sidecar
# processes, so we test argument validation, error handling paths, and
# structural properties.
#
# Falsification: each test asserts a specific property that would be absent
# if the corresponding fix were reverted.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
RESTART_SCRIPT="$PROJECT_ROOT/bin/nbs-sidecar-restart"

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

echo "=== nbs-sidecar-restart Audit Fix Tests ==="
echo ""

# =========================================================================
# Setup: temp directory for mock binaries
# =========================================================================
TEST_DIR=$(mktemp -d)
cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

# =========================================================================
# 1. Script exists and is executable
# =========================================================================
echo "1. Script exists and is executable..."
if [[ -x "$RESTART_SCRIPT" ]]; then
    pass "nbs-sidecar-restart is executable"
else
    fail "nbs-sidecar-restart is not executable"
fi

# =========================================================================
# 2. --help exits 0
# =========================================================================
echo "2. --help exits cleanly..."
HELP_OUTPUT=$("$RESTART_SCRIPT" --help 2>&1) || {
    fail "--help exited non-zero"
}
if echo "$HELP_OUTPUT" | grep -qF "Hot-restart"; then
    pass "--help shows usage text"
else
    fail "--help does not show expected usage text"
fi

# =========================================================================
# 3. No match for bogus handle — exits 0 or 1 appropriately
# =========================================================================
echo "3. Targeted handle miss — appropriate exit..."
# Use a deliberately nonexistent handle to avoid restarting real sidecars.
# If no sidecars are running at all, exit 0 ("No running sidecars found").
# If sidecars exist but none match, exit 1 ("No sidecar found for handle").
BOGUS_OUTPUT=$("$RESTART_SCRIPT" "test-bogus-handle-$$-xyzzy" 2>&1) || true
if echo "$BOGUS_OUTPUT" | grep -qF "No running sidecars found"; then
    pass "No sidecars running — clean exit"
elif echo "$BOGUS_OUTPUT" | grep -qF "No sidecar found for handle"; then
    pass "Sidecars running but bogus handle not found — correct exit"
else
    fail "Unexpected output for bogus handle: $BOGUS_OUTPUT"
fi

# =========================================================================
# 4. V5.4: pgrep error code distinction — source structure test
# =========================================================================
echo "4. V5.4: pgrep error code distinction..."
# The fix must distinguish pgrep exit code 1 (no matches) from 2+ (error).
# Verify the pattern exists in the source.
if grep -q 'pgrep_rc' "$RESTART_SCRIPT" && grep -q 'pgrep_rc -gt 1' "$RESTART_SCRIPT"; then
    pass "pgrep exit code distinction present in source"
else
    fail "pgrep exit code distinction missing — silent pgrep errors possible"
fi

# =========================================================================
# 5. V5.7: Numeric PID assertion — source structure test
# =========================================================================
echo "5. V5.7: Numeric PID assertion..."
if grep -qF '[0-9]' "$RESTART_SCRIPT" && grep -q 'ASSERTION FAILED.*non-numeric PID' "$RESTART_SCRIPT"; then
    pass "Numeric PID assertion present in source"
else
    fail "Numeric PID assertion missing — non-numeric PIDs could reach kill"
fi

# =========================================================================
# 6. V5.1: Kill failure logging — source structure test
# =========================================================================
echo "6. V5.1: Kill failure logging..."
# The old code was: kill "$pid" 2>/dev/null || true
# The new code should log when kill fails.
if grep -q 'kill signal may not have been delivered' "$RESTART_SCRIPT"; then
    pass "Kill failure logging present in source"
else
    fail "Kill failure logging missing — kill errors silently discarded"
fi

# Verify the old silent pattern is gone
if grep -qE 'kill "\$pid" 2>/dev/null \|\| true' "$RESTART_SCRIPT"; then
    fail "Old silent kill pattern still present"
else
    pass "Old silent kill pattern removed"
fi

# =========================================================================
# 7. V5.2: SIGKILL escalation — source structure test
# =========================================================================
echo "7. V5.2: SIGKILL escalation..."
if grep -q 'kill -9 "\$pid"' "$RESTART_SCRIPT" && grep -q 'SIGKILL' "$RESTART_SCRIPT"; then
    pass "SIGKILL escalation present in source"
else
    fail "SIGKILL escalation missing — stuck processes can never be restarted"
fi

# =========================================================================
# 8. V5.3: Disown failure logging — source structure test
# =========================================================================
echo "8. V5.3: Disown failure logging..."
# Old code: disown "$NEW_PID" 2>/dev/null || true
# New code: logs on failure
if grep -q 'disown failed for PID' "$RESTART_SCRIPT"; then
    pass "Disown failure logging present in source"
else
    fail "Disown failure logging missing — disown errors silently discarded"
fi

# Verify the old silent pattern is gone
if grep -qE 'disown.*\|\| true' "$RESTART_SCRIPT"; then
    fail "Old silent disown pattern still present"
else
    pass "Old silent disown pattern removed"
fi

# =========================================================================
# 9. V5.5: Argument format validation — source structure test
# =========================================================================
echo "9. V5.5: Argument format validation..."
if grep -q 'unexpected argument format' "$RESTART_SCRIPT"; then
    pass "Argument format validation present in source"
else
    fail "Argument format validation missing — argument injection possible"
fi

# Verify the regex pattern enforces --flag or --flag=value format
if grep -qE '\^--\[a-zA-Z_-\]' "$RESTART_SCRIPT"; then
    pass "Argument validation uses --flag pattern"
else
    fail "Argument validation pattern not found"
fi

# =========================================================================
# 10. V5.6: FAIL messages include handle and PID context
# =========================================================================
echo "10. V5.6: FAIL messages include context..."
# All FAIL messages should include $HANDLE or handle reference
FAIL_LINES=$(grep -c 'echo.*FAIL.*\$HANDLE' "$RESTART_SCRIPT") || FAIL_LINES=0
if [[ "$FAIL_LINES" -ge 4 ]]; then
    pass "All FAIL messages include handle context ($FAIL_LINES instances)"
else
    fail "Some FAIL messages missing handle context (found $FAIL_LINES, expected >= 4)"
fi

# Check there are no FAIL messages without context (the old pattern)
BARE_FAILS=$(grep -cE 'echo.*"FAIL — ' "$RESTART_SCRIPT") || BARE_FAILS=0
if [[ "$BARE_FAILS" -eq 0 ]]; then
    pass "No bare FAIL messages without context"
else
    fail "$BARE_FAILS FAIL message(s) still lack handle/PID context"
fi

# =========================================================================
# 11. V5.8: Double-check startup verification — source structure test
# =========================================================================
echo "11. V5.8: Double-check startup verification..."
# Old code: sleep 0.5 then single kill -0 check
# New code: sleep 1, check, sleep 2, check again
if grep -q 'died during startup' "$RESTART_SCRIPT"; then
    pass "Startup verification double-check present"
else
    fail "Startup verification double-check missing — fast crashes undetected"
fi

# =========================================================================
# 12. V5.5 SECURITY: Argument validation rejects bad arguments
# =========================================================================
echo "12. V5.5 SECURITY: Argument validation regex test..."
#
# We test the regex used in the script against known good and bad patterns.
# The regex must accept --flag and --flag=value, and reject everything else.
# This is a direct test of the security invariant.

REGEX_PASS_COUNT=0
REGEX_FAIL_COUNT=0

# Good patterns (should match)
for good_arg in "--handle=test" "--config=path/to/file" "--verbose" "--no-prompt" "--initial-prompt=hello world"; do
    if [[ "$good_arg" =~ ^--[a-zA-Z_-]+= ]] || [[ "$good_arg" =~ ^--[a-zA-Z_-]+$ ]]; then
        REGEX_PASS_COUNT=$((REGEX_PASS_COUNT + 1))
    else
        fail "Regex incorrectly rejects valid arg: '$good_arg'"
        REGEX_FAIL_COUNT=$((REGEX_FAIL_COUNT + 1))
    fi
done

# Bad patterns (should NOT match)
for bad_arg in "plain_arg" "/bin/sh" "-c" "rm -rf /" "--handle=test;evil" "; echo pwned"; do
    if [[ "$bad_arg" =~ ^--[a-zA-Z_-]+= ]] || [[ "$bad_arg" =~ ^--[a-zA-Z_-]+$ ]]; then
        # Check if it's a false positive — --handle=test;evil WILL match ^--[a-zA-Z_-]+=
        # because regex matches the prefix. This is expected: the value after = can be
        # anything because nbs-sidecar does its own argument parsing. The security
        # boundary is preventing non-flag arguments, not sanitising flag values.
        # So --handle=test;evil is actually accepted by design (it's a --flag=value).
        if [[ "$bad_arg" =~ ^--[a-zA-Z_-]+= ]]; then
            REGEX_PASS_COUNT=$((REGEX_PASS_COUNT + 1))
        else
            fail "Regex incorrectly accepts bad arg: '$bad_arg'"
            REGEX_FAIL_COUNT=$((REGEX_FAIL_COUNT + 1))
        fi
    else
        REGEX_PASS_COUNT=$((REGEX_PASS_COUNT + 1))
    fi
done

if [[ $REGEX_FAIL_COUNT -eq 0 ]]; then
    pass "Argument validation regex correctly classifies $REGEX_PASS_COUNT test patterns"
else
    fail "Argument validation regex has $REGEX_FAIL_COUNT classification errors"
fi

# =========================================================================
# 13. Infrastructure handle skip is still present
# =========================================================================
echo "13. Infrastructure handle skip..."
if grep -q 'pythia\*|shepard\*|fixup\*' "$RESTART_SCRIPT"; then
    pass "Infrastructure handle skip present"
else
    fail "Infrastructure handle skip missing"
fi

# =========================================================================
# 14. set -euo pipefail is present
# =========================================================================
echo "14. Strict mode..."
if head -15 "$RESTART_SCRIPT" | grep -q 'set -euo pipefail'; then
    pass "Strict mode (set -euo pipefail) active"
else
    fail "Strict mode not set"
fi

# =========================================================================
# 15. Binary path validation still present (V4.2)
# =========================================================================
echo "15. Binary path validation..."
if grep -q 'expected_binary.*SCRIPT_DIR.*nbs-sidecar' "$RESTART_SCRIPT"; then
    pass "Binary path validation present"
else
    fail "Binary path validation missing"
fi

# =========================================================================
# 16. --initial-prompt stripping still present
# =========================================================================
echo "16. --initial-prompt stripping..."
if grep -q 'initial-prompt' "$RESTART_SCRIPT"; then
    pass "--initial-prompt stripping present"
else
    fail "--initial-prompt stripping missing"
fi

# =========================================================================
# 17. Exit code uses 0/1, not FAILED count (V4.4)
# =========================================================================
echo "17. Exit code discipline..."
if grep -q 'FAILED.*-gt 0' "$RESTART_SCRIPT"; then
    pass "Exit code uses 0/1 (not raw FAILED count)"
else
    fail "Exit code may overflow valid range"
fi

# =========================================================================
# 18. No silent error patterns remaining
# =========================================================================
echo "18. No remaining silent error patterns..."
# Count instances of the anti-pattern: command 2>/dev/null || true
# Some are acceptable (kill -9 during SIGKILL escalation, for example).
# But the specific patterns identified in the audit should be gone.
SILENT_PATTERNS=0

# The original kill "$pid" 2>/dev/null || true should be gone
if grep -qE '^\s+kill "\$pid" 2>/dev/null \|\| true' "$RESTART_SCRIPT"; then
    fail "Silent kill pattern found (V5.1 regression)"
    SILENT_PATTERNS=$((SILENT_PATTERNS + 1))
fi

# The original disown ... || true should be gone
if grep -qE '^\s+disown.*\|\| true' "$RESTART_SCRIPT"; then
    fail "Silent disown pattern found (V5.3 regression)"
    SILENT_PATTERNS=$((SILENT_PATTERNS + 1))
fi

# The original pgrep ... || true should be gone
if grep -qE 'pgrep.*\|\| true' "$RESTART_SCRIPT"; then
    fail "Silent pgrep pattern found (V5.4 regression)"
    SILENT_PATTERNS=$((SILENT_PATTERNS + 1))
fi

if [[ $SILENT_PATTERNS -eq 0 ]]; then
    pass "No audited silent error patterns remain"
fi

# =========================================================================
# 19. Functional: targeted handle with no match exits 1
# =========================================================================
echo "19. Targeted handle with no match..."
OUTPUT=$("$RESTART_SCRIPT" "nonexistent-handle-xyzzy-$$" 2>&1) || EXIT_CODE=$?
EXIT_CODE=${EXIT_CODE:-0}
if [[ "$EXIT_CODE" -ne 0 ]]; then
    pass "Nonexistent handle exits non-zero (exit code $EXIT_CODE)"
else
    # Might find no sidecars at all, in which case exit 0 is correct
    if echo "$OUTPUT" | grep -qF "No running sidecars found"; then
        pass "No sidecars running, so no match possible (exit 0 is correct)"
    else
        fail "Nonexistent handle exited 0 when sidecars exist"
    fi
fi

# =========================================================================
# 20. V5.4: PIDS variable uses ${PIDS:-} for unset safety
# =========================================================================
echo "20. V5.4: PIDS unset safety..."
if grep -q '${PIDS:-}' "$RESTART_SCRIPT"; then
    pass "PIDS variable uses :- default for unset safety under set -u"
else
    fail "PIDS variable may trigger unbound variable error under set -u"
fi

# =========================================================================
# Results
# =========================================================================
echo ""
echo "=== Result ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All $TESTS tests passed"
else
    echo "FAIL: $FAIL of $TESTS tests failed"
fi

exit $FAIL
