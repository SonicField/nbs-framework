#!/bin/bash
# Test: nbs-ts one-shot command lifecycle
#
# Tests O1-O5 from nbs-ts-test-plan.md
# Verifies that short-lived commands have their output captured before exit.

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

echo "=== nbs-ts One-Shot Test ==="
echo ""

# O1: One-shot output captured
echo "O1. One-shot output captured..."
MARKER="ONESHOT_$$_$(date +%s)"
HANDLE=$("$NBS_TS" create "echo $MARKER" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2
READ=$("$NBS_TS" read-new "$HANDLE" --strip 2>&1)
if echo "$READ" | grep -q "$MARKER"; then
    pass "One-shot output contains marker"
else
    fail "One-shot output missing marker"
    echo "   Expected: $MARKER"
    echo "   Got: $READ"
fi

# O2: One-shot session exits
echo "O2. One-shot session exits..."
STATUS=$("$NBS_TS" status "$HANDLE" 2>&1)
if echo "$STATUS" | grep -q "dead"; then
    pass "One-shot session is dead"
else
    fail "One-shot session not dead"
    echo "   Status: $STATUS"
fi

# O3: Exit code captured
echo "O3. Exit code captured..."
HANDLE2=$("$NBS_TS" create "exit 42" | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 2
EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE2" 2>&1 | tr -d '[:space:]')
if [[ "$EXIT_CODE" == "42" ]]; then
    pass "Exit code is 42"
else
    fail "Exit code wrong"
    echo "   Expected: 42, Got: $EXIT_CODE"
fi

# O4: Command-not-found exits cleanly
echo "O4. Command-not-found exits cleanly..."
HANDLE3=$("$NBS_TS" create "nonexistent_xyz_$$" | tr -d '[:space:]')
HANDLES+=("$HANDLE3")
sleep 2
STATUS3=$("$NBS_TS" status "$HANDLE3" 2>&1)
if echo "$STATUS3" | grep -q "dead"; then
    pass "Bad command session is dead (no zombie)"
else
    fail "Bad command session not dead"
    echo "   Status: $STATUS3"
fi

# O5: Failed command log non-empty
echo "O5. Failed command log non-empty..."
READ3=$("$NBS_TS" read-new "$HANDLE3" --strip 2>&1)
if [[ -n "$READ3" ]]; then
    pass "Output log has content for failed command"
else
    fail "Output log empty for failed command (stderr not captured?)"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All 5 one-shot tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
