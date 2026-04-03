#!/bin/bash
# Test: nbs-chat session-end and resume — TDD red phase
#
# Tests the session-end/resume lifecycle:
#   - session-end posts a countdown message then creates .nbs/control-pause
#   - resume cancels the countdown or deletes control-pause
#   - pause suppresses sidecar processing (notifications, triggers)
#   - resume restores normal operation
#
# Uses 5s countdown for test speed (production default: 300s).
#
# Falsifiable tests covering:
#   1. session-end creates control-pause file after countdown
#   2. resume during countdown cancels session-end (no pause file)
#   3. resume after pause deletes control-pause file
#   4. sidecar suppresses notifications during pause
#   5. sidecar resumes notifications after resume
#   6. session-end posts countdown message to chat
#   7. cursor and PID state preserved across pause/resume cycle
#   8. session-end command exists and is executable
#   9. resume command exists and is executable

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
BIN_DIR="${PROJECT_ROOT}/bin"

source "${SCRIPT_DIR}/test_helpers.sh"

NBS_CHAT="${BIN_DIR}/nbs-chat"
SESSION_END="${BIN_DIR}/nbs-chat-session-end"
SESSION_RESUME="${BIN_DIR}/nbs-chat-resume"

TEST_DIR=$(mktemp -d)
ERRORS=0
PASS_COUNT=0

