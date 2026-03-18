#!/bin/bash
# Test: pty-session fence marker protocol
#
# Falsifiable claims:
#   1. Fence prevents command echo match: wait for a slow command's output
#      takes ~command duration, not 0s (proving fence skips past echo line)
#   2. Fence works with fast command: send+wait on instant output succeeds
#   3. Fence unique per send: second send's fence replaces first, so wait
#      only matches output after the second command
#   4. No fence file means full-pane search (backwards compat): without
#      send, wait searches entire pane content
#
# The fence protocol: `send` prepends `echo __PTY_FENCE_<guid>__` to every
# command and writes the marker to ~/.pty-session/fence/<session>. `wait`
# reads the fence file and only matches the pattern AFTER the fence marker
# output in the pane. If no fence file exists, the entire pane is searched.

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
    "$PTY" kill test-fence-slow 2>/dev/null || true
    "$PTY" kill test-fence-fast 2>/dev/null || true
    "$PTY" kill test-fence-unique 2>/dev/null || true
    "$PTY" kill test-fence-compat 2>/dev/null || true
    rm -f ~/.pty-session/logs/test-fence-slow.log
    rm -f ~/.pty-session/logs/test-fence-fast.log
    rm -f ~/.pty-session/logs/test-fence-unique.log
    rm -f ~/.pty-session/logs/test-fence-compat.log
    rm -f ~/.pty-session/fence/test-fence-slow
    rm -f ~/.pty-session/fence/test-fence-fast
    rm -f ~/.pty-session/fence/test-fence-unique
    rm -f ~/.pty-session/fence/test-fence-compat
}
trap cleanup EXIT

# Precondition
if [[ ! -x "$PTY" ]]; then
    echo "FATAL: pty-session not found at $PTY"
    exit 1
fi

echo "=== pty-session fence marker protocol tests ==="
echo ""

# Clean any leftover sessions
cleanup 2>/dev/null

# --- Test 1: Fence prevents command echo match ---
echo "1. Fence prevents command echo match..."
"$PTY" create test-fence-slow 'bash' >/dev/null 2>&1
sleep 1

# Send a command that echoes MARKER after a 2-second delay.
# Without the fence, wait would immediately match the echo line
# (the command text itself: "sleep 2; echo MARKER") visible in the pane.
# With the fence, wait only searches after the fence output, so it must
# wait for the actual command output (~2 seconds).
"$PTY" send test-fence-slow 'sleep 2; echo FENCE_SLOW_MARKER' >/dev/null 2>&1

START=$(date +%s%N)
WAIT_RC=0
"$PTY" wait test-fence-slow 'FENCE_SLOW_MARKER' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?
END=$(date +%s%N)

ELAPSED_MS=$(( (END - START) / 1000000 ))

if [[ $WAIT_RC -eq 0 ]] && [[ $ELAPSED_MS -ge 1000 ]]; then
    check "1. fence prevents echo match (wait took ${ELAPSED_MS}ms >= 1000ms)" "pass"
elif [[ $WAIT_RC -ne 0 ]]; then
    check "1. fence prevents echo match (wait failed rc=$WAIT_RC)" "fail"
else
    check "1. fence prevents echo match (wait took ${ELAPSED_MS}ms < 1000ms, fence likely broken)" "fail"
fi

# --- Test 2: Fence works with fast command ---
echo "2. Fence works with fast command..."
"$PTY" create test-fence-fast 'bash' >/dev/null 2>&1
sleep 1

"$PTY" send test-fence-fast 'echo FAST_RESULT' >/dev/null 2>&1

WAIT_RC=0
"$PTY" wait test-fence-fast 'FAST_RESULT' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?

if [[ $WAIT_RC -eq 0 ]]; then
    check "2. fast command output found through fence" "pass"
else
    check "2. fast command output found through fence (rc=$WAIT_RC)" "fail"
fi

# --- Test 3: Fence unique per send ---
echo "3. Fence unique per send..."
"$PTY" create test-fence-unique 'bash' >/dev/null 2>&1
sleep 1

# First command: produce FIRST_OUTPUT
"$PTY" send test-fence-unique 'echo FIRST_OUTPUT' >/dev/null 2>&1
WAIT_RC=0
"$PTY" wait test-fence-unique 'FIRST_OUTPUT' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?

if [[ $WAIT_RC -ne 0 ]]; then
    check "3a. first command output found" "fail"
else
    check "3a. first command output found" "pass"
fi

# Second command: produce SECOND_OUTPUT after a delay.
# The second send writes a new fence, so wait should only match
# output after the second fence — it must wait for the actual output.
"$PTY" send test-fence-unique 'sleep 1; echo SECOND_OUTPUT' >/dev/null 2>&1

# Try to wait for FIRST_OUTPUT — this should timeout because the new
# fence is past FIRST_OUTPUT in the pane. Use a short timeout.
WAIT_RC=0
"$PTY" wait test-fence-unique 'FIRST_OUTPUT' --timeout=3 >/dev/null 2>&1 || WAIT_RC=$?

if [[ $WAIT_RC -ne 0 ]]; then
    check "3b. second fence hides first output (wait timed out as expected)" "pass"
else
    check "3b. second fence hides first output (matched old output, fence not unique)" "fail"
fi

# Wait for the second command's actual output
WAIT_RC=0
"$PTY" wait test-fence-unique 'SECOND_OUTPUT' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?

if [[ $WAIT_RC -eq 0 ]]; then
    check "3c. second command output found after its fence" "pass"
else
    check "3c. second command output found after its fence (rc=$WAIT_RC)" "fail"
fi

# --- Test 4: No fence file means full-pane search (backwards compat) ---
echo "4. No fence file, full-pane search..."
"$PTY" create test-fence-compat 'bash' >/dev/null 2>&1
sleep 1

# Manually inject text into the pane via tmux send-keys (bypassing pty-session send,
# so no fence file is created).
tmux send-keys -t pty_test-fence-compat 'echo COMPAT_MARKER_XYZ' Enter 2>/dev/null
sleep 1

# Ensure no fence file exists for this session
rm -f ~/.pty-session/fence/test-fence-compat

WAIT_RC=0
"$PTY" wait test-fence-compat 'COMPAT_MARKER_XYZ' --timeout=10 >/dev/null 2>&1 || WAIT_RC=$?

if [[ $WAIT_RC -eq 0 ]]; then
    check "4. full-pane search without fence file" "pass"
else
    check "4. full-pane search without fence file (rc=$WAIT_RC)" "fail"
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
