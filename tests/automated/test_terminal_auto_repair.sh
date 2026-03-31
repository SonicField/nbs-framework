#!/bin/bash
# Test: nbs-chat-terminal auto-repair integration
#
# Verifies that the terminal automatically detects and repairs corrupt
# chat files via the poll loop. Uses nbs-ts to run the terminal in a
# PTY session and observe its behaviour.
#
# Tests:
#   1. Corruption present at startup — repair triggers automatically
#   2. No corruption — repair does NOT trigger
#   3. New corruption injected mid-session — repair triggers again

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_TS_RENDER="$(command -v nbs-ts-render)"

export PATH="$PROJECT_ROOT/bin:$PATH"

TEST_DIR=$(mktemp -d)
HANDLES=()
PASS=0
FAIL=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $label"
        FAIL=$((FAIL + 1))
    fi
}

# Helper: get rendered terminal output (strips ANSI/PTY control sequences)
rendered_output() {
    local handle="$1"
    "$NBS_TS" read "$handle" 2>/dev/null | "$NBS_TS_RENDER" 2>/dev/null
}

echo "=== nbs-chat-terminal Auto-Repair Integration Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Corruption at startup triggers auto-repair ---
echo "1. Corruption at startup triggers auto-repair..."

CHAT="$TEST_DIR/t1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid message before corruption" >/dev/null 2>&1
# Inject raw text (corrupt line)
printf '%s\n' "@supervisor I broke the chat format" >> "$CHAT"
"$NBS_CHAT" send "$CHAT" bob "valid message after corruption" >/dev/null 2>&1

# Launch terminal in nbs-ts session
HANDLE=$("$NBS_TS" create nbs-chat-terminal "$CHAT" test-viewer --no-restart 2>&1 | tr -d '[:space:]')
HANDLES+=("$HANDLE")

# Wait for the terminal to start and the poll loop to detect corruption
sleep 4

# Check terminal output for the INFO repair line
OUTPUT=$(rendered_output "$HANDLE")
check "INFO line shows repair detected" \
    "$( echo "$OUTPUT" | grep -qF 'corrupt line' && echo pass || echo fail )"

# Check the chat file — recovery message should be appended
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Recovery message appended to chat" \
    "$( echo "$CHAT_OUTPUT" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"
check "Recovered text contains original" \
    "$( echo "$CHAT_OUTPUT" | grep -qF '@supervisor I broke the chat format' && echo pass || echo fail )"
check "Valid messages preserved" \
    "$( echo "$CHAT_OUTPUT" | grep -qF 'valid message before corruption' && echo pass || echo fail )"

# Clean up session
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
HANDLES=("${HANDLES[@]:0:${#HANDLES[@]}-1}")
echo ""

# --- Test 2: No corruption — repair does NOT trigger ---
echo "2. No corruption — repair does NOT trigger..."

CHAT="$TEST_DIR/t2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "clean message one" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" bob "clean message two" >/dev/null 2>&1

MSGS_BEFORE=$("$NBS_CHAT" read "$CHAT" 2>/dev/null | grep -c '^\[' || true)

HANDLE=$("$NBS_TS" create nbs-chat-terminal "$CHAT" test-viewer --no-restart 2>&1 | tr -d '[:space:]')
HANDLES+=("$HANDLE")

sleep 4

OUTPUT=$(rendered_output "$HANDLE")
MSGS_AFTER=$("$NBS_CHAT" read "$CHAT" 2>/dev/null | grep -c '^\[' || true)

check "No repair INFO line" \
    "$( echo "$OUTPUT" | grep -qF 'corrupt' && echo fail || echo pass )"
check "No recovery message appended" \
    "$( [[ "$MSGS_BEFORE" -eq "$MSGS_AFTER" ]] && echo pass || echo fail )"

"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
HANDLES=("${HANDLES[@]:0:${#HANDLES[@]}-1}")
echo ""

# --- Test 3: New corruption mid-session triggers second repair ---
echo "3. New corruption mid-session triggers second repair..."

CHAT="$TEST_DIR/t3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "initial message" >/dev/null 2>&1
# Start with one corrupt line
printf '%s\n' "first corruption" >> "$CHAT"

HANDLE=$("$NBS_TS" create nbs-chat-terminal "$CHAT" test-viewer --no-restart 2>&1 | tr -d '[:space:]')
HANDLES+=("$HANDLE")

# Wait for first repair
sleep 4

CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
FIRST_REPAIR_COUNT=$(echo "$CHAT_OUTPUT" | grep -c '\[AUTO-REPAIR\]' || true)
check "First repair completed" \
    "$( [[ "$FIRST_REPAIR_COUNT" -ge 1 ]] && echo pass || echo fail )"

# Inject NEW corruption mid-session
printf '%s\n' "second corruption added later" >> "$CHAT"

# Wait for second repair to trigger
sleep 4

CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
SECOND_REPAIR_COUNT=$(echo "$CHAT_OUTPUT" | grep -c '\[AUTO-REPAIR\]' || true)
check "Second repair triggered for new corruption" \
    "$( [[ "$SECOND_REPAIR_COUNT" -ge 2 ]] && echo pass || echo fail )"
check "Second corruption text recovered" \
    "$( echo "$CHAT_OUTPUT" | grep -qF 'second corruption added later' && echo pass || echo fail )"

"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
HANDLES=("${HANDLES[@]:0:${#HANDLES[@]}-1}")
echo ""

# --- Test 4: Terminal restart after previous repair does NOT re-trigger ---
echo "4. Terminal restart after previous repair — no re-trigger..."

CHAT="$TEST_DIR/t4.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "message before corruption" >/dev/null 2>&1
printf '%s\n' "corrupt text that will be repaired" >> "$CHAT"
"$NBS_CHAT" send "$CHAT" bob "message after corruption" >/dev/null 2>&1

# First terminal session — repairs the corruption
HANDLE=$("$NBS_TS" create nbs-chat-terminal "$CHAT" test-viewer --no-restart 2>&1 | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 4

CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "First session repaired" \
    "$( echo "$CHAT_OUTPUT" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"

REPAIR_COUNT_FIRST=$(echo "$CHAT_OUTPUT" | grep -c '\[AUTO-REPAIR\]' || true)

"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
HANDLES=("${HANDLES[@]:0:${#HANDLES[@]}-1}")

# Second terminal session — should NOT re-trigger repair
HANDLE=$("$NBS_TS" create nbs-chat-terminal "$CHAT" test-viewer --no-restart 2>&1 | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 4

OUTPUT=$(rendered_output "$HANDLE")
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
REPAIR_COUNT_SECOND=$(echo "$CHAT_OUTPUT" | grep -c '\[AUTO-REPAIR\]' || true)

check "No repair INFO line on restart" \
    "$( echo "$OUTPUT" | grep -qF 'running auto-repair' && echo fail || echo pass )"
check "No new recovery message after restart" \
    "$( [[ "$REPAIR_COUNT_FIRST" -eq "$REPAIR_COUNT_SECOND" ]] && echo pass || echo fail )"

"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
HANDLES=("${HANDLES[@]:0:${#HANDLES[@]}-1}")
echo ""

# --- Summary ---
echo "================================"
echo "Results: $PASS passed, $FAIL failed out of $((PASS + FAIL))"
echo "================================"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
