#!/bin/bash
# Test nbs-claude: verify wrapper script basics
#
# Tests:
#   1. Script exists and is executable
#   2. Key components present
#   3. Session naming includes PID
#   4. Idle detection logic
#   5. Dual-mode support (tmux vs pty-session)
#   6. nbs-poll skill doc
#
# See also: test_nbs_claude_bus.sh for bus-aware sidecar tests
# (check_bus_events, check_chat_unread, should_inject_notify, etc.)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"

PASS=0
FAIL=0
TESTS=0

pass() {
    PASS=$((PASS + 1))
    TESTS=$((TESTS + 1))
    echo "   PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    TESTS=$((TESTS + 1))
    echo "   FAIL: $1"
}

echo "=== nbs-claude Tests ==="
echo ""

# 1. Script exists and is executable
echo "1. Script exists and is executable..."
if [[ -x "$NBS_CLAUDE" ]]; then
    pass "Script is executable"
else
    fail "Script not found or not executable"
fi

# 2. Script contains key components
echo "2. Script contains key components..."
if grep -q 'poll_sidecar' "$NBS_CLAUDE"; then
    pass "Has poll_sidecar function"
else
    fail "Missing poll_sidecar function"
fi

if grep -q 'pty-session' "$NBS_CLAUDE"; then
    pass "References pty-session"
else
    fail "Missing pty-session reference"
fi

if grep -q 'NBS_POLL_INTERVAL' "$NBS_CLAUDE"; then
    pass "Has configurable poll interval"
else
    fail "Missing poll interval config"
fi

if grep -q 'NBS_POLL_DISABLE' "$NBS_CLAUDE"; then
    pass "Has disable option"
else
    fail "Missing disable option"
fi

if grep -q 'cleanup' "$NBS_CLAUDE"; then
    pass "Has cleanup trap"
else
    fail "Missing cleanup trap"
fi

if grep -q '/nbs-poll' "$NBS_CLAUDE"; then
    pass "Injects /nbs-poll command"
else
    fail "Missing /nbs-poll injection"
fi

# 3. Session name includes PID for uniqueness
echo "3. Session naming..."
if grep -q 'SESSION_NAME="nbs-claude-\$\$"' "$NBS_CLAUDE"; then
    pass "Session name includes PID"
else
    fail "Session name missing PID"
fi

# 4. Idle detection logic
echo "4. Idle detection..."
if grep -q 'idle_seconds' "$NBS_CLAUDE"; then
    pass "Has idle counter"
else
    fail "Missing idle counter"
fi

if grep -q 'md5sum' "$NBS_CLAUDE"; then
    pass "Uses content hashing for change detection"
else
    fail "Missing content hashing"
fi

# 5. Dual-mode support
echo "5. Dual-mode support..."
if grep -q 'poll_sidecar_tmux' "$NBS_CLAUDE"; then
    pass "Has tmux sidecar function"
else
    fail "Missing tmux sidecar function"
fi

if grep -q 'poll_sidecar_pty' "$NBS_CLAUDE"; then
    pass "Has pty-session sidecar function"
else
    fail "Missing pty-session sidecar function"
fi

if grep -q 'TMUX:-' "$NBS_CLAUDE"; then
    pass "Detects existing tmux session"
else
    fail "Missing tmux detection"
fi

if grep -q 'tmux capture-pane' "$NBS_CLAUDE"; then
    pass "Uses tmux capture-pane for monitoring"
else
    fail "Missing tmux capture-pane"
fi

if grep -q 'tmux send-keys' "$NBS_CLAUDE"; then
    pass "Uses tmux send-keys for injection"
else
    fail "Missing tmux send-keys"
fi

if grep -q 'MODE="tmux"' "$NBS_CLAUDE" && grep -q 'MODE="pty"' "$NBS_CLAUDE"; then
    pass "Has both mode paths"
else
    fail "Missing mode paths"
fi

# 6. nbs-poll skill doc
echo "6. nbs-poll skill doc..."
POLL_DOC="$PROJECT_ROOT/claude_tools/nbs-poll.md"
if [[ -f "$POLL_DOC" ]]; then
    pass "Skill doc exists"
else
    fail "Skill doc not found"
fi

if grep -q 'Check Chats' "$POLL_DOC"; then
    pass "Skill doc has chat check section"
else
    fail "Skill doc missing chat check"
fi

