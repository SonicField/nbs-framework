#!/bin/bash
# Test: nbs-scribe-log audit violation tests
#
# Adversarial tests for BUG/SECURITY/HARDENING violations identified
# in .nbs/audit-report.md for main.c, scribe_log.c, and scribe_log.h.
#
# Categories:
#   A (1-4):   Truncation detection (BUG — silent truncation)
#   B (5-6):   Path length validation (SECURITY)
#   C (7-8):   Status help text / default status (BUG)
#   D (9-11):  Newline injection prevention (SECURITY)
#   E (12-13): TOCTOU race mitigation (BUG)
#   F (14-16): Postcondition / hardening checks
#
# Exit codes:
#   0 - All tests passed
#   1 - One or more tests failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_SCRIBE="${NBS_SCRIBE_BIN:-$PROJECT_ROOT/bin/nbs-scribe-log}"

export PATH="$PROJECT_ROOT/bin:$PATH"

TEST_DIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $label"
        FAIL=$((FAIL + 1))
    fi
}

TEST_NUM=0
next_test() {
    TEST_NUM=$((TEST_NUM + 1))
    echo ""
    echo "A$TEST_NUM. $1"
}

echo "=== nbs-scribe-log Audit Violation Tests ==="
echo "Test dir: $TEST_DIR"
echo "Binary:   $NBS_SCRIBE"

if [[ ! -x "$NBS_SCRIBE" ]]; then
    echo "FATAL: $NBS_SCRIBE not found or not executable"
    exit 1
fi

# =====================================================================
# A: Truncation detection (BUG — main.c violation 3)
# =====================================================================

next_test "Summary exceeding SCRIBE_MAX_SUMMARY (1024) is rejected"
LOG="$TEST_DIR/trunc1.md"
# Generate a summary longer than 1024 characters
LONG_SUMMARY=$(python3 -c "print('X' * 1025)")
set +e
"$NBS_SCRIBE" "$LOG" "$LONG_SUMMARY" \
    --participants=alex --rationale="Truncation test" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Overlong summary rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Rationale exceeding SCRIBE_MAX_FIELD (2048) is rejected"
LOG="$TEST_DIR/trunc2.md"
LONG_RATIONALE=$(python3 -c "print('R' * 2049)")
set +e
"$NBS_SCRIBE" "$LOG" "Normal summary" \
    --participants=alex --rationale="$LONG_RATIONALE" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Overlong rationale rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Participants exceeding SCRIBE_MAX_FIELD (2048) is rejected"
LOG="$TEST_DIR/trunc3.md"
LONG_PARTICIPANTS=$(python3 -c "print('P' * 2049)")
set +e
"$NBS_SCRIBE" "$LOG" "Normal summary" \
    --participants="$LONG_PARTICIPANTS" --rationale="Short" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Overlong participants rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Exactly-at-limit summary (1023 chars) is accepted"
