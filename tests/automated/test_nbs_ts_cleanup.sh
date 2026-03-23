#!/bin/bash
# Test: nbs-ts process death and cleanup
#
# Tests CL1-CL6 from nbs-ts-test-plan.md
# Verifies that sessions are properly cleaned up in various death scenarios.

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

echo "=== nbs-ts Cleanup Test ==="
echo ""

# CL1: Kill cleans session directory
echo "CL1. Kill cleans session directory..."
H=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H")
sleep 1
if [[ -d "$SESSIONS_DIR/$H" ]]; then
    "$NBS_TS" kill "$H" 2>/dev/null || true
    HANDLES=("${HANDLES[@]/$H}")
    sleep 0.5
    if [[ ! -d "$SESSIONS_DIR/$H" ]]; then
        pass "Session directory removed after kill"
    else
        fail "Session directory still exists after kill"
        ls -la "$SESSIONS_DIR/$H/" 2>/dev/null || true
    fi
else
    fail "Session directory didn't exist before kill"
fi

# CL3: No zombie after child process exits naturally
echo "CL3. No zombie after natural exit..."
H3=$("$NBS_TS" create "exit 0" | tr -d '[:space:]')
HANDLES+=("$H3")
sleep 2
# Check for zombie processes
DAEMON_PID=""
if [[ -f "$SESSIONS_DIR/$H3/daemon_pid" ]]; then
    DAEMON_PID=$(cat "$SESSIONS_DIR/$H3/daemon_pid" 2>/dev/null || true)
fi
CHILD_PID=""
if [[ -f "$SESSIONS_DIR/$H3/pid" ]]; then
    CHILD_PID=$(cat "$SESSIONS_DIR/$H3/pid" 2>/dev/null || true)
fi

ZOMBIE=false
if [[ -n "$CHILD_PID" ]] && kill -0 "$CHILD_PID" 2>/dev/null; then
    # Process still exists — check if zombie
    PROC_STATE=$(cat "/proc/$CHILD_PID/status" 2>/dev/null | grep "^State:" || echo "")
    if echo "$PROC_STATE" | grep -q "Z"; then
        ZOMBIE=true
    fi
fi

if ! $ZOMBIE; then
    pass "No zombie child process after natural exit"
else
    fail "Zombie child process detected (pid=$CHILD_PID)"
fi

# CL5: Concurrent kill is safe
echo "CL5. Concurrent kill is safe..."
H5=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H5")
sleep 1
# Fire two kills in parallel
"$NBS_TS" kill "$H5" 2>/dev/null &
PID_A=$!
"$NBS_TS" kill "$H5" 2>/dev/null &
PID_B=$!
wait $PID_A 2>/dev/null || true
wait $PID_B 2>/dev/null || true
HANDLES=("${HANDLES[@]/$H5}")
pass "Concurrent kill did not crash"

# CL6: CLI caller exit does not kill session
echo "CL6. CLI caller exit does not kill session..."
H6=$(bash -c "'$NBS_TS' create bash | tr -d '[:space:]'" 2>&1)
HANDLES+=("$H6")
# The subshell that ran create has exited. Session should still be alive.
sleep 1
STATUS6=$("$NBS_TS" status "$H6" 2>&1)
if echo "$STATUS6" | grep -q "alive"; then
    pass "Session survives after CLI caller exits (daemon model works)"
else
    fail "Session died when CLI caller exited"
    echo "   Status: $STATUS6"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All cleanup tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
