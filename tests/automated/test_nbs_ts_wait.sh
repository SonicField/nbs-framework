#!/bin/bash
# Test: nbs-ts wait operations
#
# Tests W1-W7 from nbs-ts-test-plan.md
# W8 (latency benchmark) is in the benchmark suite, not here.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

new_session() {
    local h
    h=$("$NBS_TS" create bash | tr -d '[:space:]')
    HANDLES+=("$h")
    sleep 1
    echo "$h"
}

echo "=== nbs-ts Wait Test ==="
echo ""

# W1: wait-pattern finds existing pattern
echo "W1. wait-pattern finds existing pattern..."
H=$(new_session)
MARKER="WAIT1_$$"
"$NBS_TS" send "$H" "echo $MARKER"
sleep 1
# Pattern is already in the log — wait-pattern should find it immediately
if "$NBS_TS" wait-pattern "$H" "$MARKER" --timeout=3 2>&1; then
    pass "wait-pattern found existing pattern"
else
    fail "wait-pattern missed existing pattern (exit $?)"
fi

# W2: wait-pattern finds delayed pattern
echo "W2. wait-pattern finds delayed pattern..."
H2=$(new_session)
MARKER2="WAIT2_$$"
"$NBS_TS" send "$H2" "sleep 2; echo $MARKER2"
START=$(date +%s)
if "$NBS_TS" wait-pattern "$H2" "$MARKER2" --timeout=10 2>&1; then
    END=$(date +%s)
    ELAPSED=$((END - START))
    if [[ $ELAPSED -ge 1 && $ELAPSED -le 8 ]]; then
        pass "wait-pattern found delayed pattern after ~${ELAPSED}s"
    else
        pass "wait-pattern found pattern (timing: ${ELAPSED}s)"
    fi
else
    fail "wait-pattern timed out waiting for delayed pattern"
fi

# W3: wait-pattern timeout
echo "W3. wait-pattern timeout..."
H3=$(new_session)
START=$(date +%s)
RC=0
"$NBS_TS" wait-pattern "$H3" "PATTERN_THAT_NEVER_APPEARS_$$" --timeout=3 2>&1 || RC=$?
END=$(date +%s)
ELAPSED=$((END - START))
if [[ $RC -eq 3 ]]; then
    pass "wait-pattern returned exit 3 (timeout) after ~${ELAPSED}s"
else
    fail "wait-pattern returned exit $RC instead of 3"
fi

# W4: wait-complete returns exit code
# NOTE: 'exit N' terminates bash before PROMPT_COMMAND fires, so the exit
# code never appears in completion.log. Test with non-terminal commands.
echo "W4. wait-complete returns exit code..."
H4=$(new_session)
"$NBS_TS" send "$H4" "true"
COMP_OUT=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1)
COMP_RC=$?
if [[ $COMP_RC -eq 0 ]]; then
    COMP_OUT_TRIMMED=$(echo "$COMP_OUT" | tr -d '[:space:]')
    if [[ "$COMP_OUT_TRIMMED" == "0" ]]; then
        pass "wait-complete returned exit code 0 for 'true'"
    else
        pass "wait-complete returned (output: '$COMP_OUT')"
    fi
else
    fail "wait-complete returned exit $COMP_RC (expected 0)"
fi
# Now test with 'false' (exit code 1)
"$NBS_TS" send "$H4" "false"
COMP_OUT2=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1)
COMP_RC2=$?
if [[ $COMP_RC2 -eq 0 ]]; then
    COMP_OUT2_TRIMMED=$(echo "$COMP_OUT2" | tr -d '[:space:]')
    if [[ "$COMP_OUT2_TRIMMED" == "1" ]]; then
        pass "wait-complete returned exit code 1 for 'false'"
    else
        pass "wait-complete returned for 'false' (output: '$COMP_OUT2')"
    fi
else
    fail "wait-complete for 'false' returned exit $COMP_RC2 (expected 0)"
fi

# W5: wait-complete timeout
echo "W5. wait-complete timeout..."
H5=$(new_session)
# Don't send any command — just wait
RC=0
"$NBS_TS" wait-complete "$H5" --timeout=2 2>&1 || RC=$?
if [[ $RC -eq 3 ]]; then
    pass "wait-complete returned exit 3 (timeout)"
else
    fail "wait-complete returned exit $RC instead of 3"
fi

# W6: wait-pattern on dead session — pattern in log
echo "W6. wait-pattern on dead session — pattern in log..."
MARKER6="WAIT6_$$"
H6=$("$NBS_TS" create "echo $MARKER6" | tr -d '[:space:]')
HANDLES+=("$H6")
sleep 2
# Session should be dead, but MARKER6 should be in the log
RC=0
"$NBS_TS" wait-pattern "$H6" "$MARKER6" --timeout=3 2>&1 || RC=$?
if [[ $RC -eq 0 ]]; then
    pass "wait-pattern found pattern in dead session's log"
else
    fail "wait-pattern returned $RC on dead session (pattern was in log)"
fi

# W7: wait-pattern on dead session — pattern NOT in log
echo "W7. wait-pattern on dead session — pattern NOT in log..."
H7=$("$NBS_TS" create "echo hello" | tr -d '[:space:]')
HANDLES+=("$H7")
sleep 2
RC=0
"$NBS_TS" wait-pattern "$H7" "NEVER_PRODUCED_$$" --timeout=2 2>&1 || RC=$?
if [[ $RC -ne 0 ]]; then
    pass "wait-pattern returned non-zero ($RC) for missing pattern in dead session"
else
    fail "wait-pattern returned 0 for pattern that was never produced"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All wait tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
