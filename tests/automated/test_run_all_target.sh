#!/bin/bash
# Test: --target flag in tests/run_all.sh filters correctly
#
# Falsification criteria:
#   1. --target=X must run ONLY tests matching X
#   2. No --target must run multiple tests
#   3. --target=nonexistent must run zero tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ALL="$PROJECT_DIR/tests/run_all.sh"

FAILURES=0

fail() {
    echo "  FAIL: $1"
    FAILURES=$((FAILURES + 1))
}

pass() {
    echo "  PASS: $1"
}

# --- Test 1: --target filters to a single test ---
echo "=== Test 1: --target=test_pty_session_lock runs only that test ==="

OUTPUT=$(bash "$RUN_ALL" --target=test_pty_session_lock --quick 2>&1) || true

if echo "$OUTPUT" | grep -q "PASSED: test_pty_session_lock\|FAILED: test_pty_session_lock"; then
    pass "target test appears in output"
else
    fail "target test (test_pty_session_lock) not found in output"
    echo "  Output was:"
    echo "$OUTPUT" | head -20
fi

# Check that other tests do NOT appear
for other in test_install test_nbs_chat_lifecycle test_home_validation; do
    if echo "$OUTPUT" | grep -qE "^(PASSED|FAILED): ${other}$"; then
        fail "non-target test '$other' was run (should have been filtered out)"
    else
        pass "non-target test '$other' correctly filtered out"
    fi
done

# --- Test 2: no --target runs multiple tests ---
echo ""
echo "=== Test 2: no --target runs multiple tests ==="

OUTPUT2=$(bash "$RUN_ALL" --quick 2>&1 | grep -cE "^(PASSED|FAILED):") || true

if [[ "$OUTPUT2" -gt 1 ]]; then
    pass "multiple tests ran without --target ($OUTPUT2 test results found)"
else
    fail "expected multiple tests without --target, got $OUTPUT2"
fi

# --- Test 3: non-matching target runs nothing ---
echo ""
echo "=== Test 3: --target=nonexistent_test_xyz runs zero tests ==="

OUTPUT3=$(bash "$RUN_ALL" --target=nonexistent_test_xyz --quick 2>&1) || true

if echo "$OUTPUT3" | grep -q "Passed:  0"; then
    pass "zero tests passed with non-matching target"
else
    fail "expected 'Passed:  0' in output"
    echo "  Output was:"
    echo "$OUTPUT3"
fi

# Also verify no PASSED/FAILED lines for actual tests
RESULT_COUNT=$(echo "$OUTPUT3" | grep -cE "^(PASSED|FAILED):" || true)
if [[ "$RESULT_COUNT" -eq 0 ]]; then
    pass "no test results with non-matching target"
else
    fail "found $RESULT_COUNT test results with non-matching target"
fi

# --- Summary ---
echo ""
if [[ $FAILURES -eq 0 ]]; then
    echo "=== ALL CHECKS PASSED ==="
    exit 0
else
    echo "=== $FAILURES CHECK(S) FAILED ==="
    exit 1
fi
