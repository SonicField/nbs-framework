#!/bin/bash
# Test: nbs-chat-terminal /bash command
#
# Tests:
#   1. /bash appears in help output
#   2. /bash appears in tab-completion list
#   3. /bash interactive mode exits cleanly
#   4. Binary links libutil (forkpty)
#   5. /bash command dispatch is wired (grep source)
#
# NOTE: /bash <command> captured mode cannot be tested via stdin piping
# because the pager reads from the terminal in raw mode, not from stdin.
# Those tests require interactive verification or nbs-ts-based testing.

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

echo "=== nbs-chat-terminal /bash Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: /bash in help output ---
echo "1. /bash appears in help..."
CHAT="$TEST_DIR/test1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
OUTPUT=$(printf '/help\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/bash in help" "$( echo "$OUTPUT" | grep -q '/bash' && echo pass || echo fail )"
echo ""

# --- Test 2: /bash in command completion list ---
echo "2. /bash in completion list..."
check "/bash in binary" "$( strings "$NBS_TERMINAL" 2>/dev/null | grep -cF '/bash' | grep -q '[1-9]' && echo pass || echo fail )"
echo ""

# --- Test 3: /bash interactive mode exits cleanly ---
echo "3. /bash interactive exits on 'exit'..."
CHAT="$TEST_DIR/test3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
# Enter interactive bash, immediately exit, then exit terminal
OUTPUT=$(printf '/bash\nexit\n/exit\n' | timeout 10 "$NBS_TERMINAL" "$CHAT" "viewer" 2>/dev/null) || true
check "/bash interactive exit" "$( echo "$OUTPUT" | grep -q 'Left chat' && echo pass || echo fail )"
echo ""

# --- Test 4: forkpty linked ---
echo "4. forkpty symbol available in binary..."
check "forkpty symbol" "$( nm -D "$NBS_TERMINAL" 2>/dev/null | grep -q 'forkpty' && echo pass || echo fail )"
echo ""

# --- Test 5: /bash dispatch is wired in source ---
echo "5. /bash dispatch present in source..."
TERM_SRC="$PROJECT_ROOT/src/nbs-chat/terminal.c"
check "/bash strcmp dispatch" "$( grep -qF 'strcmp(edit.buf, "/bash")' "$TERM_SRC" && echo pass || echo fail )"
check "/bash strncmp dispatch" "$( grep -qF 'strncmp(edit.buf, "/bash "' "$TERM_SRC" && echo pass || echo fail )"
check "forkpty call" "$( grep -q 'forkpty' "$TERM_SRC" && echo pass || echo fail )"
check "g_bash_cwd state" "$( grep -q 'g_bash_cwd' "$TERM_SRC" && echo pass || echo fail )"
check "pty.h include" "$( grep -q '#include <pty.h>' "$TERM_SRC" && echo pass || echo fail )"
echo ""

# --- Test 6: CWD tracking variable exists ---
echo "6. CWD tracking in place..."
check "Running status in binary" "$( strings "$NBS_TERMINAL" 2>/dev/null | grep -cF 'Running' | grep -q '[1-9]' && echo pass || echo fail )"
echo ""

# --- Test 7: Makefile links -lutil ---
echo "7. Makefile links -lutil..."
check "-lutil in Makefile" "$( grep -q 'lutil' "$PROJECT_ROOT/src/nbs-chat/Makefile" && echo pass || echo fail )"
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
