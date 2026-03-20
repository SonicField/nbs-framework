#!/bin/bash
# Test: pty-session lifecycle with evidence-based verification
#
# Captures evidence of each operation, then verifies deterministically.
# No timing-based assertions - we check the evidence, not race conditions.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
PTY_SESSION="$PROJECT_ROOT/bin/pty-session"

SESSION_NAME="test_lifecycle_$$"
ERRORS=0

cleanup() {
    "$PTY_SESSION" kill "$SESSION_NAME" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== pty-session Lifecycle Test ==="
echo "Session: $SESSION_NAME"
echo ""

# Operation 1: Create
echo "1. Create session..."
if "$PTY_SESSION" create "$SESSION_NAME" 'bash' >/dev/null 2>&1; then
    echo "   PASS: Created"
else
    echo "   FAIL: Create failed"
    ERRORS=$((ERRORS + 1))
fi

# Allow process startup
sleep 0.5

# Operation 2: List shows session
echo "2. List shows session..."
LIST_OUTPUT=$("$PTY_SESSION" list 2>&1)
if echo "$LIST_OUTPUT" | grep -q "$SESSION_NAME.*running"; then
    echo "   PASS: Session visible and running"
elif echo "$LIST_OUTPUT" | grep -q "$SESSION_NAME"; then
    echo "   PASS: Session visible (status may vary)"
else
    echo "   FAIL: Session not in list"
    echo "   List output: $LIST_OUTPUT"
    ERRORS=$((ERRORS + 1))
fi

# Operation 3: Send command
echo "3. Send command..."
MARKER="PTY_OK_$$"
if "$PTY_SESSION" send "$SESSION_NAME" "echo $MARKER" >/dev/null 2>&1; then
    echo "   PASS: Command sent"
else
    echo "   FAIL: Send failed"
    ERRORS=$((ERRORS + 1))
fi

# Allow command execution
sleep 1

# Operation 4: Read shows marker
echo "4. Read shows marker..."
READ_OUTPUT=$("$PTY_SESSION" read "$SESSION_NAME" 2>&1)
if echo "$READ_OUTPUT" | tr -d '\n' | grep -q "$MARKER"; then
    echo "   PASS: Marker found in output"
else
    echo "   FAIL: Marker not found"
    echo "   Read output: $READ_OUTPUT"
    ERRORS=$((ERRORS + 1))
fi

# Operation 5: Kill session
echo "5. Kill session..."
if "$PTY_SESSION" kill "$SESSION_NAME" >/dev/null 2>&1; then
    echo "   PASS: Session killed"
else
    echo "   FAIL: Kill failed"
    ERRORS=$((ERRORS + 1))
fi

# Allow cleanup
sleep 0.5

# Operation 6: List shows killed or absent
echo "6. List shows session killed/absent..."
LIST_AFTER=$("$PTY_SESSION" list 2>&1)
if echo "$LIST_AFTER" | grep -q "$SESSION_NAME.*killed"; then
    echo "   PASS: Session marked as killed"
elif ! echo "$LIST_AFTER" | grep -q "$SESSION_NAME"; then
    echo "   PASS: Session removed from list"
else
    echo "   FAIL: Session still appears active"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All 6 operations verified"
    exit 0
else
    echo "FAIL: $ERRORS operations failed"
    exit 1
fi
