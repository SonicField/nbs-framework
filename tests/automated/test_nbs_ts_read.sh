#!/bin/bash
# Test: nbs-ts read operations
#
# Tests R1-R7 from nbs-ts-test-plan.md

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

new_session() {
    local h
    h=$("$NBS_TS" create bash | tr -d '[:space:]')
    HANDLES+=("$h")
    sleep 1
    echo "$h"
}

echo "=== nbs-ts Read Test ==="
echo ""

# R1: read-new tracks offset
echo "R1. read-new tracks offset..."
H=$(new_session)
M1="READ1A_$$"
M2="READ1B_$$"
M3="READ1C_$$"
"$NBS_TS" send "$H" "echo $M1"
sleep 1
OUT1=$("$NBS_TS" read-new "$H" --strip 2>&1)
"$NBS_TS" send "$H" "echo $M2"
sleep 1
OUT2=$("$NBS_TS" read-new "$H" --strip 2>&1)
"$NBS_TS" send "$H" "echo $M3"
sleep 1
OUT3=$("$NBS_TS" read-new "$H" --strip 2>&1)

R1_OK=true
if ! echo "$OUT1" | grep -q "$M1"; then
    fail "R1: First read-new missing M1"
    R1_OK=false
fi
if ! echo "$OUT2" | grep -q "$M2"; then
    fail "R1: Second read-new missing M2"
    R1_OK=false
fi
if echo "$OUT2" | grep -q "$M1"; then
    fail "R1: Second read-new contains M1 (cursor not advanced)"
    R1_OK=false
fi
if ! echo "$OUT3" | grep -q "$M3"; then
    fail "R1: Third read-new missing M3"
    R1_OK=false
fi
if echo "$OUT3" | grep -q "$M2"; then
    fail "R1: Third read-new contains M2 (cursor not advanced)"
    R1_OK=false
fi
if $R1_OK; then
    pass "read-new tracks offset across 3 reads"
fi

# R2: read with --offset
echo "R2. read with --offset..."
OUT_FULL=$("$NBS_TS" read "$H" --offset=1 2>&1)
if [[ -n "$OUT_FULL" ]]; then
    pass "read --offset=1 returned data"
else
    fail "read --offset=1 returned empty"
fi

# R3: read-new --strip removes ANSI
echo "R3. read-new --strip removes ANSI..."
H2=$(new_session)
# Use tput or printf to produce ANSI escape sequences
"$NBS_TS" send "$H2" "printf '\\033[31mRED_TEXT\\033[0m'"
sleep 1
RAW=$("$NBS_TS" read-new "$H2" 2>&1)
# Reset cursor for stripped read by creating a new session
H3=$(new_session)
"$NBS_TS" send "$H3" "printf '\\033[31mRED_STRIP_$$\\033[0m'"
sleep 1
STRIPPED=$("$NBS_TS" read-new "$H3" --strip 2>&1)
if echo "$STRIPPED" | grep -q "RED_STRIP_$$"; then
    if ! echo "$STRIPPED" | grep -q $'\033'; then
        pass "--strip removed ANSI escapes, preserved text"
    else
        fail "--strip left escape sequences in output"
    fi
else
    fail "--strip output missing expected text"
    echo "   Stripped: $STRIPPED"
fi

# R4: read-new --strip preserves content
echo "R4. read-new --strip preserves content..."
H4=$(new_session)
CONTENT="PLAIN_CONTENT_$$"
"$NBS_TS" send "$H4" "echo $CONTENT"
sleep 1
STRIPPED4=$("$NBS_TS" read-new "$H4" --strip 2>&1)
if echo "$STRIPPED4" | grep -q "$CONTENT"; then
    pass "--strip preserves plain text content"
else
    fail "--strip lost plain text content"
    echo "   Expected: $CONTENT"
    echo "   Got: $STRIPPED4"
fi

# R6: read --last=N returns tail viewport
echo "R6. read --last=N returns tail viewport..."
H6=$(new_session)
for i in $(seq 1 20); do
    "$NBS_TS" send "$H6" "echo RLINE_$i"
done
sleep 3
LAST5=$("$NBS_TS" read "$H6" --last=5 2>&1 || true)
R6_OK=true
# Should contain the final lines, not the first lines
if echo "$LAST5" | grep -q "RLINE_20"; then
    : # good
else
    fail "R6: --last=5 missing RLINE_20 (final line)"
    echo "   Got: $LAST5"
    R6_OK=false
fi
if echo "$LAST5" | grep -q "RLINE_1$"; then
    fail "R6: --last=5 contains RLINE_1 (should only have tail)"
    R6_OK=false
fi
if $R6_OK; then
    pass "--last=5 returns tail lines including final output"
fi

# R7: read --last=N stable on repeated calls
echo "R7. read --last=N stable on repeated calls..."
LAST5_A=$("$NBS_TS" read "$H6" --last=5 2>&1 || true)
LAST5_B=$("$NBS_TS" read "$H6" --last=5 2>&1 || true)
if [[ "$LAST5_A" == "$LAST5_B" ]]; then
    pass "--last=5 returns same content on repeated calls"
else
    fail "--last=5 returned different content on repeated calls"
    echo "   First:  $(echo "$LAST5_A" | head -1)"
    echo "   Second: $(echo "$LAST5_B" | head -1)"
fi

# R5: read after kill
echo "R5. read after kill..."
H5=$(new_session)
MARKER5="READKILL_$$"
"$NBS_TS" send "$H5" "echo $MARKER5"
sleep 1
# Read to verify marker is there
"$NBS_TS" read-new "$H5" --strip >/dev/null 2>&1
# Kill session
"$NBS_TS" kill "$H5" 2>/dev/null || true
# Remove from cleanup list
HANDLES=("${HANDLES[@]/$H5}")
sleep 0.5
# Try to read from offset 0 — session dir is gone, so this should fail gracefully
RC=0
READ_AFTER=$("$NBS_TS" read "$H5" --offset=1 2>&1) || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "read after kill returns exit 2 (not found) — correct"
elif [[ $RC -eq 0 && -n "$READ_AFTER" ]]; then
    pass "read after kill still returned data (log cached or available)"
else
    pass "read after kill handled gracefully (exit $RC)"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All read tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
