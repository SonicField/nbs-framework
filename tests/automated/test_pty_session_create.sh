#!/bin/bash
# Test: pty-session create lifecycle
#
# Falsifiable claims:
#   1. Fast one-shot command: output captured in log, wait finds pattern
#   2. Command not found: session exits cleanly (no orphan bash)
#   3. Interactive shell: send/wait/read cycle works
#   4. Session dies after one-shot command completes
#   5. Log file is non-empty for successful one-shot command
#   6. Log file is non-empty for failed command (captures error)
#
# These tests target the pipe-pane race fix: creating with bash,
# attaching pipe-pane, then sending the command ensures output is
# always captured regardless of command duration.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
PTY="$PROJECT_ROOT/bin/pty-session"

export NBS_PTY_QUIET=1

ERRORS=0

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

cleanup() {
    "$PTY" kill test-oneshot 2>/dev/null || true
    "$PTY" kill test-notfound 2>/dev/null || true
    "$PTY" kill test-interactive 2>/dev/null || true
    "$PTY" kill test-logcheck 2>/dev/null || true
    rm -f ~/.pty-session/logs/test-oneshot.log
    rm -f ~/.pty-session/logs/test-notfound.log
    rm -f ~/.pty-session/logs/test-interactive.log
    rm -f ~/.pty-session/logs/test-logcheck.log
}
trap cleanup EXIT

# Precondition
if [[ ! -x "$PTY" ]]; then
    echo "FATAL: pty-session not found at $PTY"
    exit 1
fi

echo "=== pty-session create lifecycle tests ==="
echo ""

# Clean any leftover sessions
cleanup 2>/dev/null

# --- Test 1: Fast one-shot command, output captured ---
echo "1. Fast one-shot command..."
"$PTY" create test-oneshot 'echo ONESHOT_MARKER_12345' >/dev/null 2>&1
sleep 2

WAIT_RC=0
"$PTY" wait test-oneshot 'ONESHOT_MARKER_12345' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?
if [[ $WAIT_RC -eq 0 ]]; then
    check "1. wait finds pattern from one-shot command" "pass"
else
    check "1. wait finds pattern from one-shot command (rc=$WAIT_RC)" "fail"
fi

# --- Test 2: Command not found, session exits ---
echo "2. Command not found..."
"$PTY" create test-notfound 'nonexistent_command_xyz_98765' >/dev/null 2>&1
sleep 2

# Session should have exited — check it's not still running
LIST_OUTPUT=$("$PTY" list 2>/dev/null)
if echo "$LIST_OUTPUT" | grep -q 'test-notfound.*running'; then
    check "2. command-not-found session exits (not orphaned)" "fail"
else
    check "2. command-not-found session exits (not orphaned)" "pass"
fi

# --- Test 3: Interactive shell, send/wait/read ---
echo "3. Interactive shell..."
"$PTY" create test-interactive 'bash' >/dev/null 2>&1
sleep 1

"$PTY" send test-interactive 'echo INTERACTIVE_MARKER_67890' >/dev/null 2>&1

WAIT_RC=0
"$PTY" wait test-interactive 'INTERACTIVE_MARKER_67890' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?
if [[ $WAIT_RC -eq 0 ]]; then
    check "3. interactive send/wait finds pattern" "pass"
else
    check "3. interactive send/wait finds pattern (rc=$WAIT_RC)" "fail"
fi

READ_OUTPUT=$("$PTY" read test-interactive 2>/dev/null)
if echo "$READ_OUTPUT" | grep -q 'INTERACTIVE_MARKER_67890'; then
    check "3. read shows sent output" "pass"
else
    check "3. read shows sent output" "fail"
fi

"$PTY" kill test-interactive >/dev/null 2>&1

# --- Test 4: Session dies after one-shot completes ---
echo "4. Session lifecycle..."
# test-oneshot from test 1 should already be exited
LIST_OUTPUT=$("$PTY" list 2>/dev/null)
if echo "$LIST_OUTPUT" | grep -q 'test-oneshot.*running'; then
    check "4. one-shot session exits after command completes" "fail"
else
    check "4. one-shot session exits after command completes" "pass"
fi

# --- Test 5: Log file non-empty for successful command ---
echo "5. Log capture..."
LOG_FILE="$HOME/.pty-session/logs/test-oneshot.log"
if [[ -f "$LOG_FILE" ]] && [[ -s "$LOG_FILE" ]]; then
    if grep -q 'ONESHOT_MARKER_12345' "$LOG_FILE"; then
        check "5. log file captures output from one-shot command" "pass"
    else
        check "5. log file exists but missing marker" "fail"
    fi
else
    check "5. log file non-empty for successful command (file missing or empty)" "fail"
fi

# --- Test 6: Log file non-empty for failed command ---
echo "6. Failed command log..."
"$PTY" create test-logcheck 'echo LOG_BEFORE_FAIL && nonexistent_xyz' >/dev/null 2>&1
sleep 2

LOG_FILE="$HOME/.pty-session/logs/test-logcheck.log"
if [[ -f "$LOG_FILE" ]] && [[ -s "$LOG_FILE" ]]; then
    check "6. log file non-empty for failed command" "pass"
else
    check "6. log file non-empty for failed command (file missing or empty)" "fail"
fi

echo ""
echo "=== Results ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "FAILURES: $ERRORS"
    exit 1
fi
