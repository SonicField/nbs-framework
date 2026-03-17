#!/bin/bash
# Test: nbs-scribe-log audit v2 — adversarial tests for B23, S10, S11, and hardening fixes
#
# Tests are adversarial: they attempt to falsify the fix claims.
#
# V1 (1-3):   B23 — snprintf return check on --status
# V2 (4-5):   S10 — fopen("a") replaced by open()+fdopen() with 0644
# V3 (6-8):   S11 — --bus-dir newline injection prevention
# V4 (9-10):  Missing newline check on status field in scribe_log_append
# V5 (11-12): Idempotent claim postcondition (ensure_parent_dirs)
# V6 (13-14): Redundant access() removed — write_log_header handles both
# V7 (15):    Struct comment documents newline-free constraint
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
    echo "V$TEST_NUM. $1"
}

echo "=== nbs-scribe-log Audit V2 Tests ==="
echo "Test dir: $TEST_DIR"
echo "Binary:   $NBS_SCRIBE"

if [[ ! -x "$NBS_SCRIBE" ]]; then
    echo "FATAL: $NBS_SCRIBE not found or not executable"
    exit 1
fi

# =====================================================================
# V1: B23 — snprintf return checked on --status (main.c line 208)
# =====================================================================

next_test "B23: --status=decided writes correctly (baseline)"
LOG="$TEST_DIR/b23_1.md"
set +e
"$NBS_SCRIBE" "$LOG" "Status check" \
    --participants=alex --rationale="Baseline" \
    --status=decided \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "decided status exits 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Status field is 'decided'" \
    "$( grep -qF -- '- **Status:** decided' "$LOG" && echo pass || echo fail )"

next_test "B23: --status=reversed writes correctly"
LOG="$TEST_DIR/b23_2.md"
set +e
"$NBS_SCRIBE" "$LOG" "Reversed test" \
    --participants=alex --rationale="Testing reversed" \
    --status=reversed \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "reversed status exits 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Status field is 'reversed'" \
    "$( grep -qF -- '- **Status:** reversed' "$LOG" && echo pass || echo fail )"

next_test "B23: --status=invalid still rejected (regression guard)"
LOG="$TEST_DIR/b23_3.md"
set +e
"$NBS_SCRIBE" "$LOG" "Invalid status" \
    --participants=alex --rationale="Should fail" \
    --status=invalid \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Invalid status rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# V2: S10 — fopen("a") replaced by open()+fdopen() with explicit 0644
# =====================================================================

next_test "S10: New log file created with mode 0644 (not umask-dependent)"
# Set a permissive umask to prove the code does NOT rely on umask
OLD_UMASK=$(umask)
umask 0000
LOG="$TEST_DIR/s10_perms.md"
"$NBS_SCRIBE" "$LOG" "Permissions test" \
    --participants=alex --rationale="Check 0644" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
umask "$OLD_UMASK"
# File should be 0644 regardless of umask
PERMS=$(stat -c '%a' "$LOG" 2>/dev/null || stat -f '%Lp' "$LOG" 2>/dev/null)
check "Log file is mode 644" "$( [[ "$PERMS" == "644" ]] && echo pass || echo fail )"

next_test "S10: Append to existing log preserves content and permissions"
LOG="$TEST_DIR/s10_append.md"
# Create initial log
"$NBS_SCRIBE" "$LOG" "First entry" \
    --participants=alex --rationale="Initial" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
# Append second entry with permissive umask
OLD_UMASK=$(umask)
umask 0000
"$NBS_SCRIBE" "$LOG" "Second entry" \
    --participants=alex --rationale="Append test" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
umask "$OLD_UMASK"
HEADING_COUNT=$(grep -c '^### D-' "$LOG")
check "Both entries present after append" "$( [[ "$HEADING_COUNT" -eq 2 ]] && echo pass || echo fail )"
# Permissions should still be controlled
PERMS=$(stat -c '%a' "$LOG" 2>/dev/null || stat -f '%Lp' "$LOG" 2>/dev/null)
check "Permissions still 644 after append" "$( [[ "$PERMS" == "644" ]] && echo pass || echo fail )"

# =====================================================================
# V3: S11 — --bus-dir newline injection prevention
# =====================================================================

next_test "S11: --bus-dir with newline is rejected"
LOG="$TEST_DIR/s11_1.md"
INJECTED_BUS=$'/tmp/legit\n/tmp/evil'
set +e
"$NBS_SCRIBE" "$LOG" "Bus dir injection" \
    --participants=alex --rationale="Newline in bus-dir" \
    --bus-dir="$INJECTED_BUS" >/dev/null 2>&1
RC=$?
set -e
check "Newline in --bus-dir rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "S11: --bus-dir without newline still works"
BUS_DIR="$TEST_DIR/s11_clean_bus"
mkdir -p "$BUS_DIR"
LOG="$TEST_DIR/s11_2.md"
set +e
"$NBS_SCRIBE" "$LOG" "Clean bus dir" \
    --participants=alex --rationale="Normal bus-dir" \
    --bus-dir="$BUS_DIR" >/dev/null 2>&1
RC=$?
set -e
check "Clean --bus-dir accepted (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"

