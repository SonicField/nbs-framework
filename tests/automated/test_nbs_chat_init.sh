#!/bin/bash
# Test: nbs-chat-init bootstrap tool
#
# Tests:
#   1. --dry-run shows planned actions without executing
#   2. Creates correct .nbs/ directory structure
#   3. Creates chat file with correct name
#   4. Creates scribe log with correct name
#   5. Creates events directory with processed/
#   6. Creates workers directory
#   7. --help shows usage
#   8. Missing --name shows error
#   9. Double-init is safe (no errors on existing structure)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT_INIT="${NBS_CHAT_INIT_BIN:-$(which nbs-chat-init 2>/dev/null || echo "$PROJECT_ROOT/bin/nbs-chat-init")}"

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

echo "=== nbs-chat-init Bootstrap Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: --dry-run shows planned actions ---
echo "1. --dry-run shows planned actions..."
PROJ="$TEST_DIR/proj1"
mkdir -p "$PROJ"
OUTPUT=$("$NBS_CHAT_INIT" --name=testchat --root="$PROJ" --dry-run 2>&1) || true
check "--dry-run shows actions" "$( echo "$OUTPUT" | grep -qi 'would\|dry.run\|create\|mkdir' && echo pass || echo fail )"
check "--dry-run does not create .nbs" "$( [[ ! -d "$PROJ/.nbs" ]] && echo pass || echo fail )"

echo ""

# --- Test 2: Creates correct .nbs/ structure ---
echo "2. Creates correct .nbs/ directory structure..."
PROJ="$TEST_DIR/proj2"
mkdir -p "$PROJ"
"$NBS_CHAT_INIT" --name=myproject --root="$PROJ" --force 2>&1 >/dev/null || true
check ".nbs/ exists" "$( [[ -d "$PROJ/.nbs" ]] && echo pass || echo fail )"
check ".nbs/chat/ exists" "$( [[ -d "$PROJ/.nbs/chat" ]] && echo pass || echo fail )"
check ".nbs/events/ exists" "$( [[ -d "$PROJ/.nbs/events" ]] && echo pass || echo fail )"
check ".nbs/events/processed/ exists" "$( [[ -d "$PROJ/.nbs/events/processed" ]] && echo pass || echo fail )"
check ".nbs/workers/ exists" "$( [[ -d "$PROJ/.nbs/workers" ]] && echo pass || echo fail )"
check ".nbs/scribe/ exists" "$( [[ -d "$PROJ/.nbs/scribe" ]] && echo pass || echo fail )"

echo ""

# --- Test 3: Creates chat file with correct name ---
echo "3. Creates chat file with correct name..."
check "chat file exists" "$( [[ -f "$PROJ/.nbs/chat/myproject.chat" ]] && echo pass || echo fail )"

echo ""

# --- Test 4: Creates scribe log ---
echo "4. Creates scribe log..."
check "scribe log exists" "$( [[ -f "$PROJ/.nbs/scribe/myproject-log.md" ]] && echo pass || echo fail )"

echo ""

# --- Test 5: --help shows usage ---
echo "5. --help shows usage..."
OUTPUT=$("$NBS_CHAT_INIT" --help 2>&1) || true
check "--help shows usage" "$( echo "$OUTPUT" | grep -qi 'usage\|nbs-chat-init' && echo pass || echo fail )"

echo ""

# --- Test 6: Missing --name shows error ---
echo "6. Missing --name shows error..."
set +e
"$NBS_CHAT_INIT" --root="$TEST_DIR/proj_noname" 2>&1 >/dev/null
RC=$?
set -e
check "missing --name exits non-zero" "$( [[ $RC -ne 0 ]] && echo pass || echo fail )"

echo ""

# --- Test 7: Double-init is safe ---
echo "7. Double-init is safe..."
PROJ="$TEST_DIR/proj7"
mkdir -p "$PROJ"
"$NBS_CHAT_INIT" --name=double --root="$PROJ" --force 2>&1 >/dev/null || true
set +e
"$NBS_CHAT_INIT" --name=double --root="$PROJ" --force 2>&1 >/dev/null
RC=$?
set -e
check "double-init exits 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "chat file still exists" "$( [[ -f "$PROJ/.nbs/chat/double.chat" ]] && echo pass || echo fail )"

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
