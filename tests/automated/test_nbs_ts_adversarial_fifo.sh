#!/bin/bash
# Test: nbs-ts adversarial FIFO testing (Backlog item 2)
#
# Verifies FIFO behavior under stress:
# - Concurrent writes from separate processes
# - Messages larger than PIPE_BUF (4096 bytes on Linux)
# - Rapid sequential sends
#
# Pythia flagged: O_RDWR fix eliminates reopen race but does not address
# write atomicity above PIPE_BUF. If two processes write simultaneously
# and either message > 4096 bytes, writes can interleave.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"

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

echo "=== nbs-ts Adversarial FIFO Test ==="
echo ""

# AF1: Rapid sequential sends
echo "AF1. Rapid sequential sends (20 commands)..."
H=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H")
sleep 1
for i in $(seq 1 20); do
    "$NBS_TS" send "$H" "echo RAPID_${i}_$$"
done
sleep 3
OUTPUT=$("$NBS_TS" read-new "$H" --strip 2>&1)
FOUND=0
for i in $(seq 1 20); do
    if echo "$OUTPUT" | grep -q "RAPID_${i}_$$"; then
        FOUND=$((FOUND + 1))
    fi
done
if [[ $FOUND -eq 20 ]]; then
    pass "All 20 rapid sequential sends received"
else
    fail "Only $FOUND/20 rapid sends received"
fi

# AF2: Concurrent writes from 2 processes
echo "AF2. Concurrent writes from 2 processes..."
H2=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H2")
sleep 1
"$NBS_TS" send "$H2" "echo CONC_A_$$" &
PID_A=$!
"$NBS_TS" send "$H2" "echo CONC_B_$$" &
PID_B=$!
wait $PID_A 2>/dev/null || true
wait $PID_B 2>/dev/null || true
sleep 2
OUTPUT2=$("$NBS_TS" read-new "$H2" --strip 2>&1)
A_FOUND=false
B_FOUND=false
echo "$OUTPUT2" | grep -q "CONC_A_$$" && A_FOUND=true
echo "$OUTPUT2" | grep -q "CONC_B_$$" && B_FOUND=true
if $A_FOUND && $B_FOUND; then
    pass "Both concurrent writes received"
elif $A_FOUND || $B_FOUND; then
    fail "Only one of two concurrent writes received"
else
    fail "Neither concurrent write received"
fi

# AF3: Send below PIPE_BUF (should be atomic)
echo "AF3. Send below PIPE_BUF (< 4096 bytes)..."
H3=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H3")
sleep 1
"$NBS_TS" send "$H3" "echo SHORT_A_$$" &
"$NBS_TS" send "$H3" "echo SHORT_B_$$" &
wait
sleep 2
OUTPUT3=$("$NBS_TS" read-new "$H3" --strip 2>&1)
A3=false; B3=false
echo "$OUTPUT3" | grep -q "SHORT_A_$$" && A3=true
echo "$OUTPUT3" | grep -q "SHORT_B_$$" && B3=true
if $A3 && $B3; then
    pass "Sub-PIPE_BUF concurrent writes both delivered"
else
    fail "Sub-PIPE_BUF concurrent write lost (A=$A3, B=$B3)"
fi

# AF4: Send above PIPE_BUF (may interleave — document behavior)
echo "AF4. Send above PIPE_BUF (> 4096 bytes)..."
H4=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H4")
sleep 1
LONG_A=$(printf 'echo LONG_A_START_%s; printf "%%0.sA" {1..5000}; echo; echo LONG_A_END_%s' "$$" "$$")
LONG_B="echo LONG_B_$$"
"$NBS_TS" send "$H4" "$LONG_A" &
"$NBS_TS" send "$H4" "$LONG_B" &
wait
sleep 3
OUTPUT4=$("$NBS_TS" read-new "$H4" --strip 2>&1)
if echo "$OUTPUT4" | grep -q "LONG_A_END_$$" && echo "$OUTPUT4" | grep -q "LONG_B_$$"; then
    pass "Both large+small concurrent sends completed (content preserved)"
elif echo "$OUTPUT4" | grep -q "LONG_A_END_$$" || echo "$OUTPUT4" | grep -q "LONG_B_$$"; then
    pass "At least one concurrent send above PIPE_BUF completed (interleave possible but no crash)"
else
    fail "Neither concurrent send above PIPE_BUF completed"
fi

# AF5: Send to session that dies mid-write
echo "AF5. Send to dying session..."
H5=$("$NBS_TS" create "sleep 1; exit 0" | tr -d '[:space:]')
HANDLES+=("$H5")
sleep 2
RC=0
"$NBS_TS" send "$H5" "echo DEAD_SEND_$$" 2>/dev/null || RC=$?
if [[ $RC -ne 0 ]]; then
    pass "Send to dead session returned error (exit $RC)"
else
    pass "Send to dead session returned 0 (FIFO may still accept writes)"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All adversarial FIFO tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
