#!/bin/bash
# Test: nbs-prompts engineering standards compliance
#
# Tests the nbs-prompts script against violations identified in the
# engineering standards audit report. Covers the BUG fix (numeric
# validation of od output) and all HARDENING guards.
#
# Deterministic tests (no external dependencies):
#   1.  Script exits 0 under normal operation
#   2.  Output is non-empty
#   3.  Output contains the anti-hallucination rule
#   4.  Output contains an @scribe or @supervisor mention
#   5.  Multiple runs produce valid output (property: always valid)
#   6.  Script has set -euo pipefail
#
# Audit violation tests:
#   V1 (BUG):  Numeric validation guard exists for raw od output
#   V2 (HARDENING): Empty-array guard exists
#   V3 (HARDENING): u32 range postcondition guard exists
#   V4 (HARDENING): Non-empty prompt postcondition guard exists
#   V5 (HARDENING): Error message distinguishes pipeline stage
#   V6 (HARDENING): Comment documents the actual invariant
#
# Adversarial tests:
#   A1. Output is a single line (safe for piping)
#   A2. Output does not contain null bytes
#   A3. All prompts in the array are non-empty (static check)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_PROMPTS="$PROJECT_ROOT/bin/nbs-prompts"

ERRORS=0

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

# --- Precondition ---
if [[ ! -x "$NBS_PROMPTS" ]]; then
    echo "FATAL: nbs-prompts not found or not executable at $NBS_PROMPTS"
    exit 1
fi

echo "Test: nbs-prompts engineering standards compliance"
echo "=================================================="
echo ""

# --- Deterministic tests ---
echo "Deterministic tests:"

# 1. Script exits 0 under normal operation
OUTPUT=$(bash "$NBS_PROMPTS" 2>/dev/null)
RC=$?
if [[ $RC -eq 0 ]]; then
    check "1. Script exits 0 under normal operation" "pass"
else
    check "1. Script exits 0 under normal operation (got exit $RC)" "fail"
fi

# 2. Output is non-empty
if [[ -n "$OUTPUT" ]]; then
    check "2. Output is non-empty" "pass"
else
    check "2. Output is non-empty" "fail"
fi

# 3. Output contains the anti-hallucination rule
if echo "$OUTPUT" | grep -qF 'No quote = no evidence'; then
    check "3. Output contains anti-hallucination rule" "pass"
else
    check "3. Output contains anti-hallucination rule" "fail"
fi

# 4. Output contains @scribe or @supervisor
if echo "$OUTPUT" | grep -qF '@scribe' || echo "$OUTPUT" | grep -qF '@supervisor'; then
    check "4. Output contains agent mention (@scribe or @supervisor)" "pass"
else
    check "4. Output contains agent mention (@scribe or @supervisor)" "fail"
fi

# 5. Multiple runs produce valid output (property: always valid)
MULTI_PASS=true
for i in $(seq 1 20); do
    RUN_OUT=$(bash "$NBS_PROMPTS" 2>/dev/null) || { MULTI_PASS=false; break; }
    if [[ -z "$RUN_OUT" ]]; then
        MULTI_PASS=false
        break
    fi
    if ! echo "$RUN_OUT" | grep -qF 'No quote = no evidence'; then
        MULTI_PASS=false
        break
    fi
done
if $MULTI_PASS; then
    check "5. 20 consecutive runs all produce valid output" "pass"
else
    check "5. 20 consecutive runs all produce valid output" "fail"
fi

# 6. Script has set -euo pipefail
if grep -q '^set -euo pipefail' "$NBS_PROMPTS"; then
    check "6. Script has set -euo pipefail" "pass"
else
    check "6. Script has set -euo pipefail" "fail"
fi

echo ""
echo "Audit violation tests:"

# V1 (BUG): Numeric validation guard exists for raw od output
# The script must validate that raw matches ^[0-9]+$ before arithmetic use
if grep -qE '\[\[.*\$raw.*\^?\[0-9\]' "$NBS_PROMPTS" || \
   grep -qE 'raw.*=~.*\[0-9\]' "$NBS_PROMPTS"; then
    check "V1 (BUG). Numeric validation of raw od output" "pass"
else
    check "V1 (BUG). Numeric validation of raw od output" "fail"
fi

# V2 (HARDENING): Empty-array guard exists
# The script must check ${#prompts[@]} > 0 before using as divisor
if grep -qE '#prompts\[@\].*==.*0|prompts.*empty' "$NBS_PROMPTS"; then
    check "V2 (HARDENING). Empty-array guard" "pass"
else
    check "V2 (HARDENING). Empty-array guard" "fail"
fi

# V3 (HARDENING): u32 range postcondition
# The script must verify raw <= 4294967295
if grep -qF '4294967295' "$NBS_PROMPTS"; then
    check "V3 (HARDENING). u32 range postcondition" "pass"
else
    check "V3 (HARDENING). u32 range postcondition" "fail"
fi

# V4 (HARDENING): Non-empty prompt postcondition
# The script must verify the selected prompt is non-empty before echo
if grep -qE '\-z.*prompts\[.*index' "$NBS_PROMPTS"; then
    check "V4 (HARDENING). Non-empty prompt postcondition" "pass"
else
    check "V4 (HARDENING). Non-empty prompt postcondition" "fail"
fi

# V5 (HARDENING): Error message distinguishes pipeline stage
# The error message should mention both od and tr, or pipeline context
if grep -qE 'od.*tr.*pipeline|od/tr' "$NBS_PROMPTS"; then
    check "V5 (HARDENING). Pipeline error message distinguishes stage" "pass"
else
    check "V5 (HARDENING). Pipeline error message distinguishes stage" "fail"
fi

# V6 (HARDENING): Comment documents actual invariant (not unfalsifiable claim)
# The old comment "V3.2: Check od pipeline failure" should be replaced
# with a comment describing the actual invariant
if grep -qF 'V3.2' "$NBS_PROMPTS"; then
    check "V6 (HARDENING). Comment documents actual invariant (V3.2 still present)" "fail"
else
    # Check that there IS a descriptive comment about the random generation
    if grep -qi 'random.*u32\|u32.*random\|Generate random\|random.*urandom' "$NBS_PROMPTS"; then
        check "V6 (HARDENING). Comment documents actual invariant" "pass"
    else
        check "V6 (HARDENING). Comment documents actual invariant (no replacement found)" "fail"
    fi
fi

echo ""
echo "Adversarial tests:"

# A1. Output is a single line
LINE_COUNT=$(echo "$OUTPUT" | wc -l)
if [[ "$LINE_COUNT" -eq 1 ]]; then
    check "A1. Output is a single line" "pass"
else
    check "A1. Output is a single line (got $LINE_COUNT lines)" "fail"
fi

# A2. Output does not contain null bytes
if echo "$OUTPUT" | tr -d '\0' | cmp -s - <(echo "$OUTPUT"); then
    check "A2. Output contains no null bytes" "pass"
else
    check "A2. Output contains no null bytes" "fail"
fi

# A3. All prompts in array are non-empty (static check)
# Extract prompts between quotes in the array and verify none are empty
EMPTY_PROMPTS=0
if grep -q '^""$' "$NBS_PROMPTS" 2>/dev/null; then
    EMPTY_PROMPTS=1
fi
if [[ "$EMPTY_PROMPTS" -eq 0 ]]; then
    check "A3. No empty prompts in array (static check)" "pass"
else
    check "A3. No empty prompts in array (static check)" "fail"
fi

echo ""
echo "=================================================="
if [[ $ERRORS -eq 0 ]]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "FAILURES: $ERRORS"
    exit 1
fi
