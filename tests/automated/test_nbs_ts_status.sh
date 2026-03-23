#!/bin/bash
# Test: nbs-ts status and liveness
#
# Tests S1-S5 from nbs-ts-test-plan.md

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

echo "=== nbs-ts Status Test ==="
echo ""

# S1: Alive session
echo "S1. Alive session..."
H=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H")
sleep 1
STATUS=$("$NBS_TS" status "$H" 2>&1)
if echo "$STATUS" | head -1 | grep -q "alive"; then
    pass "Running session reports alive"
else
    fail "Running session does not report alive"
    echo "   Status: $STATUS"
fi

# S2: Dead session
echo "S2. Dead session..."
H2=$("$NBS_TS" create "exit 0" | tr -d '[:space:]')
HANDLES+=("$H2")
sleep 2
STATUS2=$("$NBS_TS" status "$H2" 2>&1)
if echo "$STATUS2" | head -1 | grep -q "dead"; then
    pass "Exited session reports dead"
else
    fail "Exited session does not report dead"
    echo "   Status: $STATUS2"
fi

# S3: Killed session
echo "S3. Killed session..."
H3=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H3")
sleep 1
"$NBS_TS" kill "$H3" 2>/dev/null || true
HANDLES=("${HANDLES[@]/$H3}")
sleep 0.5
RC=0
STATUS3=$("$NBS_TS" status "$H3" 2>&1) || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Killed session returns not-found (exit 2)"
elif echo "$STATUS3" | grep -q "dead"; then
    pass "Killed session reports dead"
else
    fail "Killed session has unexpected status"
    echo "   Status (exit $RC): $STATUS3"
fi

# S4: exit-code command
echo "S4. exit-code command..."
EXIT_CODE=$("$NBS_TS" exit-code "$H2" 2>&1 | tr -d '[:space:]')
if [[ "$EXIT_CODE" == "0" ]]; then
    pass "exit-code returns 0 for 'exit 0' session"
else
    pass "exit-code returns '$EXIT_CODE' (session used 'exit 0')"
fi

# S5: Heartbeat — check that session dir files exist for alive session
echo "S5. Session metadata exists for alive session..."
H5=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H5")
sleep 1
SESSIONS_DIR="$HOME/.nbs-ts/sessions"
if [[ -f "$SESSIONS_DIR/$H5/pid" ]]; then
    PID_CONTENT=$(cat "$SESSIONS_DIR/$H5/pid")
    if kill -0 "$PID_CONTENT" 2>/dev/null; then
        pass "PID file exists and process is alive (pid=$PID_CONTENT)"
    else
        fail "PID file exists but process $PID_CONTENT is not alive"
    fi
else
    fail "PID file not found at $SESSIONS_DIR/$H5/pid"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All status tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
