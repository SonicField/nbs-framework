#!/bin/bash
# Test: nbs-ts core lifecycle — create/send/read/kill cycle
#
# Tests L1-L7 from nbs-ts-test-plan.md
# Falsification: each test can fail if its targeted operation is broken.

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

echo "=== nbs-ts Lifecycle Test ==="
echo ""

# L1: Create interactive session
echo "L1. Create interactive session..."
HANDLE=$("$NBS_TS" create bash 2>&1)
if [[ $? -eq 0 ]] && [[ ${#HANDLE} -eq 9 || ${#HANDLE} -eq 8 ]]; then
    HANDLE=$(echo "$HANDLE" | tr -d '[:space:]')
    HANDLES+=("$HANDLE")
    pass "Created session $HANDLE"
else
    fail "Create failed or returned unexpected handle: '$HANDLE'"
    echo "Cannot continue without a session."
    exit 1
fi

sleep 1

# L2: List shows session alive
echo "L2. List shows session alive..."
LIST_OUTPUT=$("$NBS_TS" list 2>&1)
if echo "$LIST_OUTPUT" | grep -q "${HANDLE}.*alive"; then
    pass "Session visible and alive"
else
    fail "Session not in list or not alive"
    echo "   List output: $LIST_OUTPUT"
fi

# L3: Send command
echo "L3. Send command..."
MARKER="NBS_TS_LIFECYCLE_$$_$(date +%s)"
if "$NBS_TS" send "$HANDLE" "echo $MARKER" 2>&1; then
    pass "Command sent (exit 0)"
else
    fail "Send returned non-zero"
fi

sleep 1

# L4: Read-new shows output
echo "L4. Read-new shows output..."
READ_OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>&1)
if echo "$READ_OUTPUT" | grep -q "$MARKER"; then
    pass "Marker found in read-new output"
else
    fail "Marker not found in read-new output"
    echo "   Expected: $MARKER"
    echo "   Got: $READ_OUTPUT"
fi

# L5: Read-new is idempotent-empty
echo "L5. Read-new is idempotent-empty..."
READ_AGAIN=$("$NBS_TS" read-new "$HANDLE" --strip 2>&1)
if [[ -z "$READ_AGAIN" ]]; then
    pass "Second read-new returned empty"
else
    fail "Second read-new returned data (cursor not advanced)"
    echo "   Got: $READ_AGAIN"
fi

# L6: Kill session
echo "L6. Kill session..."
if "$NBS_TS" kill "$HANDLE" 2>&1; then
    pass "Kill returned exit 0"
else
    fail "Kill returned non-zero"
fi

sleep 0.5

# L7: List shows session gone
echo "L7. List shows session gone..."
LIST_AFTER=$("$NBS_TS" list 2>&1)
if ! echo "$LIST_AFTER" | grep -q "$HANDLE"; then
    pass "Session removed from list"
else
    fail "Session still in list after kill"
    echo "   List output: $LIST_AFTER"
fi

# Remove handle from cleanup list since we already killed it
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All 7 lifecycle tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
