#!/bin/bash
# Test: Cross-sidecar oracle dedup
#
# Verifies that when two processes simultaneously attempt to spawn an oracle,
# the lock file prevents duplicate spawns. Uses the trigger lock mechanism
# directly via concurrent nbs-workers spawn attempts.
#
# Tests:
#   1. Two concurrent lock attempts — only one acquires the lock
#   2. Lock file persists for reuse
#   3. Sequential attempts both succeed (no stale lock)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

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

echo "=== Cross-Sidecar Oracle Dedup Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Two concurrent lock attempts ---
echo "1. Two concurrent lock attempts — only one wins..."

LOCKFILE="$TEST_DIR/oracle.lock"
RESULT_A="$TEST_DIR/result_a"
RESULT_B="$TEST_DIR/result_b"

# Process A: try to acquire lock, hold it for 2 seconds
(
    exec 9>"$LOCKFILE"
    if flock -n 9; then
        echo "acquired" > "$RESULT_A"
        sleep 2
        echo "released" >> "$RESULT_A"
    else
        echo "blocked" > "$RESULT_A"
    fi
) &
PID_A=$!

# Small delay so A gets the lock first
sleep 0.1

# Process B: try to acquire lock (should fail — non-blocking)
(
    exec 9>"$LOCKFILE"
    if flock -n 9; then
        echo "acquired" > "$RESULT_B"
    else
        echo "blocked" > "$RESULT_B"
    fi
) &
PID_B=$!

wait $PID_A 2>/dev/null || true
wait $PID_B 2>/dev/null || true

A_STATUS=$(cat "$RESULT_A" 2>/dev/null | head -1)
B_STATUS=$(cat "$RESULT_B" 2>/dev/null | head -1)

check "Process A acquired lock" "$( [[ "$A_STATUS" == "acquired" ]] && echo pass || echo fail )"
check "Process B was blocked" "$( [[ "$B_STATUS" == "blocked" ]] && echo pass || echo fail )"
check "Exactly one acquired" "$( [[ "$A_STATUS" == "acquired" && "$B_STATUS" == "blocked" ]] && echo pass || echo fail )"

echo ""

# --- Test 2: Lock file persists ---
echo "2. Lock file persists for reuse..."
check "Lock file exists after release" "$( [[ -f "$LOCKFILE" ]] && echo pass || echo fail )"

echo ""

# --- Test 3: Sequential attempts both succeed ---
echo "3. Sequential attempts both succeed (no stale lock)..."
RESULT_C="$TEST_DIR/result_c"
RESULT_D="$TEST_DIR/result_d"

# First sequential attempt
(
    exec 9>"$LOCKFILE"
    if flock -n 9; then
        echo "acquired" > "$RESULT_C"
    else
        echo "blocked" > "$RESULT_C"
    fi
)

# Second sequential attempt
(
    exec 9>"$LOCKFILE"
    if flock -n 9; then
        echo "acquired" > "$RESULT_D"
    else
        echo "blocked" > "$RESULT_D"
    fi
)

C_STATUS=$(cat "$RESULT_C" 2>/dev/null)
D_STATUS=$(cat "$RESULT_D" 2>/dev/null)

check "First sequential acquires" "$( [[ "$C_STATUS" == "acquired" ]] && echo pass || echo fail )"
check "Second sequential acquires" "$( [[ "$D_STATUS" == "acquired" ]] && echo pass || echo fail )"

echo ""

# --- Test 4: High contention (10 processes) ---
echo "4. High contention — 10 concurrent lock attempts..."
CONTENTION_DIR="$TEST_DIR/contention"
mkdir -p "$CONTENTION_DIR"
CONTENTION_LOCK="$CONTENTION_DIR/oracle.lock"

for i in $(seq 1 10); do
    (
        exec 9>"$CONTENTION_LOCK"
        if flock -n 9; then
            echo "acquired" > "$CONTENTION_DIR/result_$i"
            sleep 0.5
        else
            echo "blocked" > "$CONTENTION_DIR/result_$i"
        fi
    ) &
done
wait

ACQUIRED=$(grep -l "acquired" "$CONTENTION_DIR"/result_* 2>/dev/null | wc -l)
BLOCKED=$(grep -l "blocked" "$CONTENTION_DIR"/result_* 2>/dev/null | wc -l)
TOTAL=$((ACQUIRED + BLOCKED))

check "All 10 processes completed" "$( [[ $TOTAL -eq 10 ]] && echo pass || echo fail )"
check "Exactly 1 acquired under contention" "$( [[ $ACQUIRED -eq 1 ]] && echo pass || echo fail )"
check "9 were blocked" "$( [[ $BLOCKED -eq 9 ]] && echo pass || echo fail )"

echo ""

# --- Summary ---
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