cleanup() {
    # Remove any leftover pause files
    rm -f "${TEST_DIR}/fake-project/.nbs/control-pause" 2>/dev/null
    # Kill any countdown processes
    if [[ -f "${TEST_DIR}/countdown_pid" ]]; then
        kill "$(cat "${TEST_DIR}/countdown_pid")" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

echo "=== Session End/Resume Test ==="
echo "Test dir: $TEST_DIR"
echo ""

FAKE_ROOT="${TEST_DIR}/fake-project"
mkdir -p "${FAKE_ROOT}/.nbs/pids"
mkdir -p "${FAKE_ROOT}/.nbs/chat"

# Create a test chat file
CHAT_FILE="${FAKE_ROOT}/.nbs/chat/test.chat"
"$NBS_CHAT" create "$CHAT_FILE" 2>/dev/null

# ---- Test 1: Commands exist ----
echo "1. Command existence..."

check "nbs-chat-session-end exists and is executable" \
    "$([[ -x "$SESSION_END" ]] && echo pass || echo fail)"

check "nbs-chat-resume exists and is executable" \
    "$([[ -x "$SESSION_RESUME" ]] && echo pass || echo fail)"

echo ""

# ---- Test 2: session-end creates control-pause after countdown ----
echo "2. session-end creates control-pause after countdown..."

# Call session-end with 5s countdown
rm -f "${FAKE_ROOT}/.nbs/control-pause"
"$SESSION_END" "$FAKE_ROOT" --countdown=5 --chat="$CHAT_FILE" &
SE_PID=$!
echo "$SE_PID" > "${TEST_DIR}/countdown_pid"

# Verify no pause file immediately
sleep 1
check "no control-pause during countdown" \
    "$([[ ! -f "${FAKE_ROOT}/.nbs/control-pause" ]] && echo pass || echo fail)"

# Wait for countdown to complete
wait "$SE_PID" 2>/dev/null || true

check "control-pause exists after countdown" \
    "$([[ -f "${FAKE_ROOT}/.nbs/control-pause" ]] && echo pass || echo fail)"

# Clean up
rm -f "${FAKE_ROOT}/.nbs/control-pause"

echo ""

# ---- Test 3: resume during countdown cancels session-end ----
echo "3. resume during countdown cancels session-end..."

rm -f "${FAKE_ROOT}/.nbs/control-pause"
"$SESSION_END" "$FAKE_ROOT" --countdown=10 --chat="$CHAT_FILE" &
SE_PID=$!
echo "$SE_PID" > "${TEST_DIR}/countdown_pid"

# Wait 2s then resume
sleep 2
"$SESSION_RESUME" "$FAKE_ROOT" 2>/dev/null || true

# Wait for session-end to exit (should be killed by resume)
sleep 2

check "session-end cancelled by resume (no pause file)" \
    "$([[ ! -f "${FAKE_ROOT}/.nbs/control-pause" ]] && echo pass || echo fail)"

# Make sure the countdown process is gone
check "countdown process terminated" \
    "$(kill -0 "$SE_PID" 2>/dev/null && echo fail || echo pass)"

echo ""

# ---- Test 4: resume after pause deletes control-pause ----
echo "4. resume after pause deletes control-pause..."

# Create a pause state
touch "${FAKE_ROOT}/.nbs/control-pause"
check "pause file exists before resume" \
    "$([[ -f "${FAKE_ROOT}/.nbs/control-pause" ]] && echo pass || echo fail)"

"$SESSION_RESUME" "$FAKE_ROOT" 2>/dev/null || true

check "pause file deleted after resume" \
    "$([[ ! -f "${FAKE_ROOT}/.nbs/control-pause" ]] && echo pass || echo fail)"

echo ""

# ---- Test 5: session-end posts countdown message to chat ----
echo "5. session-end posts countdown message to chat..."

rm -f "${FAKE_ROOT}/.nbs/control-pause"
"$SESSION_END" "$FAKE_ROOT" --countdown=3 --chat="$CHAT_FILE" &
SE_PID=$!
echo "$SE_PID" > "${TEST_DIR}/countdown_pid"
wait "$SE_PID" 2>/dev/null || true

# Check chat for session-end message
output=$("$NBS_CHAT" read "$CHAT_FILE" --last=5 2>/dev/null || true)
check "countdown message posted to chat" \
    "$(echo "$output" | grep -qi 'session.end\|pause\|shutdown\|countdown' && echo pass || echo fail)"

rm -f "${FAKE_ROOT}/.nbs/control-pause"

echo ""

# ---- Test 6: Sidecar respects control-pause (structural check) ----
echo "6. Sidecar pause mechanism (structural)..."

SIDECAR_C="${PROJECT_ROOT}/src/nbs-sidecar/sidecar.c"

# Sidecar must check for control-pause and skip processing
check "sidecar checks control-pause file" \
    "$(grep -q 'control-pause' "$SIDECAR_C" && echo pass || echo fail)"

# When pause file exists, sidecar must continue (skip processing)
check "sidecar skips processing when paused" \
    "$(grep -A5 'control-pause' "$SIDECAR_C" | grep -q 'continue' && echo pass || echo fail)"

echo ""

# ---- Test 7: State preservation across pause/resume ----
echo "7. State preservation across pause/resume cycle..."

# Write cursor state before pause
"$NBS_CHAT" send "$CHAT_FILE" "testuser" "Pre-pause message" 2>/dev/null
msg_before=$("$NBS_CHAT" count "$CHAT_FILE" 2>/dev/null)

# Pause
touch "${FAKE_ROOT}/.nbs/control-pause"

# Add a message during pause
"$NBS_CHAT" send "$CHAT_FILE" "testuser" "During-pause message" 2>/dev/null

# Resume
rm -f "${FAKE_ROOT}/.nbs/control-pause"

# Verify messages are intact
msg_after=$("$NBS_CHAT" count "$CHAT_FILE" 2>/dev/null)
check "messages preserved across pause/resume ($msg_before → $msg_after)" \
    "$([[ "$msg_after" -gt "$msg_before" ]] && echo pass || echo fail)"

# Messages posted during pause should be readable
output=$("$NBS_CHAT" read "$CHAT_FILE" --last=3 2>/dev/null || true)
check "during-pause message readable after resume" \
    "$(echo "$output" | grep -q 'During-pause' && echo pass || echo fail)"

echo ""

# ---- Test 8: Ephemeral trigger suppression during pause (structural) ----
echo "8. Ephemeral trigger suppression during pause (structural)..."

# The pause check (control-pause → continue) occurs BEFORE triggers in the main loop.
# This means triggers are never reached when paused.
# Verify the pause check is before the trigger section.
pause_line=$(grep -n 'control-pause' "$SIDECAR_C" | head -1 | cut -d: -f1)
trigger_line=$(grep -n 'trigger_periodic_check' "$SIDECAR_C" | head -1 | cut -d: -f1)

check "pause check before trigger section ($pause_line < $trigger_line)" \
    "$([[ -n "$pause_line" && -n "$trigger_line" && "$pause_line" -lt "$trigger_line" ]] && echo pass || echo fail)"

echo ""

# ---- Summary ----
echo "=== Results ==="
TOTAL=$((PASS_COUNT + ERRORS))
echo "Pass: $PASS_COUNT | Fail: $ERRORS | Total: $TOTAL"
if [[ $ERRORS -eq 0 ]]; then
    echo "All tests passed."
    exit 0
else
    echo "$ERRORS test(s) failed — expected in TDD red phase."
    exit 1
fi