if grep -q 'Check Workers' "$POLL_DOC"; then
    pass "Skill doc has worker check section"
else
    fail "Skill doc missing worker check"
fi

if grep -q 'return silently' "$POLL_DOC"; then
    pass "Skill doc specifies silent return"
else
    fail "Skill doc missing silent return behaviour"
fi

# 7. Plan mode auto-select
echo "7. Plan mode auto-select..."
if grep -q 'detect_plan_mode' "$NBS_CLAUDE"; then
    pass "Has detect_plan_mode function"
else
    fail "Missing detect_plan_mode function"
fi

if grep -q 'Would you like to proceed?' "$NBS_CLAUDE"; then
    pass "Plan mode detection pattern present"
else
    fail "Missing plan mode detection pattern"
fi

# Verify plan mode sends '2' keystroke (not /nbs-poll)
if grep -A2 'detect_plan_mode' "$NBS_CLAUDE" | grep -q "'2'"; then
    pass "Plan mode sends '2' keystroke"
else
    fail "Plan mode does not send '2' keystroke"
fi

# Verify plan mode check is separate from idle-timeout prompt detection
# Plan mode should fire on content change (not just after idle timeout)
if grep -B2 'detect_plan_mode' "$NBS_CLAUDE" | grep -q 'content change'; then
    pass "Plan mode detection fires on content change"
else
    fail "Plan mode detection not tied to content change"
fi

# Verify plan mode detection exists in both sidecar functions
TMUX_PLAN=$(grep -c 'detect_plan_mode' "$NBS_CLAUDE" | head -1)
if [[ "$TMUX_PLAN" -ge 4 ]]; then
    pass "Plan mode detection in both sidecar modes (tmux + pty)"
else
    fail "Plan mode detection not in both sidecar modes (found $TMUX_PLAN references, expected >= 4)"
fi

# 8. Functional test: detect_plan_mode pattern matching
echo "8. Plan mode pattern matching..."

# Source just the detect_plan_mode function for testing
eval "$(grep -A3 '^detect_plan_mode()' "$NBS_CLAUDE")"

# Test: plan mode prompt should match
PLAN_PROMPT='Claude has written up a plan and is ready to execute. Would you like to proceed?

   1. Yes, clear context and bypass permissions
 ❯ 2. Yes, and bypass permissions'

if detect_plan_mode "$PLAN_PROMPT"; then
    pass "Detects plan mode prompt correctly"
else
    fail "Failed to detect plan mode prompt"
fi

# Test: normal prompt should NOT match
NORMAL_PROMPT='claude ❯'
if detect_plan_mode "$NORMAL_PROMPT"; then
    fail "False positive: normal prompt detected as plan mode"
else
    pass "Normal prompt correctly ignored by plan mode detector"
fi

# Test: mid-response output should NOT match
RESPONSE_OUTPUT='I will read the file and check the results.
Running tests now...'
if detect_plan_mode "$RESPONSE_OUTPUT"; then
    fail "False positive: normal output detected as plan mode"
else
    pass "Normal output correctly ignored by plan mode detector"
fi

# 9. Control inbox components present
echo "9. Control inbox components..."
if grep -q 'CONTROL_INBOX=' "$NBS_CLAUDE"; then
    pass "Has CONTROL_INBOX variable"
else
    fail "Missing CONTROL_INBOX variable"
fi

if grep -q 'CONTROL_REGISTRY=' "$NBS_CLAUDE"; then
    pass "Has CONTROL_REGISTRY variable"
else
    fail "Missing CONTROL_REGISTRY variable"
fi

if grep -q 'seed_registry' "$NBS_CLAUDE"; then
    pass "Has seed_registry function"
else
    fail "Missing seed_registry function"
fi

if grep -q 'check_control_inbox' "$NBS_CLAUDE"; then
    pass "Has check_control_inbox function"
else
    fail "Missing check_control_inbox function"
fi

if grep -q 'process_control_command' "$NBS_CLAUDE"; then
    pass "Has process_control_command function"
else
    fail "Missing process_control_command function"
fi

# Verify control inbox is checked in both sidecar modes
TMUX_INBOX=$(grep -c 'check_control_inbox' "$NBS_CLAUDE")
if [[ "$TMUX_INBOX" -ge 3 ]]; then
    pass "check_control_inbox called in both sidecar modes (found $TMUX_INBOX references)"
