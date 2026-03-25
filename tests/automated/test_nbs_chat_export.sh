#!/bin/bash
# Test: nbs-chat export command
#
# Tests:
#   1.  Export empty chat — clean exit, no output
#   2.  Export all messages — ANSI colours present, correct count
#   3.  --last=N — correct subset
#   4.  --handle single — only matching handle
#   5.  --handle comma-separated — multiple handles
#   6.  --handle no match — empty output, exit 0
#   7.  --grep — case-insensitive match
#   8.  --grep no match — empty output, exit 0
#   9.  --from and --to — index range
#   10. --from exceeds message count — empty output
#   11. --after and --before — time range
#   12. Combined filters — --last + --handle + --grep
#   13. handle_match edge cases — substring no match, single handle
#   14. Missing file — exit 2
#   15. No arguments — exit 4
#   16. Colour consistency — same handle gets same colour

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

TEST_DIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
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

echo "=== nbs-chat export Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Export empty chat ---
echo "1. Export empty chat..."
CHAT="$TEST_DIR/empty.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)
RC=$?
check "Exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Empty output" "$( [[ -z "$OUTPUT" ]] && echo pass || echo fail )"

# --- Populate a test chat ---
CHAT="$TEST_DIR/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" supervisor "Phase 1 complete — all tests pass" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" theologian "Architecture review: ceiling confirmed" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" testkeeper "ABBA verified: fibonacci 1.60x" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" supervisor "Phase 2 starting now" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" generalist "Ready for assignment" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" theologian "Gap map analysis complete" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" gatekeeper "No commits to review" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" supervisor "Session close — ceiling confirmed" >/dev/null 2>&1

# --- Test 2: Export all messages ---
echo "2. Export all messages..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)
RC=$?
# Count lines — each message is one line of output
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "Exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "8 messages" "$( [[ $LINE_COUNT -eq 8 ]] && echo pass || echo fail )"
# ANSI escape codes present
check "Contains ANSI escapes" "$( echo "$OUTPUT" | grep -qP '\033\[' && echo pass || echo fail )"
# Contains handle names
check "Contains supervisor" "$( echo "$OUTPUT" | grep -q 'supervisor' && echo pass || echo fail )"
check "Contains theologian" "$( echo "$OUTPUT" | grep -q 'theologian' && echo pass || echo fail )"

# --- Test 3: --last=N ---
echo "3. --last=N filter..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --last=3 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "3 messages" "$( [[ $LINE_COUNT -eq 3 ]] && echo pass || echo fail )"
check "Last message is supervisor" "$( echo "$OUTPUT" | tail -1 | grep -q 'supervisor' && echo pass || echo fail )"
check "Does not contain testkeeper" "$( echo "$OUTPUT" | grep -qv 'testkeeper' && echo pass || echo fail )"

# --- Test 4: --handle single ---
echo "4. --handle single filter..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --handle=theologian 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "2 theologian messages" "$( [[ $LINE_COUNT -eq 2 ]] && echo pass || echo fail )"
# Every line should contain theologian
NON_THEO=$(echo "$OUTPUT" | grep -cv 'theologian' || true)
check "All lines are theologian" "$( [[ $NON_THEO -eq 0 ]] && echo pass || echo fail )"

# --- Test 5: --handle comma-separated ---
echo "5. --handle comma-separated..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --handle=theologian,testkeeper 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "3 messages (2 theologian + 1 testkeeper)" "$( [[ $LINE_COUNT -eq 3 ]] && echo pass || echo fail )"
check "Contains theologian" "$( echo "$OUTPUT" | grep -q 'theologian' && echo pass || echo fail )"
check "Contains testkeeper" "$( echo "$OUTPUT" | grep -q 'testkeeper' && echo pass || echo fail )"
# No other handles
check "No supervisor" "$( ! echo "$OUTPUT" | grep -q 'supervisor' && echo pass || echo fail )"

# --- Test 6: --handle no match ---
echo "6. --handle no match..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --handle=nonexistent 2>&1)
RC=$?
check "Exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Empty output" "$( [[ -z "$OUTPUT" ]] && echo pass || echo fail )"

# --- Test 7: --grep ---
echo "7. --grep case-insensitive..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --grep=ceiling 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "2 matches for ceiling" "$( [[ $LINE_COUNT -eq 2 ]] && echo pass || echo fail )"
# Case insensitive
OUTPUT2=$("$NBS_CHAT" export "$CHAT" --grep=CEILING 2>&1)
LINE_COUNT2=$(echo "$OUTPUT2" | wc -l)
check "Case insensitive match" "$( [[ $LINE_COUNT2 -eq 2 ]] && echo pass || echo fail )"

# --- Test 8: --grep no match ---
echo "8. --grep no match..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --grep=zzzznonexistent 2>&1)
RC=$?
check "Exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Empty output" "$( [[ -z "$OUTPUT" ]] && echo pass || echo fail )"

