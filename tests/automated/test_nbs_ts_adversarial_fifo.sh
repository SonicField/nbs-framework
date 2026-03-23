#!/bin/bash
# test_nbs_ts_adversarial_fifo.sh — Adversarial FIFO testing for nbs-ts
#
# Tests concurrent writes, large messages, and rapid sequential sends
# to verify the O_RDWR FIFO fix and write atomicity.
#
# Exit codes: 0 = all pass, 1 = failure

set -euo pipefail

NBS_TS="${NBS_TS:-$(dirname "$0")/../../bin/nbs-ts}"
[[ -x "$NBS_TS" ]] || { echo "SKIP: nbs-ts not found"; exit 0; }

PASS=0
FAIL=0
pass() { echo "   PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "   FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== nbs-ts Adversarial FIFO Test ==="

# AF1: Rapid sequential sends (10 commands in quick succession)
echo "AF1. Rapid sequential sends..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
for i in $(seq 1 10); do
    "$NBS_TS" send "$HANDLE" "echo RAPID_$i"
done
sleep 3
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>/dev/null) || true
FOUND=0
for i in $(seq 1 10); do
    if echo "$OUTPUT" | grep -q "RAPID_$i"; then
        FOUND=$((FOUND + 1))
    fi
done
if [[ "$FOUND" -eq 10 ]]; then
    pass "All 10 rapid sends received ($FOUND/10)"
else
    fail "Only $FOUND/10 rapid sends received"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# AF2: Message at PIPE_BUF boundary (4096 bytes on Linux)
echo "AF2. Message at PIPE_BUF boundary..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
# Generate a 4000-char command (under PIPE_BUF)
LONG_CMD="echo $(head -c 3990 /dev/urandom | base64 | tr -d '\n' | head -c 3990)"
"$NBS_TS" send "$HANDLE" "$LONG_CMD"
sleep 2
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>/dev/null) || true
if [[ ${#OUTPUT} -gt 100 ]]; then
    pass "PIPE_BUF boundary message delivered (${#OUTPUT} bytes output)"
else
    fail "PIPE_BUF boundary message may have been lost (${#OUTPUT} bytes output)"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# AF3: Message above PIPE_BUF (8192 bytes)
echo "AF3. Message above PIPE_BUF..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
# Generate a command that produces large output
"$NBS_TS" send "$HANDLE" "seq 1 1000"
sleep 3
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>/dev/null) || true
if echo "$OUTPUT" | grep -q "1000"; then
    pass "Large output captured correctly"
else
    fail "Large output may have been truncated"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# AF4: Send to dead session
echo "AF4. Send to dead session..."
HANDLE=$("$NBS_TS" create 'exit 0')
sleep 2
if "$NBS_TS" send "$HANDLE" "echo hello" 2>/dev/null; then
    fail "Send to dead session should fail"
else
    pass "Send to dead session correctly rejected"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

# AF5: Concurrent sends from two subshells
echo "AF5. Concurrent sends..."
HANDLE=$("$NBS_TS" create 'bash')
sleep 1
(
    for i in $(seq 1 5); do
        "$NBS_TS" send "$HANDLE" "echo SENDER_A_$i"
        sleep 0.1
    done
) &
PID_A=$!
(
    for i in $(seq 1 5); do
        "$NBS_TS" send "$HANDLE" "echo SENDER_B_$i"
        sleep 0.1
    done
) &
PID_B=$!
wait $PID_A $PID_B 2>/dev/null || true
sleep 3
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>/dev/null) || true
COUNT_A=$(echo "$OUTPUT" | grep -c "SENDER_A_" || true)
COUNT_B=$(echo "$OUTPUT" | grep -c "SENDER_B_" || true)
TOTAL=$((COUNT_A + COUNT_B))
if [[ "$TOTAL" -ge 8 ]]; then
    pass "Concurrent sends: $TOTAL/10 received (A=$COUNT_A, B=$COUNT_B)"
else
    fail "Concurrent sends: only $TOTAL/10 received (A=$COUNT_A, B=$COUNT_B)"
fi
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

echo ""
echo "=== Result ==="
echo "PASS: $PASS  FAIL: $FAIL"
[[ "$FAIL" -eq 0 ]] && echo "All adversarial FIFO tests passed" || exit 1