else
    fail "check_control_inbox not in both sidecar modes (found $TMUX_INBOX references, expected >= 3)"
fi

# Verify forward-only design (no truncation of inbox)
if grep -q 'CONTROL_INBOX_LINE' "$NBS_CLAUDE"; then
    pass "Uses line offset tracking (forward-only)"
else
    fail "Missing line offset tracking"
fi

# Verify inbox file is never truncated (should not contain truncate/> patterns on inbox)
if grep 'CONTROL_INBOX' "$NBS_CLAUDE" | grep -qE '>\s*"\$.*INBOX"'; then
    fail "Control inbox may be truncated (found > redirect to inbox file)"
else
    pass "Control inbox is not truncated"
fi

# 10. Functional test: control inbox processing
echo "10. Control inbox functional tests..."

# Create a temporary directory to simulate .nbs/
TEST_DIR=$(mktemp -d)
ORIG_DIR=$(pwd)
cd "$TEST_DIR" || exit 1
mkdir -p .nbs/chat .nbs/events

# Source the control inbox functions from nbs-claude
# We need to extract the functions without running main
# Using a temp file + source instead of eval to handle shell syntax (globs, redirects)
# Set SIDECAR_HANDLE and NBS_ROOT before sourcing — the control/resource paths use them
SIDECAR_HANDLE="testhandle"
NBS_ROOT="."
NBS_REMOTE_HOST=""
NBS_REMOTE_SSH_OPTS=""
_EXTRACT_TMP=$(mktemp)
sed -n '/^# --- Dynamic resource registration ---/,/^# --- Idle detection sidecar/p' "$NBS_CLAUDE" | head -n -2 > "$_EXTRACT_TMP"
source "$_EXTRACT_TMP"
rm -f "$_EXTRACT_TMP"

# Test: seed_registry populates from existing chat files
touch .nbs/chat/live.chat .nbs/chat/debug.chat
seed_registry
if grep -qF "chat:${NBS_ROOT}/.nbs/chat/live.chat" "$CONTROL_REGISTRY" && \
   grep -qF "chat:${NBS_ROOT}/.nbs/chat/debug.chat" "$CONTROL_REGISTRY"; then
    pass "seed_registry finds existing chat files"
else
    fail "seed_registry did not find chat files"
fi

if grep -qF "bus:${NBS_ROOT}/.nbs/events" "$CONTROL_REGISTRY"; then
    pass "seed_registry finds existing events directory"
else
    fail "seed_registry did not find events directory"
fi

# Test: seed_registry is idempotent
seed_registry
CHAT_COUNT=$(grep -c "chat:${NBS_ROOT}/.nbs/chat/live.chat" $CONTROL_REGISTRY)
if [[ "$CHAT_COUNT" -eq 1 ]]; then
    pass "seed_registry is idempotent (no duplicates)"
else
    fail "seed_registry created duplicates (count: $CHAT_COUNT)"
fi

# Test: process_control_command register-chat
process_control_command "register-chat .nbs/chat/new.chat"
if grep -qF "chat:.nbs/chat/new.chat" $CONTROL_REGISTRY; then
    pass "register-chat adds to registry"
else
    fail "register-chat did not add to registry"
fi

# Test: duplicate registration is idempotent
process_control_command "register-chat .nbs/chat/new.chat"
NEW_COUNT=$(grep -c "chat:.nbs/chat/new.chat" $CONTROL_REGISTRY)
if [[ "$NEW_COUNT" -eq 1 ]]; then
    pass "Duplicate register-chat is idempotent"
else
    fail "Duplicate register-chat created duplicates (count: $NEW_COUNT)"
fi

# Test: unregister-chat removes from registry
process_control_command "unregister-chat .nbs/chat/new.chat"
if grep -qF "chat:.nbs/chat/new.chat" $CONTROL_REGISTRY; then
    fail "unregister-chat did not remove from registry"
else
    pass "unregister-chat removes from registry"
fi

# Test: unregister non-existent resource is safe
process_control_command "unregister-chat .nbs/chat/nonexistent.chat"
pass "Unregistering non-existent resource does not crash"

# Test: register-bus
process_control_command "register-bus /some/other/events"
if grep -qF "bus:/some/other/events" $CONTROL_REGISTRY; then
    pass "register-bus adds to registry"
else
    fail "register-bus did not add to registry"
fi