LOG="$TEST_DIR/trunc4.md"
EXACT_SUMMARY=$(python3 -c "print('Y' * 1023)")
set +e
"$NBS_SCRIBE" "$LOG" "$EXACT_SUMMARY" \
    --participants=alex --rationale="Boundary test" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Exactly-at-limit summary accepted (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"

# =====================================================================
# B: Path length validation (SECURITY — main.c violation 5)
# =====================================================================

next_test "Log path exceeding SCRIBE_MAX_PATH (4096) is rejected"
# Build a path longer than 4096 characters
LONG_PATH="$TEST_DIR/$(python3 -c "print('a' * 4090)")/log.md"
set +e
"$NBS_SCRIBE" "$LONG_PATH" "Path too long" \
    --participants=alex --rationale="Path test" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
# Should be rejected with exit code 4 (bad args) or abort (exit code != 0)
check "Overlong path rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Empty log path is rejected"
set +e
"$NBS_SCRIBE" "" "Empty path test" \
    --participants=alex --rationale="Empty path" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Empty path rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# C: Status help text and default (BUG — main.c violations 7, 8)
# =====================================================================

next_test "--status=mitigated is accepted and documented"
LOG="$TEST_DIR/status1.md"
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "Mitigated status test" \
    --participants=alex --rationale="Testing mitigated" \
    --status=mitigated \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
RC=$?
set -e
check "mitigated status accepted (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Status line shows 'mitigated'" \
    "$( grep -qF -- '- **Status:** mitigated' "$LOG" && echo pass || echo fail )"

next_test "--help output lists 'mitigated' as valid status"
set +e
HELP_OUT=$("$NBS_SCRIBE" --help 2>&1)
set -e
check "--help mentions mitigated" \
    "$( echo "$HELP_OUT" | grep -qF 'mitigated' && echo pass || echo fail )"

# =====================================================================
# D: Newline injection prevention (SECURITY — scribe_log.c violation 10)
# =====================================================================

next_test "Summary containing newline is rejected"
LOG="$TEST_DIR/inject1.md"
# Use printf to embed an actual newline in the summary
INJECTED_SUMMARY=$'Legit summary\n### D-9999999999 Injected fake entry'
set +e
"$NBS_SCRIBE" "$LOG" "$INJECTED_SUMMARY" \
    --participants=alex --rationale="Injection test" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
# Should be rejected: either exit 4 (bad args) or abort (signal)
check "Newline in summary rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Rationale containing newline is rejected"
LOG="$TEST_DIR/inject2.md"
INJECTED_RATIONALE=$'Good rationale\n### D-8888888888 Injected via rationale'
set +e
"$NBS_SCRIBE" "$LOG" "Normal summary" \
    --participants=alex --rationale="$INJECTED_RATIONALE" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Newline in rationale rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Participants containing newline is rejected"
LOG="$TEST_DIR/inject3.md"
INJECTED_PARTICIPANTS=$'alex\n### D-7777777777 Injected via participants'
set +e
"$NBS_SCRIBE" "$LOG" "Normal summary" \
    --participants="$INJECTED_PARTICIPANTS" --rationale="Clean" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Newline in participants rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# E: TOCTOU race mitigation (BUG — scribe_log.c violations 4, 5)
# =====================================================================

next_test "Concurrent init+append does not corrupt (race window closed)"
LOG="$TEST_DIR/race1.md"
# Launch 5 concurrent appenders to stress the init/append race
for i in $(seq 1 5); do
    "$NBS_SCRIBE" "$LOG" "Race-entry-$i" \
        --participants="racer-$i" --rationale="Stress test $i" \
        --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1 &
done
wait
# Count decision headings — should be exactly 5
HEADING_COUNT=$(grep -c '^### D-' "$LOG" 2>/dev/null || echo 0)
check "All 5 concurrent entries present" \
    "$( [[ "$HEADING_COUNT" -eq 5 ]] && echo pass || echo fail )"
# The header should appear exactly once (no double init)
HEADER_COUNT=$(grep -c '^# Decision Log' "$LOG" 2>/dev/null || echo 0)
check "Header appears exactly once (no double init)" \
    "$( [[ "$HEADER_COUNT" -eq 1 ]] && echo pass || echo fail )"

next_test "Concurrent init on new file does not truncate"
# More aggressive: 10 concurrent appenders on a fresh file
LOG="$TEST_DIR/race2.md"
for i in $(seq 1 10); do
    "$NBS_SCRIBE" "$LOG" "Heavy-race-$i" \
        --participants="racer" --rationale="Heavy stress $i" \
        --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1 &
done
wait
HEADING_COUNT=$(grep -c '^### D-' "$LOG" 2>/dev/null || echo 0)
check "All 10 concurrent entries present" \
    "$( [[ "$HEADING_COUNT" -eq 10 ]] && echo pass || echo fail )"

# =====================================================================
# F: Postcondition and hardening checks
# =====================================================================

next_test "Log file created with explicit permissions (not world-writable)"
LOG="$TEST_DIR/perms1.md"
"$NBS_SCRIBE" "$LOG" "Permissions test" \
    --participants=alex --rationale="Check perms" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
# Check that the log file is not world-writable
PERMS=$(stat -c '%a' "$LOG" 2>/dev/null || stat -f '%Lp' "$LOG" 2>/dev/null)
check "Log file not world-writable" \
    "$( [[ ! "$PERMS" =~ [2367]$ ]] && echo pass || echo fail )"

next_test "Return code is a documented exit code (0, 1, or 4)"
LOG="$TEST_DIR/rc1.md"
set +e
"$NBS_SCRIBE" "$LOG" "Return code test" \
    --participants=alex --rationale="RC check" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Return code is 0 (documented)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"

next_test "Invalid status rejected with documented exit code"
LOG="$TEST_DIR/rc2.md"
set +e
"$NBS_SCRIBE" "$LOG" "Bad status" \
    --participants=alex --rationale="Reason" \
    --status=invalid >/dev/null 2>&1
RC=$?
set -e
check "Invalid status gives exit code 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# Summary
# =====================================================================

echo ""
echo "=== Audit Test Result: $PASS passed, $FAIL failed ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All audit tests passed"
    exit 0
else
    echo "FAIL: $FAIL audit test(s) failed"
    exit 1
fi
