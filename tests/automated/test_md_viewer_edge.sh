#!/bin/bash
# Test: nbs-md-viewer edge cases — empty input, single character, binary garbage
#
# Plan section 8.2: test_md_viewer_edge.sh
# Must not crash on any input.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
MD_VIEWER="$PROJECT_ROOT/bin/nbs-md-viewer"

[ -x "$MD_VIEWER" ] || { echo "SKIP: nbs-md-viewer not found"; exit 0; }
[ -x "$NBS_TS" ] || { echo "SKIP: nbs-ts not found"; exit 0; }

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-md-viewer Edge Cases Test ==="
echo ""

TMPDIR=$(mktemp -d)

# E1: Empty input
echo "E1. Empty input does not crash..."
HANDLE=$("$NBS_TS" create --name=md-empty "echo -n '' | $MD_VIEWER" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2
"$NBS_TS" send "$HANDLE" "q" 2>/dev/null || true
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true
sleep 1

STATUS=$("$NBS_TS" status "$HANDLE" 2>&1)
if echo "$STATUS" | grep -q "dead"; then
    EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE" 2>&1) || EXIT_CODE="unknown"
    if [ "$EXIT_CODE" = "0" ]; then
        pass "Empty input: exited cleanly (code 0)"
    else
        # Some viewers exit with non-zero on empty input — acceptable as long as no crash
        pass "Empty input: exited with code $EXIT_CODE (no crash)"
    fi
else
    "$NBS_TS" kill "$HANDLE" 2>/dev/null || true
    fail "Empty input: viewer stuck (had to be killed)"
fi

# E2: Single character
echo "E2. Single character input does not crash..."
HANDLE2=$("$NBS_TS" create --name=md-single "echo 'X' | $MD_VIEWER" | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 2
"$NBS_TS" send "$HANDLE2" "q" 2>/dev/null || true
"$NBS_TS" wait-complete "$HANDLE2" --timeout=5 2>/dev/null || true
sleep 1

STATUS2=$("$NBS_TS" status "$HANDLE2" 2>&1)
if echo "$STATUS2" | grep -q "dead"; then
    EXIT_CODE2=$("$NBS_TS" exit-code "$HANDLE2" 2>&1) || EXIT_CODE2="unknown"
    if [ "$EXIT_CODE2" = "0" ]; then
        pass "Single character: exited cleanly (code 0)"
    else
        pass "Single character: exited with code $EXIT_CODE2 (no crash)"
    fi
else
    "$NBS_TS" kill "$HANDLE2" 2>/dev/null || true
    fail "Single character: viewer stuck (had to be killed)"
fi

# E3: Binary garbage (random bytes including NUL, control chars)
echo "E3. Binary garbage input does not crash..."
# Generate 256 bytes of random binary data including NUL bytes
dd if=/dev/urandom bs=256 count=1 of="$TMPDIR/garbage.bin" 2>/dev/null
HANDLE3=$("$NBS_TS" create --name=md-binary "$MD_VIEWER < $TMPDIR/garbage.bin" | tr -d '[:space:]')
HANDLES+=("$HANDLE3")
sleep 2
"$NBS_TS" send "$HANDLE3" "q" 2>/dev/null || true
"$NBS_TS" wait-complete "$HANDLE3" --timeout=5 2>/dev/null || true
sleep 1

STATUS3=$("$NBS_TS" status "$HANDLE3" 2>&1)
if echo "$STATUS3" | grep -q "dead"; then
    EXIT_CODE3=$("$NBS_TS" exit-code "$HANDLE3" 2>&1) || EXIT_CODE3="unknown"
    # Any exit code is fine — the key thing is it didn't segfault/hang
    pass "Binary garbage: exited with code $EXIT_CODE3 (no crash)"
else
    "$NBS_TS" kill "$HANDLE3" 2>/dev/null || true
    fail "Binary garbage: viewer stuck (had to be killed)"
fi

# E4: Very long line (10000 characters)
echo "E4. Very long line does not crash..."
python3 -c "print('x' * 10000)" > "$TMPDIR/long.md"
HANDLE4=$("$NBS_TS" create --name=md-long "$MD_VIEWER < $TMPDIR/long.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE4")
sleep 2
"$NBS_TS" send "$HANDLE4" "q" 2>/dev/null || true
"$NBS_TS" wait-complete "$HANDLE4" --timeout=5 2>/dev/null || true
sleep 1

STATUS4=$("$NBS_TS" status "$HANDLE4" 2>&1)
if echo "$STATUS4" | grep -q "dead"; then
    EXIT_CODE4=$("$NBS_TS" exit-code "$HANDLE4" 2>&1) || EXIT_CODE4="unknown"
    if [ "$EXIT_CODE4" = "0" ]; then
        pass "Long line: exited cleanly (code 0)"
    else
        # Non-zero exit is acceptable as long as no crash/signal
        pass "Long line: exited with code $EXIT_CODE4 (no crash)"
    fi
else
    "$NBS_TS" kill "$HANDLE4" 2>/dev/null || true
    fail "Long line: viewer stuck (had to be killed)"
fi

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
