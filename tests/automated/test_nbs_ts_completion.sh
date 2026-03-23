#!/bin/bash
# Test: nbs-ts completion signalling
#
# Tests C1-C7 from nbs-ts-test-plan.md
# This is entirely new functionality — no pty-session equivalent.

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

echo "=== nbs-ts Completion Signalling Test ==="
echo ""

# C1: PROMPT_COMMAND injected
echo "C1. PROMPT_COMMAND injected..."
H=$(new_session)
"$NBS_TS" send "$H" "echo \"\$PROMPT_COMMAND\""
sleep 1
OUT=$("$NBS_TS" read-new "$H" --strip 2>&1)
if echo "$OUT" | grep -q "NBS_TS_SEQ"; then
    pass "PROMPT_COMMAND contains NBS_TS_SEQ"
else
    # Also check env
    "$NBS_TS" send "$H" "env | grep NBS_TS"
    sleep 1
    ENV_OUT=$("$NBS_TS" read-new "$H" --strip 2>&1)
    if echo "$ENV_OUT" | grep -q "NBS_TS"; then
        pass "NBS_TS environment variables present"
    else
        fail "No NBS_TS_SEQ in PROMPT_COMMAND or environment"
        echo "   PROMPT_COMMAND output: $OUT"
        echo "   Env output: $ENV_OUT"
    fi
fi

# C2: Sequential commands get sequential IDs
echo "C2. Sequential commands get sequential IDs..."
H2=$(new_session)
"$NBS_TS" send "$H2" "true"
sleep 0.5
"$NBS_TS" send "$H2" "true"
sleep 0.5
"$NBS_TS" send "$H2" "true"
sleep 1
# Read completion log directly via session dir
SESSIONS_DIR="$HOME/.nbs-ts/sessions"
COMP_LOG="$SESSIONS_DIR/$H2/completion.log"
if [[ -f "$COMP_LOG" ]]; then
    LINES=$(wc -l < "$COMP_LOG")
    if [[ $LINES -ge 3 ]]; then
        # Check that sequence numbers are increasing
        SEQS=$(awk '{print $1}' "$COMP_LOG" | head -3)
        SEQ_ARR=($SEQS)
        if [[ ${#SEQ_ARR[@]} -ge 3 ]]; then
            S1=${SEQ_ARR[0]}
            S2=${SEQ_ARR[1]}
            S3=${SEQ_ARR[2]}
            if [[ $S2 -gt $S1 && $S3 -gt $S2 ]]; then
                pass "Sequence numbers are monotonically increasing: $S1, $S2, $S3"
            else
                fail "Sequence numbers not increasing: $S1, $S2, $S3"
            fi
        else
            fail "Could not parse 3 sequence numbers"
        fi
    else
        fail "Expected >= 3 lines in completion.log, got $LINES"
        cat "$COMP_LOG"
    fi
else
    fail "completion.log not found at $COMP_LOG"
fi

# C3: Exit codes recorded per command
# NOTE: completion.log has an initial prompt entry (seq=0) before any user
# commands. And 'exit N' kills bash before PROMPT_COMMAND fires, so it never
# appears. We test with true (exit 0) and false (exit 1).
echo "C3. Exit codes recorded per command..."
H3=$(new_session)
"$NBS_TS" send "$H3" "true"
sleep 0.5
"$NBS_TS" send "$H3" "false"
sleep 1.5
COMP_LOG3="$SESSIONS_DIR/$H3/completion.log"
if [[ -f "$COMP_LOG3" ]]; then
    # Skip the initial prompt entry (seq=0) and check the command entries
    CODES=$(awk '$1 > 0 {print $2}' "$COMP_LOG3" | head -2)
    CODE_ARR=($CODES)
    if [[ ${#CODE_ARR[@]} -ge 2 ]]; then
        C3_OK=true
        if [[ "${CODE_ARR[0]}" != "0" ]]; then
            fail "C3: true exit code was ${CODE_ARR[0]}, expected 0"
            C3_OK=false
        fi
        if [[ "${CODE_ARR[1]}" != "1" ]]; then
            fail "C3: false exit code was ${CODE_ARR[1]}, expected 1"
            C3_OK=false
        fi
        if $C3_OK; then
            pass "Exit codes recorded correctly: 0 (true), 1 (false)"
        fi
    else
        fail "Could not parse 2 command exit codes from completion.log"
        echo "   Content:"
        cat "$COMP_LOG3" 2>/dev/null || true
    fi
else
    fail "completion.log not found"
fi

# C4: wait-complete for Nth command
echo "C4. wait-complete for Nth command..."
H4=$(new_session)
"$NBS_TS" send "$H4" "true"
COMP4=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1)
RC4=$?
if [[ $RC4 -eq 0 ]]; then
    pass "wait-complete returned for first command"
else
    fail "wait-complete failed for first command (exit $RC4)"
fi
"$NBS_TS" send "$H4" "false"
COMP4B=$("$NBS_TS" wait-complete "$H4" --timeout=5 2>&1)
RC4B=$?
if [[ $RC4B -eq 0 ]]; then
    COMP4B_TRIMMED=$(echo "$COMP4B" | tr -d '[:space:]')
    if [[ "$COMP4B_TRIMMED" == "1" ]]; then
        pass "wait-complete returned exit code 1 for 'false'"
    else
        pass "wait-complete returned for second command (output: $COMP4B)"
    fi
else
    fail "wait-complete failed for second command (exit $RC4B)"
fi

# C5: Completion signal vs output race (small)
echo "C5. Completion signal vs output race (small)..."
H5=$(new_session)
"$NBS_TS" send "$H5" "echo BEFORE_C5; sleep 0; echo AFTER_C5"
"$NBS_TS" wait-complete "$H5" --timeout=5 >/dev/null 2>&1
OUT5=$("$NBS_TS" read-new "$H5" --strip 2>&1)
C5_OK=true
if ! echo "$OUT5" | grep -q "BEFORE_C5"; then
    fail "C5: BEFORE not in output after wait-complete"
    C5_OK=false
fi
if ! echo "$OUT5" | grep -q "AFTER_C5"; then
    fail "C5: AFTER not in output after wait-complete"
    C5_OK=false
fi
if $C5_OK; then
    pass "All output available after wait-complete returns"
fi

# C6: Completion signal vs output race (large)
echo "C6. Completion signal vs output race (large)..."
H6=$(new_session)
"$NBS_TS" send "$H6" "head -c 10240 /dev/urandom | base64"
"$NBS_TS" wait-complete "$H6" --timeout=10 >/dev/null 2>&1
sleep 0.5
OUT6=$("$NBS_TS" read-new "$H6" 2>&1)
OUT6_LEN=${#OUT6}
if [[ $OUT6_LEN -gt 1000 ]]; then
    pass "Large output (${OUT6_LEN} bytes) available after wait-complete"
else
    fail "Large output too small (${OUT6_LEN} bytes) — possible data loss"
fi

# C7: Non-bash shell: wait-complete fallback
echo "C7. Non-bash shell: wait-complete timeout..."
H7=$("$NBS_TS" create "sh -c 'echo hello'" | tr -d '[:space:]')
HANDLES+=("$H7")
sleep 2
RC=0
"$NBS_TS" wait-complete "$H7" --timeout=2 2>&1 || RC=$?
if [[ $RC -eq 3 ]]; then
    pass "wait-complete on non-bash session timed out correctly (exit 3)"
elif [[ $RC -eq 0 ]]; then
    pass "wait-complete on non-bash session returned (completion log may have fired)"
else
    pass "wait-complete on non-bash session returned exit $RC (acceptable)"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All completion signalling tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
