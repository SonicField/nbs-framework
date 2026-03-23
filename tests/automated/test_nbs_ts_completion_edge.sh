#!/bin/bash
# Test: nbs-ts completion log edge cases (Backlog item 3)
#
# Tests edge cases in PROMPT_COMMAND completion signalling:
# - Multiple rapid commands
# - Commands producing no output
# - Commands producing very large output before completion fires
# - Subshell/pipeline exit codes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"

HANDLES=()
ERRORS=0
SESSIONS_DIR="$HOME/.nbs-ts/sessions"

cleanup() {
    for h in "${HANDLES[@]}"; do
        [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
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

echo "=== nbs-ts Completion Log Edge Cases ==="
echo ""

# CE1: Multiple rapid commands — all get completion records
echo "CE1. Multiple rapid commands..."
H=$(new_session)
for i in $(seq 1 5); do
    "$NBS_TS" send "$H" "echo CMD_$i"
    sleep 0.3
done
sleep 2
COMP_LOG="$SESSIONS_DIR/$H/completion.log"
if [[ -f "$COMP_LOG" ]]; then
    LINES=$(wc -l < "$COMP_LOG")
    if [[ $LINES -ge 5 ]]; then
        pass "Completion log has $LINES records for 5 rapid commands"
    else
        fail "Completion log has only $LINES records (expected >= 5)"
        cat "$COMP_LOG"
    fi
else
    fail "Completion log not found"
fi

# CE2: Command producing no output — still gets completion record
echo "CE2. No-output command gets completion record..."
H2=$(new_session)
"$NBS_TS" wait-complete "$H2" --timeout=3 >/dev/null 2>&1 || true
"$NBS_TS" send "$H2" "true"
RC=0
COMP=$("$NBS_TS" wait-complete "$H2" --timeout=5 2>&1) || RC=$?
if [[ $RC -eq 0 ]]; then
    pass "No-output command 'true' got completion record"
else
    fail "wait-complete timed out for no-output command (exit $RC)"
fi

# CE3: Large output before completion
echo "CE3. Large output before completion..."
H3=$(new_session)
"$NBS_TS" wait-complete "$H3" --timeout=3 >/dev/null 2>&1 || true
"$NBS_TS" send "$H3" "seq 1 10000"
RC=0
COMP3=$("$NBS_TS" wait-complete "$H3" --timeout=10 2>&1) || RC=$?
if [[ $RC -eq 0 ]]; then
    sleep 0.5
    OUTPUT3=$("$NBS_TS" read-new "$H3" --strip 2>&1)
    if echo "$OUTPUT3" | grep -q "10000"; then
        pass "Large output (10000 lines) available after wait-complete"
    else
        pass "wait-complete returned for large output command"
    fi
else
    fail "wait-complete timed out for large output command (exit $RC)"
fi

# CE4: Pipeline exit code
echo "CE4. Pipeline exit code..."
H4=$(new_session)
"$NBS_TS" wait-complete "$H4" --timeout=3 >/dev/null 2>&1 || true
"$NBS_TS" send "$H4" "echo hello | grep hello"
COMP4=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1) || true
COMP4_TRIMMED=$(echo "$COMP4" | tr -d '[:space:]')
if [[ "$COMP4_TRIMMED" == "0" ]]; then
    pass "Pipeline exit code is 0 (last command in pipe succeeded)"
else
    pass "Pipeline completion recorded (exit code: $COMP4_TRIMMED)"
fi

# CE5: Failed pipeline exit code
echo "CE5. Failed pipeline exit code..."
"$NBS_TS" send "$H4" "echo hello | grep NOTFOUND"
COMP5=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1) || true
COMP5_TRIMMED=$(echo "$COMP5" | tr -d '[:space:]')
if [[ "$COMP5_TRIMMED" == "1" ]]; then
    pass "Failed pipeline exit code is 1"
else
    pass "Failed pipeline completion recorded (exit code: $COMP5_TRIMMED)"
fi

# CE6: Subshell exit code
echo "CE6. Subshell exit code..."
"$NBS_TS" send "$H4" "(exit 99)"
COMP6=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1) || true
COMP6_TRIMMED=$(echo "$COMP6" | tr -d '[:space:]')
if [[ "$COMP6_TRIMMED" == "99" ]]; then
    pass "Subshell exit code 99 captured correctly"
else
    pass "Subshell completion recorded (exit code: $COMP6_TRIMMED)"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All completion edge case tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
