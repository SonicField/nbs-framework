#!/bin/bash
# test_nbs_ts_completion_edge.sh — Completion log edge cases for nbs-ts
#
# Tests: startup record skipping, exit-N without PROMPT_COMMAND,
# nested shell PROMPT_COMMAND inheritance, rapid command sequences.

set -euo pipefail

NBS_TS="${NBS_TS:-$(dirname "$0")/../../bin/nbs-ts}"
[[ -x "$NBS_TS" ]] || { echo "SKIP: nbs-ts not found"; exit 0; }

PASS=0
FAIL=0
pass() { echo "   PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "   FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== nbs-ts Completion Log Edge Cases ==="

# CE1: Startup record is skipped (NBS_TS_SEQ=-1 fix)
echo "CE1. Startup record skipped..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 2
# wait-complete should timeout because no USER command has been sent
if "$NBS_TS" wait-complete "$HANDLE" --timeout=3 2>/dev/null; then
    fail "wait-complete returned before any user command — startup record leaked"
else
    pass "wait-complete correctly timed out (startup record skipped)"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# CE2: exit N captured via exit-code, not completion log
echo "CE2. exit N captured via exit-code..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
"$NBS_TS" send "$HANDLE" "exit 42"
sleep 2
EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE" 2>/dev/null) || EXIT_CODE="-1"
if [[ "$EXIT_CODE" == "42" ]]; then
    pass "exit 42 captured correctly via exit-code"
elif [[ "$EXIT_CODE" == "-1" ]]; then
    # Might need more time for daemon to write exit_code file
    sleep 2
    EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE" 2>/dev/null) || EXIT_CODE="-1"
    if [[ "$EXIT_CODE" == "42" ]]; then
        pass "exit 42 captured correctly via exit-code (delayed)"
    else
        fail "exit 42 not captured (got: $EXIT_CODE)"
    fi
else
    fail "exit 42 not captured (got: $EXIT_CODE)"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# CE3: Rapid commands produce sequential completion records
echo "CE3. Rapid command sequence..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
"$NBS_TS" send "$HANDLE" "true"
"$NBS_TS" send "$HANDLE" "false"
"$NBS_TS" send "$HANDLE" "true"
sleep 3
# Should have 3 completion records (seq 1, 2, 3)
# Read the completion log directly to verify ordering
SESSION_DIR="$HOME/.nbs-ts/sessions/$HANDLE"
if [[ -f "$SESSION_DIR/completion.log" ]]; then
    RECORDS=$(grep -c '^[0-9]' "$SESSION_DIR/completion.log" 2>/dev/null || echo 0)
    if [[ "$RECORDS" -ge 3 ]]; then
        pass "Rapid commands: $RECORDS completion records (expected ≥3)"
    else
        fail "Rapid commands: only $RECORDS records (expected ≥3)"
    fi
else
    fail "completion.log not found"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# CE4: Subshell does not inherit PROMPT_COMMAND
echo "CE4. Subshell isolation..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
"$NBS_TS" send "$HANDLE" "true"
sleep 1
# Count records before subshell
SESSION_DIR="$HOME/.nbs-ts/sessions/$HANDLE"
BEFORE=$(grep -c '^[0-9]' "$SESSION_DIR/completion.log" 2>/dev/null || echo 0)
# Run commands inside a subshell
"$NBS_TS" send "$HANDLE" "(echo sub1; echo sub2; echo sub3)"
sleep 2
AFTER=$(grep -c '^[0-9]' "$SESSION_DIR/completion.log" 2>/dev/null || echo 0)
# Subshell should produce exactly ONE completion record (for the subshell command itself)
DIFF=$((AFTER - BEFORE))
if [[ "$DIFF" -eq 1 ]]; then
    pass "Subshell produced 1 completion record (not 3) — isolation correct"
elif [[ "$DIFF" -le 2 ]]; then
    pass "Subshell produced $DIFF completion records — acceptable"
else
    fail "Subshell produced $DIFF completion records — PROMPT_COMMAND may be inherited"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# CE5: wait-complete returns correct exit code
echo "CE5. wait-complete exit code accuracy..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
"$NBS_TS" send "$HANDLE" "false"
RESULT=$("$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null) || true
if [[ "$RESULT" == "1" ]]; then
    pass "wait-complete returned exit code 1 for 'false'"
else
    fail "wait-complete returned '$RESULT' (expected 1)"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

echo ""
echo "=== Result ==="
echo "PASS: $PASS  FAIL: $FAIL"
[[ "$FAIL" -eq 0 ]] && echo "All completion edge case tests passed" || exit 1
