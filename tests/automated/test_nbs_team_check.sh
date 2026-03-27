#!/bin/bash
# Test: nbs-team-check standalone health check script
#
# Tests exit codes, argument validation, and output format.
# Does NOT require a running team — tests the script's behaviour
# against the filesystem and process table.
#
# Tests:
#   1.  No arguments → exit 4
#   2.  One argument → exit 4
#   3.  Empty chat-tag → exit 4
#   4.  Nonexistent project root → exit 4
#   5.  Valid args, no agents → exit 1, reports 0/7
#   6.  Output mentions "Suggest: /fixup" when agents missing
#   7.  Output reports sidecar count
#   8.  Three arguments → exit 4 (too many)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TEAM_CHECK="${PROJECT_ROOT}/bin/nbs-team-check"

# Add bin/ to PATH so nbs-ts is findable
export PATH="${PROJECT_ROOT}/bin:$PATH"

TEST_DIR=$(mktemp -d)
ERRORS=0
PASS_COUNT=0

cleanup() {
    rm -rf "$TEST_DIR"
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

echo "=== nbs-team-check Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: No arguments → exit 4 ---
echo "1. No arguments..."
set +e
OUTPUT=$("$NBS_TEAM_CHECK" 2>&1)
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
check "shows usage" "$( echo "$OUTPUT" | grep -qF 'Usage:' && echo pass || echo fail )"

echo ""

# --- Test 2: One argument → exit 4 ---
echo "2. One argument..."
set +e
"$NBS_TEAM_CHECK" "sometag" 2>/dev/null
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

echo ""

# --- Test 3: Empty chat-tag → exit 4 ---
echo "3. Empty chat-tag..."
set +e
"$NBS_TEAM_CHECK" "" "$TEST_DIR" 2>/dev/null
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

echo ""

# --- Test 4: Nonexistent project root → exit 4 ---
echo "4. Nonexistent project root..."
set +e
OUTPUT=$("$NBS_TEAM_CHECK" "test" "/nonexistent/path/$$" 2>&1)
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
check "error mentions path" "$( echo "$OUTPUT" | grep -qF '/nonexistent/' && echo pass || echo fail )"

echo ""

# --- Test 5: Valid args, no agents → exit 1, reports 0/7 ---
echo "5. Valid args, no agents running..."
PROJ="$TEST_DIR/proj5"
mkdir -p "$PROJ/.nbs"
set +e
OUTPUT=$("$NBS_TEAM_CHECK" "nonexistent-tag" "$PROJ" 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "reports 0/7 agents" "$( echo "$OUTPUT" | grep -qF '0/7' && echo pass || echo fail )"

echo ""

# --- Test 6: Output suggests /fixup when agents missing ---
echo "6. Output suggests /fixup..."
check "suggests /fixup" "$( echo "$OUTPUT" | grep -qF '/fixup' && echo pass || echo fail )"

echo ""

# --- Test 7: Output reports sidecar count ---
echo "7. Output reports sidecar count..."
check "reports sidecars" "$( echo "$OUTPUT" | grep -qi 'sidecar' && echo pass || echo fail )"

echo ""

# --- Test 8: Three arguments → exit 4 ---
echo "8. Three arguments..."
set +e
"$NBS_TEAM_CHECK" "tag" "$TEST_DIR" "extra" 2>/dev/null
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

echo ""

# --- Test 9: Script is executable ---
echo "9. Script is executable..."
check "is executable" "$( [[ -x "$NBS_TEAM_CHECK" ]] && echo pass || echo fail )"

echo ""

# --- Test 10: Missing sidecars listed by name ---
echo "10. Missing sidecars listed by name..."
PROJ10="$TEST_DIR/proj10"
mkdir -p "$PROJ10/.nbs"
set +e
OUTPUT=$("$NBS_TEAM_CHECK" "nonexistent-tag" "$PROJ10" 2>&1)
set -e
check "lists scribe" "$( echo "$OUTPUT" | grep -qF 'scribe' && echo pass || echo fail )"
check "lists supervisor" "$( echo "$OUTPUT" | grep -qF 'supervisor' && echo pass || echo fail )"
check "lists generalist" "$( echo "$OUTPUT" | grep -qF 'generalist' && echo pass || echo fail )"

echo ""

# --- Test 11: Session name format is nbs-<role>-<tag> ---
echo "11. Session name format..."
# Verify the script greps for the correct pattern by inspecting source
GREP_PATTERN=$(grep 'session_name=' "$NBS_TEAM_CHECK" | head -1)
check "uses nbs-role-tag format" "$( echo "$GREP_PATTERN" | grep -qF 'nbs-${agent}-${CHAT_TAG}' && echo pass || echo fail )"
# Verify grep matches status before name (alive comes before session name in nbs-ts output)
GREP_LINE=$(grep 'alive.*session_name\|session_name.*alive' "$NBS_TEAM_CHECK" | head -1)
check "greps alive before name" "$( echo "$GREP_LINE" | grep -qF 'alive.*${session_name}' && echo pass || echo fail )"

echo ""

# --- Summary ---
echo "=== Results: $PASS_COUNT passed, $ERRORS failed ==="
if [[ $ERRORS -eq 0 ]]; then
    exit 0
else
    exit 1
fi