# --- Test 9: --from and --to ---
echo "9. --from and --to range..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --from=2 --to=5 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "3 messages (index 2,3,4)" "$( [[ $LINE_COUNT -eq 3 ]] && echo pass || echo fail )"
# First should be testkeeper (index 2), last should be generalist (index 4)
check "First is testkeeper" "$( echo "$OUTPUT" | head -1 | grep -q 'testkeeper' && echo pass || echo fail )"
check "Last is generalist" "$( echo "$OUTPUT" | tail -1 | grep -q 'generalist' && echo pass || echo fail )"

# --- Test 10: --from exceeds message count ---
echo "10. --from exceeds message count..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --from=100 2>&1)
RC=$?
check "Exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Empty output" "$( [[ -z "$OUTPUT" ]] && echo pass || echo fail )"

# --- Test 11: --after and --before ---
echo "11. --after and --before time range..."
# Use relative time — all messages are recent
OUTPUT=$("$NBS_CHAT" export "$CHAT" --after=1h 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
check "All 8 messages within last hour" "$( [[ $LINE_COUNT -eq 8 ]] && echo pass || echo fail )"
# Far future — nothing
OUTPUT2=$("$NBS_CHAT" export "$CHAT" --before=2020-01-01T00:00:00 2>&1)
RC=$?
check "No messages before 2020" "$( [[ -z "$OUTPUT2" ]] && echo pass || echo fail )"

# --- Test 12: Combined filters ---
echo "12. Combined filters --last + --handle + --grep..."
OUTPUT=$("$NBS_CHAT" export "$CHAT" --last=5 --handle=supervisor --grep=phase 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
# Last 5 messages, filtered to supervisor, filtered to "phase"
# Last 5 are: supervisor(Phase 2), generalist, theologian, gatekeeper, supervisor(Session close)
# supervisor in last 5: "Phase 2 starting now" and "Session close — ceiling confirmed"
# Of those, containing "phase": "Phase 2 starting now"
check "1 match for combined" "$( [[ $LINE_COUNT -eq 1 ]] && echo pass || echo fail )"
check "Contains Phase 2" "$( echo "$OUTPUT" | grep -q 'Phase 2' && echo pass || echo fail )"

# --- Test 13: handle_match edge cases ---
echo "13. handle_match edge cases..."
# Substring should NOT match: "theo" should not match "theologian"
OUTPUT=$("$NBS_CHAT" export "$CHAT" --handle=theo 2>&1)
check "Substring no match" "$( [[ -z "$OUTPUT" ]] && echo pass || echo fail )"
# Exact single handle
OUTPUT2=$("$NBS_CHAT" export "$CHAT" --handle=gatekeeper 2>&1)
LINE_COUNT=$(echo "$OUTPUT2" | wc -l)
check "Single handle exact match" "$( [[ $LINE_COUNT -eq 1 ]] && echo pass || echo fail )"

# --- Test 14: Missing file ---
echo "14. Missing file..."
RC=0
"$NBS_CHAT" export "$TEST_DIR/nonexistent.chat" >/dev/null 2>&1 || RC=$?
check "Exit 2 for missing file" "$( [[ $RC -eq 2 ]] && echo pass || echo fail )"

# --- Test 15: No arguments ---
echo "15. No arguments..."
RC=0
"$NBS_CHAT" export >/dev/null 2>&1 || RC=$?
check "Exit 4 for no args" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- Test 16: Colour consistency ---
echo "16. Colour consistency..."
# Export twice — same handle should get same colour code
OUTPUT1=$("$NBS_CHAT" export "$CHAT" --handle=supervisor 2>&1)
OUTPUT2=$("$NBS_CHAT" export "$CHAT" --handle=supervisor 2>&1)
# Extract ANSI colour code from first line
COLOUR1=$(echo "$OUTPUT1" | head -1 | grep -oP '\033\[\d+;\d+;\d+m' | head -1)
COLOUR2=$(echo "$OUTPUT2" | head -1 | grep -oP '\033\[\d+;\d+;\d+m' | head -1)
check "Same colour across runs" "$( [[ "$COLOUR1" == "$COLOUR2" ]] && echo pass || echo fail )"
# Different handles get different colours WITHIN the same export
OUTPUT_ALL=$("$NBS_CHAT" export "$CHAT" 2>&1)
# supervisor is first handle (colour 0), theologian is second (colour 1)
SUP_LINE=$(echo "$OUTPUT_ALL" | grep 'supervisor' | head -1)
THEO_LINE=$(echo "$OUTPUT_ALL" | grep 'theologian' | head -1)
SUP_COLOUR=$(echo "$SUP_LINE" | grep -oP '\033\[\d+;\d+;\d+m' | head -1)
THEO_COLOUR=$(echo "$THEO_LINE" | grep -oP '\033\[\d+;\d+;\d+m' | head -1)
check "Different handles get different colours" "$( [[ "$SUP_COLOUR" != "$THEO_COLOUR" ]] && echo pass || echo fail )"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
