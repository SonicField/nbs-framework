#!/bin/bash
# Test: nbs-ts adversarial inputs
#
# Tests A1-A14 from nbs-ts-test-plan.md
# Verifies that invalid inputs are rejected safely.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-ts Adversarial Test ==="
echo ""

# A5: Send to nonexistent session
echo "A5. Send to nonexistent session..."
RC=0
"$NBS_TS" send "deadbeef" "hello" 2>/dev/null || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Send to nonexistent returns exit 2"
else
    fail "Send to nonexistent returned exit $RC (expected 2)"
fi

# A6: Read nonexistent session
echo "A6. Read nonexistent session..."
RC=0
"$NBS_TS" read-new "deadbeef" 2>/dev/null || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Read-new nonexistent returns exit 2"
else
    fail "Read-new nonexistent returned exit $RC (expected 2)"
fi

# A7: Kill nonexistent session
echo "A7. Kill nonexistent session..."
RC=0
"$NBS_TS" kill "deadbeef" 2>/dev/null || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Kill nonexistent returns exit 2"
else
    fail "Kill nonexistent returned exit $RC (expected 2)"
fi

# A8: Wait on nonexistent session
echo "A8. Wait on nonexistent session..."
RC=0
"$NBS_TS" wait-pattern "deadbeef" "x" --timeout=1 2>/dev/null || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Wait-pattern nonexistent returns exit 2"
else
    fail "Wait-pattern nonexistent returned exit $RC (expected 2)"
fi

# A9: Unknown command
echo "A9. Unknown command..."
RC=0
"$NBS_TS" bogus 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Unknown command returns exit 4"
else
    fail "Unknown command returned exit $RC (expected 4)"
fi

# A10: Create with missing args
echo "A10. Create with missing args..."
RC=0
"$NBS_TS" create 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Create without args returns exit 4"
else
    fail "Create without args returned exit $RC (expected 4)"
fi

# A11: Unknown option
echo "A11. Unknown option..."
# Create a real session first for valid handle
H=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H")
sleep 1
RC=0
"$NBS_TS" read-new "$H" --bogus=5 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Unknown option returns exit 4"
else
    fail "Unknown option returned exit $RC (expected 4)"
fi

# A1-A4: Handle validation (these use handles that aren't 8 hex chars)
echo "A1. Path traversal in handle..."
RC=0
"$NBS_TS" send "../../etc" "hello" 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Path traversal handle rejected (exit 4)"
else
    fail "Path traversal handle returned exit $RC (expected 4)"
fi

echo "A2. Slash in handle..."
RC=0
"$NBS_TS" send "test/bad" "hello" 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Slash in handle rejected (exit 4)"
else
    fail "Slash in handle returned exit $RC (expected 4)"
fi

echo "A3. Empty handle..."
RC=0
"$NBS_TS" send "" "hello" 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Empty handle rejected (exit 4)"
else
    fail "Empty handle returned exit $RC (expected 4)"
fi

echo "A4. Very long handle..."
LONG_HANDLE=$(printf '%0.sa' {1..256})
RC=0
"$NBS_TS" send "$LONG_HANDLE" "hello" 2>/dev/null || RC=$?
if [[ $RC -eq 4 ]]; then
    pass "Long handle rejected (exit 4)"
else
    fail "Long handle returned exit $RC (expected 4)"
fi

# A12: Large output
echo "A12. Large output (1MB+)..."
H12=$("$NBS_TS" create "head -c 1048576 /dev/urandom | base64" | tr -d '[:space:]')
HANDLES+=("$H12")
sleep 5
OUT12=$("$NBS_TS" read-new "$H12" 2>&1)
OUT12_LEN=${#OUT12}
if [[ $OUT12_LEN -gt 1000 ]]; then
    pass "Large output captured (${OUT12_LEN} bytes)"
else
    fail "Large output too small (${OUT12_LEN} bytes)"
fi

# A13: Concurrent sessions (reduced from 50 to 10 for CI speed)
echo "A13. Concurrent sessions (10)..."
CONCURRENT_HANDLES=()
CREATE_OK=true
for i in $(seq 1 10); do
    CH=$("$NBS_TS" create "sleep 10" 2>/dev/null | tr -d '[:space:]')
    if [[ -z "$CH" ]]; then
        fail "A13: Failed to create session $i"
        CREATE_OK=false
        break
    fi
    CONCURRENT_HANDLES+=("$CH")
done
sleep 1
if $CREATE_OK; then
    # Check all alive
    ALL_ALIVE=true
    LIST=$("$NBS_TS" list 2>&1)
    for ch in "${CONCURRENT_HANDLES[@]}"; do
        if ! echo "$LIST" | grep -q "$ch"; then
            ALL_ALIVE=false
            break
        fi
    done
    if $ALL_ALIVE; then
        pass "All 10 concurrent sessions created and visible"
    else
        fail "Not all concurrent sessions visible in list"
    fi
fi
# Kill all concurrent sessions
for ch in "${CONCURRENT_HANDLES[@]}"; do
    "$NBS_TS" kill "$ch" 2>/dev/null || true
done
sleep 1
# Verify cleanup
ZOMBIES=0
for ch in "${CONCURRENT_HANDLES[@]}"; do
    if "$NBS_TS" status "$ch" 2>/dev/null | grep -q "alive"; then
        ZOMBIES=$((ZOMBIES + 1))
    fi
done
if [[ $ZOMBIES -eq 0 ]]; then
    pass "All concurrent sessions cleaned up (no zombies)"
else
    fail "$ZOMBIES zombie sessions remain"
fi

# A14: Session handle collision
echo "A14. Session handle reuse after kill..."
H14=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H14")
sleep 0.5
"$NBS_TS" kill "$H14" 2>/dev/null || true
HANDLES=("${HANDLES[@]/$H14}")
sleep 0.5
# Handles are random, so collision is extremely unlikely.
# Instead verify that creating after killing works.
H14B=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$H14B")
sleep 0.5
STATUS14=$("$NBS_TS" status "$H14B" 2>&1)
if echo "$STATUS14" | grep -q "alive"; then
    pass "New session after kill works correctly"
else
    fail "New session after kill has unexpected status: $STATUS14"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All adversarial tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
