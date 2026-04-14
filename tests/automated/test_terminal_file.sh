#!/bin/bash
# Test: nbs-chat-terminal /file command integration
#
# Tests:
#   1. /file appears in help output
#   2. /file appears in tab-completion list
#   3. /file command dispatches (exits cleanly)
#   4. /file with path argument accepted

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"

export PATH="$PROJECT_ROOT/bin:$PATH"

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

echo "=== nbs-chat-terminal /file Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: /file in help ---
echo "1. /file appears in help..."
CHAT="$TEST_DIR/test1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
OUTPUT=$(printf '/help\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/file in help" "$( echo "$OUTPUT" | grep -q '/file' && echo pass || echo fail )"
echo ""

# --- Test 2: /file in binary (completion list) ---
echo "2. /file in completion list..."
check "/file in binary" "$( strings "$NBS_TERMINAL" 2>/dev/null | grep -cF '/file' | grep -q '[1-9]' && echo pass || echo fail )"
echo ""

# --- Test 3: /file dispatches and exits cleanly ---
echo "3. /file dispatches (ESC exits browser)..."
CHAT="$TEST_DIR/test3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
mkdir -p "$TEST_DIR/browse_target"
echo "test" > "$TEST_DIR/browse_target/file.txt"
# Send /file with path, then ESC to exit browser, then /exit
OUTPUT=$(printf "/file $TEST_DIR/browse_target\n\x1b\n/exit\n" | timeout 10 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/file exits cleanly" "$( echo "$OUTPUT" | grep -q 'Left chat' && echo pass || echo fail )"
echo ""

# --- Test 4: /file without path exits cleanly ---
echo "4. /file no path exits cleanly..."
CHAT="$TEST_DIR/test4.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
# ESC exits browser, then /exit
OUTPUT=$(printf '/file\n\x1b\n/exit\n' | timeout 10 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/file no path ok" "$( echo "$OUTPUT" | grep -q 'Left chat' && echo pass || echo fail )"
echo ""

# --- Test 5: nbs-file-browser binary is installed ---
echo "5. nbs-file-browser is installed..."
check "binary installed" "$( command -v nbs-file-browser >/dev/null && echo pass || echo fail )"
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
