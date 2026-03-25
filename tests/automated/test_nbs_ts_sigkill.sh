#!/bin/bash
# test_nbs_ts_sigkill.sh — CL4: SIGKILL to daemon cleanup behaviour
set -euo pipefail

NBS_TS="${NBS_TS:-$(dirname "$0")/../../bin/nbs-ts}"
PASS=0; FAIL=0
pass() { echo "   PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "   FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== nbs-ts SIGKILL Test (CL4) ==="
echo ""

# CL4a: Child dies when daemon is SIGKILL'd
echo "CL4a. Child dies after daemon SIGKILL..."
HANDLE=$("$NBS_TS" create 'bash') || { fail "create failed"; exit 1; }
sleep 0.5
DAEMON_PID=$(cat ~/.nbs-ts/sessions/$HANDLE/daemon_pid 2>/dev/null)
CHILD_PID=$(cat ~/.nbs-ts/sessions/$HANDLE/pid 2>/dev/null)
kill -9 "$DAEMON_PID" 2>/dev/null || true
sleep 1
if kill -0 "$CHILD_PID" 2>/dev/null; then
    fail "Child still alive after daemon SIGKILL (orphan)"
else
    pass "Child dead after daemon SIGKILL"
fi

# CL4b: Session dir is stale (not cleaned up — expected)
echo "CL4b. Session dir stale after SIGKILL..."
if [ -d ~/.nbs-ts/sessions/$HANDLE ]; then
    pass "Session dir exists (expected — SIGKILL prevents cleanup)"
else
    fail "Session dir cleaned up (unexpected for SIGKILL)"
fi

# CL4c: nbs-ts kill cleans stale session dir
echo "CL4c. nbs-ts kill cleans stale dir..."
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
if [ -d ~/.nbs-ts/sessions/$HANDLE ]; then
    fail "Session dir still exists after kill"
else
    pass "Stale session dir cleaned by nbs-ts kill"
fi

# CL4d: nbs-ts list does not show stale session after kill
echo "CL4d. Stale session gone from list..."
if "$NBS_TS" list 2>/dev/null | grep -q "$HANDLE"; then
    fail "Stale session still in list"
else
    pass "Stale session removed from list"
fi

echo ""
echo "=== Result ==="
if [ "$FAIL" -eq 0 ]; then
    echo "PASS: All $PASS SIGKILL tests passed"
else
    echo "FAIL: $PASS passed, $FAIL failed"
    exit 1
fi
