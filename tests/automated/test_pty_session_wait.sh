#!/bin/bash
# Test: pty-session wait command detects pattern
#
# Falsification: Test fails if wait doesn't detect pattern that appears

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
PTY_SESSION="$PROJECT_ROOT/bin/pty-session"
VERDICT_FILE="$SCRIPT_DIR/verdicts/pty_session_wait_verdict.json"

SESSION_NAME="test_wait_$$"

cleanup() {
    "$PTY_SESSION" kill "$SESSION_NAME" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== Testing pty-session Wait Command ==="
echo ""

ERRORS=0

# Create session
echo "Setup: Creating session..."
"$PTY_SESSION" create "$SESSION_NAME" 'bash'
sleep 2

# Test 1: Wait for prompt (should already be there)
# Send a known marker so we don't depend on PS1 format
"$PTY_SESSION" send "$SESSION_NAME" 'echo PROMPT_READY'
echo "Test 1: Wait for existing pattern..."
if "$PTY_SESSION" wait "$SESSION_NAME" 'PROMPT_READY' --timeout=10; then
    echo "  PASS: Found prompt immediately"
else
    echo "  FAIL: Could not find prompt"
    ERRORS=$((ERRORS + 1))
fi

# Test 2: Send command, wait for output
echo "Test 2: Wait for pattern after command..."
"$PTY_SESSION" send "$SESSION_NAME" 'sleep 2 && echo DELAYED_OUTPUT'

if "$PTY_SESSION" wait "$SESSION_NAME" 'DELAYED_OUTPUT' --timeout=10; then
    echo "  PASS: Found delayed output"
else
    echo "  FAIL: Did not find delayed output"
    ERRORS=$((ERRORS + 1))
fi

# Test 3: Verify output is in read
echo "Test 3: Verify pattern in read..."
OUTPUT=$("$PTY_SESSION" read "$SESSION_NAME")
if echo "$OUTPUT" | grep -q "DELAYED_OUTPUT"; then
    echo "  PASS: Pattern confirmed in read"
else
    echo "  FAIL: Pattern not in read output"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "=== RESULT ==="

if [[ $ERRORS -eq 0 ]]; then
    echo "Verdict: PASS"
    echo '{"verdict": "PASS", "errors": 0, "tests_run": 3}' > "$VERDICT_FILE"
    exit 0
else
    echo "Verdict: FAIL"
    echo "{\"verdict\": \"FAIL\", \"errors\": $ERRORS, \"tests_run\": 3}" > "$VERDICT_FILE"
    exit 1
fi