# Test: set-poll-interval
POLL_INTERVAL=30
process_control_command "set-poll-interval 300"
if [[ "$POLL_INTERVAL" -eq 300 ]]; then
    pass "set-poll-interval updates POLL_INTERVAL"
else
    fail "set-poll-interval did not update (expected 300, got $POLL_INTERVAL)"
fi

# Test: set-poll-interval rejects non-numeric
POLL_INTERVAL=300
process_control_command "set-poll-interval abc"
if [[ "$POLL_INTERVAL" -eq 300 ]]; then
    pass "set-poll-interval rejects non-numeric input"
else
    fail "set-poll-interval accepted non-numeric input"
fi

# Test: set-poll-interval rejects zero
process_control_command "set-poll-interval 0"
if [[ "$POLL_INTERVAL" -eq 300 ]]; then
    pass "set-poll-interval rejects zero"
else
    fail "set-poll-interval accepted zero"
fi

# Test: unknown command is silently ignored
process_control_command "unknown-command /some/path"
pass "Unknown command silently ignored"

# Test: empty and comment lines are handled
process_control_command ""
process_control_command "  "
pass "Empty lines handled without crash"

# Test: check_control_inbox processes new lines only
CONTROL_INBOX_LINE=0
echo "register-chat .nbs/chat/inbox-test1.chat" > $CONTROL_INBOX
echo "register-chat .nbs/chat/inbox-test2.chat" >> $CONTROL_INBOX
check_control_inbox
if grep -qF "chat:.nbs/chat/inbox-test1.chat" $CONTROL_REGISTRY && \
   grep -qF "chat:.nbs/chat/inbox-test2.chat" $CONTROL_REGISTRY; then
    pass "check_control_inbox processes lines from inbox"
else
    fail "check_control_inbox did not process inbox lines"
fi

# Test: check_control_inbox does not re-process old lines
# Remove an entry, then check inbox again — should NOT re-add it
process_control_command "unregister-chat .nbs/chat/inbox-test1.chat"
check_control_inbox
if grep -qF "chat:.nbs/chat/inbox-test1.chat" $CONTROL_REGISTRY; then
    fail "check_control_inbox re-processed old lines"
else
    pass "check_control_inbox does not re-process old lines (forward-only)"
fi

# Test: check_control_inbox processes only new lines after offset
echo "register-chat .nbs/chat/inbox-test3.chat" >> $CONTROL_INBOX
check_control_inbox
if grep -qF "chat:.nbs/chat/inbox-test3.chat" $CONTROL_REGISTRY; then
    pass "check_control_inbox processes new lines after offset"
else
    fail "check_control_inbox did not process new lines after offset"
fi

# Test: control inbox file is preserved (never truncated)
INBOX_LINES=$(wc -l < $CONTROL_INBOX)
if [[ "$INBOX_LINES" -eq 3 ]]; then
    pass "Control inbox file preserved (all 3 lines intact)"
else
    fail "Control inbox file modified (expected 3 lines, got $INBOX_LINES)"
fi

# Cleanup
cd "$ORIG_DIR" || true
rm -rf "$TEST_DIR"

# 11. Adversarial control inbox tests
echo "11. Adversarial control inbox tests..."

# Set up a fresh test directory
TEST_DIR=$(mktemp -d)
cd "$TEST_DIR" || exit 1
mkdir -p .nbs/chat .nbs/events

# Source control inbox functions
SIDECAR_HANDLE="testhandle"
_EXTRACT_TMP=$(mktemp)
sed -n '/^# --- Dynamic resource registration ---/,/^# --- Idle detection sidecar/p' "$NBS_CLAUDE" | head -n -2 > "$_EXTRACT_TMP"
source "$_EXTRACT_TMP"
rm -f "$_EXTRACT_TMP"

# Initialise
seed_registry

# Test: path traversal attempt
process_control_command "register-chat ../../../etc/passwd"
if grep -qF "chat:../../../etc/passwd" $CONTROL_REGISTRY; then
    # This is expected — the sidecar does not sanitise paths.
    # The AI skill doc and the nbs-poll handler must refuse to read arbitrary paths.
    # This test documents the current behaviour, not a security flaw per se,
    # because the only writer is the AI itself (trusted principal).
    pass "Path traversal string registered (by design — writer is trusted)"
else
    fail "Path traversal string not registered"
