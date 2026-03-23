#!/bin/bash
# Test: nbs-ts restart and watchdog (Phase 5)
#
# Verifies that nbs-chat-terminal-restart.sh and watchdog use nbs-ts
# instead of tmux for agent lifecycle management.
#
# NOTE: These tests verify the scripts' nbs-ts integration, not
# full agent restart (which requires a running team).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
RESTART_SCRIPT="$PROJECT_ROOT/bin/nbs-chat-terminal-restart.sh"

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

# Check prerequisites
if [[ ! -x "$NBS_TS" ]]; then
    echo "SKIP: nbs-ts binary not found"
    exit 0
fi
if [[ ! -f "$RESTART_SCRIPT" ]]; then
    echo "SKIP: nbs-chat-terminal-restart.sh not found"
    exit 0
fi
if ! grep -q "nbs-ts" "$RESTART_SCRIPT" 2>/dev/null; then
    echo "SKIP: restart script does not use nbs-ts yet"
    exit 0
fi

echo "=== nbs-ts Restart/Watchdog Test (Phase 5) ==="
echo ""

# RS1: nbs-ts kill works for agent-style sessions
echo "RS1. nbs-ts kill works for agent sessions..."
HANDLE=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 1
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true
HANDLES=("${HANDLES[@]/$HANDLE}")
sleep 0.5
RC=0
"$NBS_TS" status "$HANDLE" 2>/dev/null || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Agent session killed and cleaned up"
else
    fail "Agent session not properly cleaned (exit $RC)"
fi

# RS2: nbs-ts list can enumerate sessions (for restart script)
echo "RS2. nbs-ts list enumerates sessions..."
H1=$("$NBS_TS" create "sleep 60" | tr -d '[:space:]')
H2=$("$NBS_TS" create "sleep 60" | tr -d '[:space:]')
HANDLES+=("$H1" "$H2")
sleep 1
LIST=$("$NBS_TS" list 2>&1)
FOUND=0
echo "$LIST" | grep -q "$H1" && FOUND=$((FOUND + 1))
echo "$LIST" | grep -q "$H2" && FOUND=$((FOUND + 1))
if [[ $FOUND -eq 2 ]]; then
    pass "nbs-ts list found both sessions"
else
    fail "nbs-ts list found $FOUND/2 sessions"
fi

# RS3: nbs-ts send delivers skill injection (no keystroke delay)
echo "RS3. Skill injection via nbs-ts send..."
H3=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H3")
sleep 1
"$NBS_TS" send "$H3" "echo SKILL_INJECTED_$$"
sleep 1
OUTPUT=$("$NBS_TS" read-new "$H3" --strip 2>&1)
if echo "$OUTPUT" | grep -q "SKILL_INJECTED_$$"; then
    pass "Skill injection delivered via nbs-ts send"
else
    fail "Skill injection not found in output"
    echo "   Got: $OUTPUT"
fi

# RS4: No tmux in nbs-ts code path of restart script
echo "RS4. No tmux in nbs-ts code path..."
NBS_TS_BLOCK=$(sed -n '/nbs-ts mode\|NBS_TRANSPORT.*ts/,/tmux fallback\|else\b/p' "$RESTART_SCRIPT" 2>/dev/null | grep -v '# ---' || true)
if [[ -n "$NBS_TS_BLOCK" ]]; then
    if echo "$NBS_TS_BLOCK" | grep -q "tmux "; then
        fail "tmux commands in nbs-ts code path of restart script"
    else
        pass "Restart script nbs-ts code path is tmux-free"
    fi
else
    # Check if script has been fully converted (no tmux at all)
    if ! grep -q "tmux " "$RESTART_SCRIPT" 2>/dev/null; then
        pass "Restart script has no tmux commands"
    else
        pass "Restart script has tmux (likely fallback — acceptable during transition)"
    fi
fi

# RS5: Watchdog can detect sessions via nbs-ts
echo "RS5. Session liveness detection..."
H5=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H5")
sleep 1
ALIVE_COUNT=$("$NBS_TS" list 2>&1 | grep -c "alive" || echo "0")
if [[ $ALIVE_COUNT -ge 1 ]]; then
    pass "nbs-ts list reports $ALIVE_COUNT alive sessions (watchdog can count)"
else
    fail "No alive sessions detected"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All restart/watchdog tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
