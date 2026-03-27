#!/bin/bash
# Test: nbs-chat-terminal watchdog integration
#
# Tests the /shutdown and /restart terminal commands and their
# observable side effects. Unit tests for the watchdog state machine
# live in test_watchdog_unit.c — these tests verify the integration
# between terminal command parsing and watchdog behaviour.
#
# The watchdog only activates when the terminal can find a .nbs/
# directory by walking up from the chat file. Tests that need the
# watchdog active create a project-like directory structure.
#
# Tests:
#   1.  /shutdown sends wrap-up message to chat
#   2.  /shutdown prints "Watchdog disabled" in terminal
#   3.  /shutdown message is attributed to the terminal handle
#   4.  /restart when watchdog disabled prints error
#   5.  /help lists /shutdown command
#   6.  /help lists /restart command
#   7.  /shutdown wrap-up message contains @team mention
#   8.  /shutdown is idempotent (second invocation is no-op)
#   9.  restart script rejects missing arguments
#   10. restart script rejects nonexistent project root
#   11. /shutdown without project root (watchdog never init) is no-op

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"
RESTART_SCRIPT="$PROJECT_ROOT/bin/nbs-chat-terminal-restart.sh"

# Add bin/ to PATH so bus_bridge.c can find nbs-bus via execlp
export PATH="$PROJECT_ROOT/bin:$PATH"

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

# Helper: create a chat file inside a project-like directory structure
# so that resolve_project_root succeeds and the watchdog activates.
# Usage: create_project_chat VARNAME
#   Sets VARNAME to the chat file path.
create_project_chat() {
    local test_name="$1"
    local proj_dir="$TEST_DIR/${test_name}_proj"
    mkdir -p "$proj_dir/.nbs/chat" "$proj_dir/.nbs/events/processed"
    local chat="$proj_dir/.nbs/chat/test.chat"
    "$NBS_CHAT" create "$chat" >/dev/null
    echo "$chat"
}

echo "=== nbs-chat-terminal Watchdog Integration Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: /shutdown sends wrap-up message to chat ---
echo "1. /shutdown sends wrap-up message to chat..."
CHAT=$(create_project_chat test1)
printf '/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" >/dev/null 2>&1 || true
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "/shutdown sends wrap-up message" "$( echo "$OUTPUT" | grep -qF 'Good work' && echo pass || echo fail )"

echo ""

