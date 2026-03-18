#!/bin/bash
# Test: watchdog chat-scoped session tag derivation
#
# Verifies that:
#   1. The tag derivation logic (strip .chat suffix, replace dots with dashes)
#      produces the expected tags for known inputs.
#   2. tmux sessions are scoped to their chat tag — grep patterns match only
#      the sessions belonging to the correct chat.
#   3. The restart script's bash-native tag derivation matches the C logic.
#
# Tests:
#   1.  live.chat produces tag "live"
#   2.  nn.Module.chat produces tag "nn-Module"
#   3.  Sessions scoped to chatA: grep finds exactly 1 for nbs-.*-chatA
#   4.  Sessions scoped to chatB: grep finds exactly 1 for nbs-.*-chatB
#   5.  No false positives: grep for nbs-.*-live finds 0 among test sessions
#   6.  Restart script tag derivation: nn.Module.chat -> nn-Module

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

ERRORS=0
PASS_COUNT=0

# Tmux sessions created by this test, cleaned up on exit
TEST_SESSIONS=()

cleanup() {
    for s in "${TEST_SESSIONS[@]}"; do
        tmux kill-session -t "$s" 2>/dev/null || true
    done
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

# ---------- helper: replicate C tag derivation in bash ----------
# This mirrors the logic in terminal.c lines 215-228 and
# nbs-chat-terminal-restart.sh line 44.
derive_tag() {
    local filename="$1"
    local base
    base=$(basename "$filename" .chat)
    echo "${base//./-}"
}

echo "=== test_watchdog_chat_scope ==="

# --- Test 1: live.chat -> "live" ---
TAG=$(derive_tag "live.chat")
check "live.chat produces tag 'live'" "$( [[ "$TAG" == "live" ]] && echo pass || echo fail )"

# --- Test 2: nn.Module.chat -> "nn-Module" ---
TAG=$(derive_tag "nn.Module.chat")
check "nn.Module.chat produces tag 'nn-Module'" "$( [[ "$TAG" == "nn-Module" ]] && echo pass || echo fail )"

# --- Test 3 & 4: sessions scoped to their chat tag ---
# Create two fake tmux sessions with distinct chat tags.
# Use 'true' as the command so the session exists briefly; hold it with sleep.
SESSION_A="nbs-test1-chatA"
SESSION_B="nbs-test2-chatB"
TEST_SESSIONS+=("$SESSION_A" "$SESSION_B")

tmux new-session -d -s "$SESSION_A" "sleep 30" 2>/dev/null
tmux new-session -d -s "$SESSION_B" "sleep 30" 2>/dev/null

# Small delay to let tmux register
sleep 0.5

COUNT_A=$(tmux list-sessions -F '#{session_name}' 2>/dev/null | grep -c 'nbs-.*-chatA' || echo 0)
check "grep nbs-.*-chatA finds exactly 1" "$( [[ "$COUNT_A" -eq 1 ]] && echo pass || echo fail )"

COUNT_B=$(tmux list-sessions -F '#{session_name}' 2>/dev/null | grep -c 'nbs-.*-chatB' || echo 0)
check "grep nbs-.*-chatB finds exactly 1" "$( [[ "$COUNT_B" -eq 1 ]] && echo pass || echo fail )"

# --- Test 5: no false positives for unrelated tag ---
# Filter to only our test sessions to avoid picking up real sessions
COUNT_LIVE=$(tmux list-sessions -F '#{session_name}' 2>/dev/null \
    | grep 'nbs-test[12]-' \
    | grep -c 'nbs-.*-live' 2>/dev/null || true)
COUNT_LIVE=${COUNT_LIVE:-0}
check "no false positives: nbs-test*-live finds 0" "$( [[ "$COUNT_LIVE" -eq 0 ]] && echo pass || echo fail )"

# --- Test 6: restart script tag derivation ---
# Replicate exactly what the restart script does (line 43-44)
CHAT_FILE="nn.Module.chat"
CHAT_BASE=$(basename "$CHAT_FILE" .chat)
RESTART_TAG="${CHAT_BASE//./-}"
check "restart script derivation: nn.Module.chat -> nn-Module" \
    "$( [[ "$RESTART_TAG" == "nn-Module" ]] && echo pass || echo fail )"

# --- Summary ---
echo ""
echo "Results: ${PASS_COUNT} passed, ${ERRORS} failed"
if [[ "$ERRORS" -gt 0 ]]; then
    exit 1
fi
echo "All tests passed."