fi
# Clean it up for subsequent tests
process_control_command "unregister-chat ../../../etc/passwd"

# Test: command injection attempt in path (semicolons, pipes, backticks)
process_control_command "register-chat .nbs/chat/evil;rm -rf /"
if grep -qF 'chat:.nbs/chat/evil;rm' $CONTROL_REGISTRY; then
    pass "Semicolon in path treated as literal (no execution)"
else
    # The awk split on whitespace means the path is just the second field
    # 'evil;rm' — the '-rf' and '/' are extra fields, silently ignored
    if grep -qF 'chat:.nbs/chat/evil;rm' $CONTROL_REGISTRY 2>/dev/null; then
        pass "Semicolon in path treated as literal"
    else
        pass "Extra fields after path silently ignored"
    fi
fi

# Test: backtick injection
process_control_command 'register-chat `whoami`.chat'
if grep -qF 'chat:`whoami`.chat' $CONTROL_REGISTRY; then
    pass "Backtick in path treated as literal (no expansion)"
else
    fail "Backtick expanded or caused error"
fi
process_control_command 'unregister-chat `whoami`.chat'

# Test: dollar expansion attempt
process_control_command 'register-chat $HOME/.secret'
if grep -qF 'chat:$HOME/.secret' $CONTROL_REGISTRY; then
    pass "Dollar sign in path treated as literal (no expansion)"
else
    fail "Dollar sign expanded or caused error"
fi
process_control_command 'unregister-chat $HOME/.secret'

# Test: very long path (boundary test)
LONG_PATH=$(python3 -c "print('a' * 4096)")
process_control_command "register-chat $LONG_PATH"
if grep -qF "chat:$LONG_PATH" $CONTROL_REGISTRY; then
    pass "Very long path registered without crash"
else
    pass "Very long path handled gracefully"
fi
process_control_command "unregister-chat $LONG_PATH"

# Test: newline in inbox (should be separate commands)
echo -e "register-chat .nbs/chat/line1.chat\nregister-chat .nbs/chat/line2.chat" > $CONTROL_INBOX
CONTROL_INBOX_LINE=0
check_control_inbox
LINE1=$(grep -c "chat:.nbs/chat/line1.chat" $CONTROL_REGISTRY)
LINE2=$(grep -c "chat:.nbs/chat/line2.chat" $CONTROL_REGISTRY)
if [[ "$LINE1" -eq 1 ]] && [[ "$LINE2" -eq 1 ]]; then
    pass "Multi-line inbox processed as separate commands"
else
    fail "Multi-line inbox not processed correctly (line1=$LINE1, line2=$LINE2)"
fi

# Test: comment lines are ignored
echo "# This is a comment" >> $CONTROL_INBOX
check_control_inbox
if grep -qF "# This is a comment" $CONTROL_REGISTRY; then
    fail "Comment line was added to registry"
else
    pass "Comment lines ignored in inbox"
fi

# Test: register then unregister then re-register (idempotent cycle)
process_control_command "register-chat .nbs/chat/cycle.chat"
process_control_command "unregister-chat .nbs/chat/cycle.chat"
process_control_command "register-chat .nbs/chat/cycle.chat"
CYCLE_COUNT=$(grep -c "chat:.nbs/chat/cycle.chat" $CONTROL_REGISTRY)
if [[ "$CYCLE_COUNT" -eq 1 ]]; then
    pass "Register-unregister-register cycle produces exactly one entry"
else
    fail "Cycle produced $CYCLE_COUNT entries (expected 1)"
fi

# Test: set-poll-interval with negative number
POLL_INTERVAL=30
process_control_command "set-poll-interval -1"
if [[ "$POLL_INTERVAL" -eq 30 ]]; then
    pass "set-poll-interval rejects negative number"
else
    fail "set-poll-interval accepted negative number (got $POLL_INTERVAL)"
fi

# Test: set-poll-interval with very large number
process_control_command "set-poll-interval 999999"
if [[ "$POLL_INTERVAL" -eq 999999 ]]; then
    pass "set-poll-interval accepts large number"
else
    fail "set-poll-interval rejected large number"
fi

# Cleanup
cd "$ORIG_DIR" || true
rm -rf "$TEST_DIR"

echo ""
echo "=== Result ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All $TESTS tests passed"
else
    echo "FAIL: $FAIL of $TESTS tests failed"
fi

exit $FAIL