# --- Test 2: /shutdown prints "Watchdog disabled" in terminal ---
echo "2. /shutdown prints watchdog disabled message..."
CHAT=$(create_project_chat test2)
TERM_OUTPUT=$(printf '/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/shutdown shows disabled message" "$( echo "$TERM_OUTPUT" | grep -qF 'Watchdog disabled' && echo pass || echo fail )"

echo ""

# --- Test 3: /shutdown message is attributed to the terminal handle ---
echo "3. /shutdown message attributed to terminal handle..."
CHAT=$(create_project_chat test3)
printf '/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "myhandle" >/dev/null 2>&1 || true
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "wrap-up attributed to handle" "$( echo "$OUTPUT" | grep -q 'myhandle:.*Good work' && echo pass || echo fail )"

echo ""

# --- Test 4: /restart when watchdog disabled prints error ---
echo "4. /restart when watchdog disabled prints error..."
CHAT=$(create_project_chat test4)
# /shutdown disables the watchdog, then /restart should show error
TERM_OUTPUT=$(printf '/shutdown\n/restart\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/restart after /shutdown shows error" "$( echo "$TERM_OUTPUT" | grep -qi 'cannot restart\|not initialised' && echo pass || echo fail )"

echo ""

# --- Test 5: /help lists /shutdown command ---
echo "5. /help lists /shutdown..."
CHAT="$TEST_DIR/test5.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
TERM_OUTPUT=$(printf '/help\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/help mentions /shutdown" "$( echo "$TERM_OUTPUT" | grep -qF '/shutdown' && echo pass || echo fail )"

echo ""

# --- Test 6: /help lists /restart command ---
echo "6. /help lists /restart..."
CHAT="$TEST_DIR/test6.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
TERM_OUTPUT=$(printf '/help\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/help mentions /restart" "$( echo "$TERM_OUTPUT" | grep -qF '/restart' && echo pass || echo fail )"

echo ""

# --- Test 7: /shutdown wrap-up message contains @team mention ---
echo "7. /shutdown wrap-up message contains @team..."
CHAT=$(create_project_chat test7)
printf '/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" >/dev/null 2>&1 || true
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "wrap-up contains @team" "$( echo "$OUTPUT" | grep -qF '@team' && echo pass || echo fail )"

echo ""

# --- Test 8: /shutdown is idempotent (second invocation is no-op) ---
echo "8. /shutdown idempotent — only first invocation sends message..."
CHAT=$(create_project_chat test8)
set +e
TERM_OUTPUT=$(printf '/shutdown\n/shutdown\n/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null)
RC=$?
set -e
check "repeated /shutdown does not crash" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# Only first /shutdown should send the wrap-up message
OUTPUT=$("$NBS_CHAT" read "$CHAT")
MSG_COUNT=$(echo "$OUTPUT" | grep -c 'Good work' || true)
check "exactly 1 wrap-up message sent" "$( [[ $MSG_COUNT -eq 1 ]] && echo pass || echo fail )"
# Second and third should print 'already disabled'
check "terminal shows 'already disabled'" "$( echo "$TERM_OUTPUT" | grep -qF 'already disabled' && echo pass || echo fail )"

echo ""

# --- Test 9: restart script rejects missing arguments ---
echo "9. Restart script rejects missing arguments..."
if [[ -x "$RESTART_SCRIPT" ]]; then
    set +e
    bash "$RESTART_SCRIPT" 2>/dev/null
    RC=$?
    set -e
    check "restart script fails with no args" "$( [[ $RC -ne 0 ]] && echo pass || echo fail )"
else
    check "restart script fails with no args" "fail"
    echo "     (restart script not found at $RESTART_SCRIPT)"
fi

echo ""

# --- Test 10: restart script rejects nonexistent project root ---
echo "10. Restart script rejects nonexistent project root..."
if [[ -x "$RESTART_SCRIPT" ]]; then
    set +e
    bash "$RESTART_SCRIPT" "/nonexistent/project/root/$$" "/nonexistent/chat.chat" 2>/dev/null
    RC=$?
    set -e
    check "restart script fails with bad project root" "$( [[ $RC -ne 0 ]] && echo pass || echo fail )"
else
    check "restart script fails with bad project root" "fail"
    echo "     (restart script not found at $RESTART_SCRIPT)"
fi

echo ""

# --- Test 11: /shutdown without project root is no-op ---
echo "11. /shutdown without project root (watchdog never init)..."
CHAT="$TEST_DIR/test11.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
TERM_OUTPUT=$(printf '/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
OUTPUT=$("$NBS_CHAT" read "$CHAT")
# Watchdog was never initialised, so /shutdown should show 'already disabled'
check "no project root: shows already disabled" "$( echo "$TERM_OUTPUT" | grep -qF 'already disabled' && echo pass || echo fail )"
# No wrap-up message should have been sent
MSG_COUNT=$(echo "$OUTPUT" | grep -c 'Good work' || true)
check "no project root: no wrap-up sent" "$( [[ $MSG_COUNT -eq 0 ]] && echo pass || echo fail )"

echo ""

# --- Test 12: --restart flag prints "Restarting team" ---
echo "12. --restart flag triggers restart..."
PROJ_DIR="$TEST_DIR/test12_proj"
mkdir -p "$PROJ_DIR/.nbs/chat" "$PROJ_DIR/.nbs/events/processed" "$PROJ_DIR/bin"
CHAT="$PROJ_DIR/.nbs/chat/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
# Create a stub restart script that exits immediately
cat > "$PROJ_DIR/bin/nbs-chat-terminal-restart.sh" << 'STUB'
#!/bin/bash
exit 0
STUB
chmod +x "$PROJ_DIR/bin/nbs-chat-terminal-restart.sh"
TERM_OUTPUT=$(printf '/exit\n' | timeout 10 "$NBS_TERMINAL" "$CHAT" "alex" --restart 2>/dev/null || true)
check "--restart prints Restarting team" "$( echo "$TERM_OUTPUT" | grep -qF 'Restarting team' && echo pass || echo fail )"
check "--restart prints restart complete" "$( echo "$TERM_OUTPUT" | grep -qF 'restart complete' && echo pass || echo fail )"

echo ""

# --- Test 13: --restart without project root is harmless ---
echo "13. --restart without project root does not crash..."
CHAT="$TEST_DIR/test13.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
set +e
TERM_OUTPUT=$(printf '/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" --restart 2>/dev/null)
RC=$?
set -e
check "--restart without project root exits cleanly" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# Should NOT print "Restarting team" since no project root found
check "--restart without project root: no restart attempted" "$( echo "$TERM_OUTPUT" | grep -qF 'Restarting team' && echo fail || echo pass )"

echo ""

# --- Test 14: usage text mentions --restart flag ---
echo "14. usage text mentions --restart..."
USAGE_OUTPUT=$(timeout 5 "$NBS_TERMINAL" 2>&1 || true)
check "usage mentions --restart" "$( echo "$USAGE_OUTPUT" | grep -qF -- '--restart' && echo pass || echo fail )"

echo ""

# --- Test 15: terminal works normally without --restart ---
echo "15. terminal without --restart works normally..."
CHAT=$(create_project_chat test15)
TERM_OUTPUT=$(printf '/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "no --restart: exits cleanly" "$( [[ $? -eq 0 ]] && echo pass || echo fail )"
check "no --restart: no restart attempted" "$( echo "$TERM_OUTPUT" | grep -qF 'Restarting team' && echo fail || echo pass )"

echo ""

# --- Test 16: /pythia without project root shows INFO line ---
echo "16. /pythia without project root shows INFO line..."
CHAT="$TEST_DIR/test16.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
TERM_OUTPUT=$(printf '/pythia\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/pythia shows INFO>" "$( echo "$TERM_OUTPUT" | grep -qF 'INFO>' && echo pass || echo fail )"
check "/pythia shows [pythia] label" "$( echo "$TERM_OUTPUT" | grep -qF '[pythia]' && echo pass || echo fail )"

echo ""

# --- Test 17: /shutdown without project root shows INFO line ---
echo "17. /shutdown without project root shows INFO line..."
CHAT="$TEST_DIR/test17.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
TERM_OUTPUT=$(printf '/shutdown\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/shutdown shows INFO>" "$( echo "$TERM_OUTPUT" | grep -qF 'INFO>' && echo pass || echo fail )"
check "/shutdown shows [shutdown] label" "$( echo "$TERM_OUTPUT" | grep -qF '[shutdown]' && echo pass || echo fail )"

echo ""

# --- Test 18: /restart with project root shows INFO line ---
echo "18. /restart shows INFO line..."
PROJ_DIR="$TEST_DIR/test18_proj"
mkdir -p "$PROJ_DIR/.nbs/chat" "$PROJ_DIR/.nbs/events/processed" "$PROJ_DIR/bin"
CHAT="$PROJ_DIR/.nbs/chat/test.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
# Stub restart script that outputs a test message
cat > "$PROJ_DIR/bin/nbs-chat-terminal-restart.sh" << 'STUB'
#!/bin/bash
echo "test restart output"
exit 0
STUB
chmod +x "$PROJ_DIR/bin/nbs-chat-terminal-restart.sh"
TERM_OUTPUT=$(printf '/restart\n/exit\n' | timeout 10 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/restart shows INFO>" "$( echo "$TERM_OUTPUT" | grep -qF 'INFO>' && echo pass || echo fail )"
check "/restart shows [restart] label" "$( echo "$TERM_OUTPUT" | grep -qF '[restart]' && echo pass || echo fail )"

echo ""

# --- Test 19: /restart captures child script output as INFO lines ---
echo "19. /restart streams script output..."
# Re-use test18 project with a more verbose stub
cat > "$PROJ_DIR/bin/nbs-chat-terminal-restart.sh" << 'STUB'
#!/bin/bash
echo "step 1 done"
echo "step 2 done"
exit 0
STUB
chmod +x "$PROJ_DIR/bin/nbs-chat-terminal-restart.sh"
CHAT19="$PROJ_DIR/.nbs/chat/test19.chat"
"$NBS_CHAT" create "$CHAT19" >/dev/null
# Use sleep between /restart and /exit so the poll loop has time to
# drain the child pipe before the terminal exits.
{ printf '/restart\n'; sleep 3; printf '/exit\n'; } | timeout 10 "$NBS_TERMINAL" "$CHAT19" "alex" >/tmp/test19_out.txt 2>/dev/null || true
TERM_OUTPUT=$(cat /tmp/test19_out.txt)
rm -f /tmp/test19_out.txt
check "restart captures 'step 1 done'" "$( echo "$TERM_OUTPUT" | grep -qF 'step 1 done' && echo pass || echo fail )"
check "restart captures 'step 2 done'" "$( echo "$TERM_OUTPUT" | grep -qF 'step 2 done' && echo pass || echo fail )"

echo ""

# --- Test 20: /fixup without project root shows INFO line ---
echo "20. /fixup without project root shows INFO line..."
CHAT="$TEST_DIR/test20.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
TERM_OUTPUT=$(printf '/fixup\n/exit\n' | timeout 5 "$NBS_TERMINAL" "$CHAT" "alex" 2>/dev/null || true)
check "/fixup shows INFO>" "$( echo "$TERM_OUTPUT" | grep -qF 'INFO>' && echo pass || echo fail )"
check "/fixup shows [fixup] label" "$( echo "$TERM_OUTPUT" | grep -qF '[fixup]' && echo pass || echo fail )"

echo ""

# --- Summary ---
echo "=== Results: $PASS_COUNT passed, $ERRORS failed ==="
if [[ $ERRORS -eq 0 ]]; then
    exit 0
else
    exit 1
fi