next_test "S11: --bus-dir with trailing newline is rejected"
LOG="$TEST_DIR/s11_3.md"
TRAILING_NL_BUS=$'/tmp/bus\n'
set +e
"$NBS_SCRIBE" "$LOG" "Trailing newline bus" \
    --participants=alex --rationale="Trailing NL" \
    --bus-dir="$TRAILING_NL_BUS" >/dev/null 2>&1
RC=$?
set -e
check "Trailing newline in --bus-dir rejected (exit 4)" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# V4: Missing newline check on status in scribe_log_append
# =====================================================================

# Note: status is validated against a whitelist in main.c, so newlines
# in status from the CLI are impossible. But the ASSERT_MSG in
# scribe_log_append protects the library-level API. We can only test
# the CLI boundary here — the assert protects against programmatic misuse.

next_test "V4: Status values with no newlines work (all 4 valid values)"
for STATUS in decided superseded reversed mitigated; do
    LOG="$TEST_DIR/v4_${STATUS}.md"
    set +e
    "$NBS_SCRIBE" "$LOG" "Status $STATUS" \
        --participants=alex --rationale="Testing $STATUS" \
        --status="$STATUS" \
        --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
    RC=$?
    set -e
    check "--status=$STATUS accepted (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
done

next_test "V4: Bogus status with embedded newline rejected at CLI"
LOG="$TEST_DIR/v4_inject.md"
# The whitelist check in main.c will reject this before reaching scribe_log_append
INJECTED_STATUS=$'decided\n### D-fake Injected'
set +e
"$NBS_SCRIBE" "$LOG" "Status injection" \
    --participants=alex --rationale="Injection via status" \
    --status="$INJECTED_STATUS" >/dev/null 2>&1
RC=$?
set -e
# Should be rejected either by whitelist (exit 4) or assert (signal)
check "Newline-injected status rejected (exit != 0)" "$( [[ "$RC" -ne 0 ]] && echo pass || echo fail )"

# =====================================================================
# V5: Idempotent claim postcondition (ensure_parent_dirs)
# =====================================================================

next_test "V5: Parent directories created for nested path"
LOG="$TEST_DIR/v5/deep/nested/dir/log.md"
set +e
"$NBS_SCRIBE" "$LOG" "Deep nested" \
    --participants=alex --rationale="Nested dirs" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Nested path succeeds (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Log file exists at nested path" "$( [[ -f "$LOG" ]] && echo pass || echo fail )"
# Postcondition: parent directory must be a directory
check "Parent dir is a directory" "$( [[ -d "$(dirname "$LOG")" ]] && echo pass || echo fail )"

next_test "V5: Repeated creation of same nested path succeeds (idempotency)"
LOG="$TEST_DIR/v5/deep/nested/dir/log.md"
set +e
"$NBS_SCRIBE" "$LOG" "Second append to nested" \
    --participants=alex --rationale="Idempotent dirs" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Second append to same nested path (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
HEADING_COUNT=$(grep -c '^### D-' "$LOG")
check "Both entries present" "$( [[ "$HEADING_COUNT" -eq 2 ]] && echo pass || echo fail )"

# =====================================================================
# V6: Redundant access() removed — write_log_header handles both
# =====================================================================

next_test "V6: Fresh file created without prior access() check"
LOG="$TEST_DIR/v6_fresh.md"
# The fix removes the access() call in scribe_log_init; write_log_header
# uses O_CREAT|O_EXCL to atomically create-or-fail.
set +e
"$NBS_SCRIBE" "$LOG" "Fresh file" \
    --participants=alex --rationale="No access() needed" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Fresh file creation succeeds (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Header present" "$( grep -qF '# Decision Log' "$LOG" && echo pass || echo fail )"
check "Decision present" "$( grep -qF 'Fresh file' "$LOG" && echo pass || echo fail )"

next_test "V6: Existing file append still works after access() removal"
LOG="$TEST_DIR/v6_existing.md"
# Create file first
"$NBS_SCRIBE" "$LOG" "First" \
    --participants=alex --rationale="Create" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
# Append to existing
set +e
"$NBS_SCRIBE" "$LOG" "Second" \
    --participants=alex --rationale="Append" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
check "Append to existing succeeds (exit 0)" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
HEADING_COUNT=$(grep -c '^### D-' "$LOG")
check "Both entries present" "$( [[ "$HEADING_COUNT" -eq 2 ]] && echo pass || echo fail )"

# =====================================================================
# V7: Struct comment documents newline-free constraint (scribe_log.h)
# =====================================================================

next_test "V7: scribe_log.h documents newline-free constraint"
HEADER_FILE="$PROJECT_ROOT/src/nbs-scribe-log/scribe_log.h"
check "Header file mentions newline-free constraint" \
    "$( grep -qi 'newline' "$HEADER_FILE" && echo pass || echo fail )"

# =====================================================================
# Summary
# =====================================================================

echo ""
echo "=== Audit V2 Test Result: $PASS passed, $FAIL failed ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All audit v2 tests passed"
    exit 0
else
    echo "FAIL: $FAIL audit v2 test(s) failed"
    exit 1
fi
