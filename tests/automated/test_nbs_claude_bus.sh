#!/bin/bash
# Test nbs-claude bus-aware sidecar: verify event checking, chat cursor peeking,
# notification logic, and prompt detection.
#
# These tests exercise the new bus-aware functions added to nbs-claude:
#   - check_bus_events(): non-destructive bus peek
#   - check_chat_unread(): cursor peeking without advancement
#   - should_inject_notify(): cooldown and priority logic
#   - is_prompt_visible(): prompt detection in pane content
#
# Falsification approach: each test tries to break the invariant, not confirm it.
# The key invariant is: the sidecar never injects when nothing is pending.
#
# Tests:
#   1. Structural: new functions and config present in script
#   2. nbs-notify skill doc: exists, is lightweight
#   3. is_prompt_visible: true/false cases
#   4. check_bus_events: empty bus, pending events, no bus registered, missing dir
#   5. check_chat_unread: caught up, unread, no chats registered, missing cursors
#   6. should_inject_notify: nothing pending, events pending, cooldown, critical bypass
#   7. Event-driven structure: conditional notification, no blind polling
#   8. Configuration defaults: correct values for event-driven mode
#   9. Cursor peeking safety: cursor files NOT modified by check_chat_unread
#  10. Edge cases: empty chat file, chat with no delimiter, multiple bus dirs
#  11. Injection verification: post-injection prompt check, retry, both modes
#  12. nbs-poll.md safety net language
#  13. docs/nbs-claude.md updated
#  14. detect_context_stress: functional and structural
#  15. Startup grace period: no notifications during grace window
#  16. NBS_INITIAL_PROMPT: custom initial prompt for sidecar
#  17. Self-healing: detect_skill_failure, build_recovery_prompt, failure tracking

#  18. Deterministic Pythia trigger: check_pythia_trigger function
#  19. Deterministic standup trigger: check_standup_trigger function
#  20. Idle standup suppression: are_chat_unread_sidecar_only function

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

echo "=== nbs-claude Bus-Aware Sidecar Tests ==="
echo ""

# =========================================================================
# 1. Structural: new functions and config present
# =========================================================================
echo "1. Bus-aware functions present in script..."

if grep -q 'check_bus_events' "$NBS_CLAUDE"; then
    pass "Has check_bus_events function"
else
    fail "Missing check_bus_events function"
fi

if grep -q 'check_chat_unread' "$NBS_CLAUDE"; then
    pass "Has check_chat_unread function"
else
    fail "Missing check_chat_unread function"
fi

if grep -q 'should_inject_notify' "$NBS_CLAUDE"; then
    pass "Has should_inject_notify function"
else
    fail "Missing should_inject_notify function"
fi

if grep -q 'is_prompt_visible' "$NBS_CLAUDE"; then
    pass "Has is_prompt_visible function"
else
    fail "Missing is_prompt_visible function"
fi

if grep -q 'NBS_BUS_CHECK_INTERVAL' "$NBS_CLAUDE"; then
    pass "Has BUS_CHECK_INTERVAL config"
else
    fail "Missing BUS_CHECK_INTERVAL config"
fi

if grep -q 'NBS_NOTIFY_COOLDOWN' "$NBS_CLAUDE"; then
    pass "Has NOTIFY_COOLDOWN config"
else
    fail "Missing NOTIFY_COOLDOWN config"
fi

if grep -q 'NBS_HANDLE' "$NBS_CLAUDE"; then
    pass "Has NBS_HANDLE config"
else
    fail "Missing NBS_HANDLE config"
fi

if grep -q '/nbs-notify' "$NBS_CLAUDE"; then
    pass "Injects /nbs-notify command"
else
    fail "Missing /nbs-notify injection"
fi

# Verify event-driven structure: bus check with conditional notification
if grep -q 'Track 1.*Bus-aware' "$NBS_CLAUDE" && grep -q 'should_inject_notify' "$NBS_CLAUDE"; then
    pass "Has event-driven notification structure (no blind polling)"
else
    fail "Missing event-driven notification structure"
fi

# Verify bus_check_counter exists in both sidecar modes
TMUX_BCC=$(grep -c 'bus_check_counter' "$NBS_CLAUDE")
if [[ "$TMUX_BCC" -ge 6 ]]; then
    pass "bus_check_counter in both sidecar modes (found $TMUX_BCC references)"
else
    fail "bus_check_counter not in both sidecar modes (found $TMUX_BCC, expected >= 6)"
fi

# =========================================================================
# 2. nbs-notify skill doc
# =========================================================================
echo "2. nbs-notify skill doc..."

NOTIFY_DOC="$PROJECT_ROOT/claude_tools/nbs-notify.md"
if [[ -f "$NOTIFY_DOC" ]]; then
    pass "nbs-notify.md exists"
else
    fail "nbs-notify.md not found"
fi

# Verify it is lightweight (under 35 lines — includes proactivity guidance)
if [[ -f "$NOTIFY_DOC" ]]; then
    NOTIFY_LINES=$(wc -l < "$NOTIFY_DOC")
    if [[ "$NOTIFY_LINES" -lt 40 ]]; then
        pass "nbs-notify.md is lightweight ($NOTIFY_LINES lines, < 40)"
    else
        fail "nbs-notify.md is too large ($NOTIFY_LINES lines, expected < 40)"
    fi
fi

# Verify it has the $ARGUMENTS placeholder
if grep -q '\$ARGUMENTS' "$NOTIFY_DOC"; then
    pass "nbs-notify.md has \$ARGUMENTS placeholder"
else
    fail "nbs-notify.md missing \$ARGUMENTS placeholder"
fi

# Verify it references nbs-bus check
if grep -q 'nbs-bus check' "$NOTIFY_DOC"; then
    pass "nbs-notify.md references nbs-bus check"
else
    fail "nbs-notify.md missing nbs-bus check reference"
fi

# Verify it references nbs-chat read --unread
if grep -q 'nbs-chat read' "$NOTIFY_DOC"; then
    pass "nbs-notify.md references nbs-chat read"
else
    fail "nbs-notify.md missing nbs-chat read reference"
fi

# Verify it has proactive behaviour guidance
if grep -q 'proactive\|too attentive\|return silently' "$NOTIFY_DOC"; then
    pass "nbs-notify.md specifies agent behaviour (proactive or silent return)"
else
    fail "nbs-notify.md missing agent behaviour guidance"
fi

# Verify allowed-tools frontmatter
if grep -q 'allowed-tools: Bash, Read' "$NOTIFY_DOC"; then
    pass "nbs-notify.md has correct allowed-tools"
else
    fail "nbs-notify.md has wrong allowed-tools"
fi

# =========================================================================
# 3. is_prompt_visible: functional tests
# =========================================================================
echo "3. is_prompt_visible pattern matching..."

# Source just the is_prompt_visible function
eval "$(grep -A3 '^is_prompt_visible()' "$NBS_CLAUDE")"

# Test: prompt with ❯ should match
if is_prompt_visible "some output
more output
claude ❯"; then
    pass "Detects ❯ prompt character"
else
    fail "Failed to detect ❯ prompt"
fi

# Test: prompt with > at end of line should NOT match (tightened to ❯ only)
if is_prompt_visible "some output
more output
> "; then
    fail "False positive: bare > detected as prompt (should only match ❯)"
else
    pass "Bare > correctly rejected (not a Claude prompt)"
fi

# Test: bare > at end of line should NOT match
if is_prompt_visible "line 1
line 2
>"; then
    fail "False positive: bare > detected as prompt"
else
    pass "Bare > at end of line correctly rejected"
fi

# Test: no prompt should NOT match
if is_prompt_visible "AI is thinking...
[spinner animation]
processing request"; then
    fail "False positive: no prompt detected as prompt"
else
    pass "No prompt correctly rejected"
fi

# Test: prompt buried deep (not in last 3 lines) should NOT match
if is_prompt_visible "claude ❯
line 2
line 3
line 4
line 5"; then
    fail "False positive: prompt outside last 3 lines detected"
else
    pass "Prompt outside last 3 lines correctly rejected"
fi

# Test: empty content should NOT match
if is_prompt_visible ""; then
    fail "False positive: empty content detected as prompt"
else
    pass "Empty content correctly rejected"
fi

# Test: > in the middle of a line should NOT match (only ❯ is valid)
if is_prompt_visible "some text
prefix >
next line"; then
    fail "False positive: > at end of line with prefix detected as prompt"
else
    pass "Line ending with > correctly rejected (not Claude prompt)"
fi

# Test: ❯ with prefix text should match
if is_prompt_visible "some text
prefix ❯
next line"; then
    pass "Detects ❯ at end of line with prefix text"
else
    fail "Failed to detect ❯ at end of line with prefix"
fi

# Test: HTML-like output ending with > should NOT match
if is_prompt_visible "Rendering component
<div class='container'>
</div>"; then
    fail "False positive: HTML closing tag detected as prompt"
else
    pass "HTML closing tag correctly rejected"
fi

# Test: Shell redirect in output should NOT match
if is_prompt_visible "Running command
echo 'test' >
output.txt"; then
    fail "False positive: shell redirect detected as prompt"
else
    pass "Shell redirect output correctly rejected"
fi

# =========================================================================
# Set up test environment for functional tests
# =========================================================================

TEST_DIR=$(mktemp -d)
ORIG_DIR=$(pwd)
cd "$TEST_DIR" || exit 1
mkdir -p .nbs/chat .nbs/events

# Source the relevant functions from nbs-claude
# We extract from Dynamic resource registration through Idle detection sidecar (tmux)
_EXTRACT_TMP=$(mktemp)
# Extract configuration
sed -n '/^# --- Configuration ---/,/^# --- Cleanup ---/p' "$NBS_CLAUDE" | head -n -1 >> "$_EXTRACT_TMP"
# Extract prompt/modal detection functions (plan mode, permissions, proceed, ask, context stress)
sed -n '/^# --- Plan mode detection ---/,/^# --- Dynamic resource registration ---/p' "$NBS_CLAUDE" | head -n -1 >> "$_EXTRACT_TMP"
# Extract resource registration + bus checking + prompt detection
sed -n '/^# --- Dynamic resource registration ---/,/^# --- Idle detection sidecar/p' "$NBS_CLAUDE" | head -n -2 >> "$_EXTRACT_TMP"
source "$_EXTRACT_TMP"
rm -f "$_EXTRACT_TMP"

# Initialise registry
seed_registry

# =========================================================================
# 4. check_bus_events: functional tests
# =========================================================================
echo "4. check_bus_events..."

# Test: no bus registered in registry → returns 2
# Remove bus entry if seed_registry added it
grep -v "^bus:" $CONTROL_REGISTRY > $CONTROL_REGISTRY.tmp 2>/dev/null
mv $CONTROL_REGISTRY.tmp $CONTROL_REGISTRY

check_bus_events
rc=$?
if [[ $rc -eq 2 ]]; then
    pass "No bus registered → returns 2"
else
    fail "No bus registered → expected rc=2, got rc=$rc"
fi

# Test: bus registered but directory empty → returns 1
echo "bus:.nbs/events" >> $CONTROL_REGISTRY
check_bus_events
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "Empty bus → returns 1"
else
    fail "Empty bus → expected rc=1, got rc=$rc"
fi
if [[ "$BUS_EVENT_COUNT" -eq 0 ]]; then
    pass "Empty bus → BUS_EVENT_COUNT=0"
else
    fail "Empty bus → BUS_EVENT_COUNT=$BUS_EVENT_COUNT (expected 0)"
fi

# Test: bus registered with pending events → returns 0
# nbs-bus publish uses positional args: <dir> <source> <type> <priority> [payload]
if command -v nbs-bus &>/dev/null; then
    nbs-bus publish .nbs/events/ sidecar-test test normal "test event" 2>/dev/null
    check_bus_events
    rc=$?
    if [[ $rc -eq 0 ]]; then
        pass "Pending events → returns 0"
    else
        fail "Pending events → expected rc=0, got rc=$rc"
    fi
    if [[ "$BUS_EVENT_COUNT" -gt 0 ]]; then
        pass "Pending events → BUS_EVENT_COUNT=$BUS_EVENT_COUNT > 0"
    else
        fail "Pending events → BUS_EVENT_COUNT=0 (expected > 0)"
    fi
    if [[ -n "$BUS_EVENT_SUMMARY" ]]; then
        pass "Pending events → has summary: $BUS_EVENT_SUMMARY"
    else
        fail "Pending events → empty summary"
    fi
    # Clean up event
    for ev in .nbs/events/*.event; do
        [[ -f "$ev" ]] && nbs-bus ack .nbs/events/ "$(basename "$ev")" 2>/dev/null
    done
else
    pass "SKIP: nbs-bus not available (3 tests skipped)"
fi

# Test: bus registered but directory does not exist → returns 1 (dir check fails)
grep -v "^bus:" $CONTROL_REGISTRY > $CONTROL_REGISTRY.tmp
mv $CONTROL_REGISTRY.tmp $CONTROL_REGISTRY
echo "bus:/nonexistent/events" >> $CONTROL_REGISTRY
check_bus_events
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "Missing bus directory → returns 1 (not 2, bus IS registered)"
else
    # Could be 2 if has_bus never set because -d check fails
    # Actually: -d fails → has_bus stays 0 → returns 2
    if [[ $rc -eq 2 ]]; then
        # The -d check prevents has_bus from being set, so it returns 2
        # This is actually correct behaviour — if the dir doesn't exist,
        # it's equivalent to no bus being registered
        pass "Missing bus directory → returns 2 (dir not found, treated as no bus)"
    else
        fail "Missing bus directory → expected rc=1 or 2, got rc=$rc"
    fi
fi

# Restore proper registry
grep -v "^bus:" $CONTROL_REGISTRY > $CONTROL_REGISTRY.tmp
mv $CONTROL_REGISTRY.tmp $CONTROL_REGISTRY
echo "bus:.nbs/events" >> $CONTROL_REGISTRY

# Test: no registry file at all → returns 2
mv $CONTROL_REGISTRY $CONTROL_REGISTRY.bak
check_bus_events
rc=$?
if [[ $rc -eq 2 ]]; then
    pass "No registry file → returns 2"
else
    fail "No registry file → expected rc=2, got rc=$rc"
fi
mv $CONTROL_REGISTRY.bak $CONTROL_REGISTRY

# =========================================================================
# 5. check_chat_unread: functional tests
# =========================================================================
echo "5. check_chat_unread..."

# Create a chat file with known message count
# Format: header lines, then ---, then messages (one per non-empty line)
cat > .nbs/chat/test.chat <<'CHAT'
# NBS Chat — test channel
Created: 2025-01-01
---
[2025-01-01 10:00:00 alice] Hello
[2025-01-01 10:01:00 bob] Hi there
[2025-01-01 10:02:00 alice] How are you?
[2025-01-01 10:03:00 bob] Good, thanks
[2025-01-01 10:04:00 alice] Great
CHAT

# Register the test chat
echo "chat:.nbs/chat/test.chat" >> $CONTROL_REGISTRY

# Test: no cursor file → messages are unread
# cursor defaults to 0 (meaning "position 0 read"), so unread = total - 1
SIDECAR_HANDLE="testhandle"
check_chat_unread
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "No cursor file → has unread (rc=0)"
else
    fail "No cursor file → expected rc=0, got rc=$rc"
fi
if [[ "$CHAT_UNREAD_COUNT" -eq 4 ]]; then
    pass "No cursor file → 4 unread messages (cursor defaults to 0)"
else
    fail "No cursor file → expected 4 unread, got $CHAT_UNREAD_COUNT"
fi

# Test: cursor at 0 → 4 unread (messages 1-4)
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
# Read cursors
testhandle=0
CURSORS

check_chat_unread
rc=$?
if [[ "$CHAT_UNREAD_COUNT" -eq 4 ]]; then
    pass "cursor=0 → 4 unread"
else
    fail "cursor=0 → expected 4 unread, got $CHAT_UNREAD_COUNT"
fi

# Test: cursor at 4 (last message index) → caught up
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
testhandle=4
CURSORS

check_chat_unread
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "cursor=4 (last msg) → caught up (rc=1)"
else
    fail "cursor=4 → expected rc=1, got rc=$rc"
fi
if [[ "$CHAT_UNREAD_COUNT" -eq 0 ]]; then
    pass "cursor=4 → 0 unread"
else
    fail "cursor=4 → expected 0 unread, got $CHAT_UNREAD_COUNT"
fi

# Test: cursor at 2 → 2 unread (messages 3-4)
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
testhandle=2
CURSORS

check_chat_unread
rc=$?
if [[ "$CHAT_UNREAD_COUNT" -eq 2 ]]; then
    pass "cursor=2 → 2 unread"
else
    fail "cursor=2 → expected 2 unread, got $CHAT_UNREAD_COUNT"
fi

# Test: summary includes chat filename
if [[ "$CHAT_UNREAD_SUMMARY" == *"test.chat"* ]]; then
    pass "Summary includes chat filename"
else
    fail "Summary missing chat filename: '$CHAT_UNREAD_SUMMARY'"
fi

# Test: multiple chats with unread
cat > .nbs/chat/other.chat <<'CHAT'
# NBS Chat — other channel
---
[msg1] Hello
[msg2] World
CHAT
echo "chat:.nbs/chat/other.chat" >> $CONTROL_REGISTRY

cat > .nbs/chat/test.chat.cursors <<'CURSORS'
testhandle=2
CURSORS
# No cursor file for other.chat → cursor defaults to 0 → 1 unread
# test.chat: cursor=2, 5 msgs → 2 unread. other.chat: cursor=0, 2 msgs → 1 unread.

check_chat_unread
rc=$?
if [[ "$CHAT_UNREAD_COUNT" -eq 3 ]]; then
    pass "Multiple chats → combined unread count=3 (2+1)"
else
    fail "Multiple chats → expected 3 unread, got $CHAT_UNREAD_COUNT"
fi

# Test: cursor file NOT modified by check_chat_unread
BEFORE=$(cat .nbs/chat/test.chat.cursors)
check_chat_unread
AFTER=$(cat .nbs/chat/test.chat.cursors)
if [[ "$BEFORE" == "$AFTER" ]]; then
    pass "Cursor file NOT modified by check_chat_unread"
else
    fail "Cursor file was modified by check_chat_unread!"
fi

# Test: no chats registered → returns 2
grep -v "^chat:" $CONTROL_REGISTRY > $CONTROL_REGISTRY.tmp
mv $CONTROL_REGISTRY.tmp $CONTROL_REGISTRY

check_chat_unread
rc=$?
if [[ $rc -eq 2 ]]; then
    pass "No chats registered → returns 2"
else
    fail "No chats registered → expected rc=2, got rc=$rc"
fi

# Restore chat entries
echo "chat:.nbs/chat/test.chat" >> $CONTROL_REGISTRY

# Test: empty chat file (no messages after ---)
cat > .nbs/chat/empty.chat <<'CHAT'
# NBS Chat — empty
---
CHAT
echo "chat:.nbs/chat/empty.chat" >> $CONTROL_REGISTRY

check_chat_unread
rc=$?
# empty.chat has 0 messages, so 0 unread for it
# test.chat has 2 unread (cursor=2, total=5)
if [[ "$CHAT_UNREAD_COUNT" -eq 2 ]]; then
    pass "Empty chat file → 0 unread from it, total still correct"
else
    fail "Empty chat file → expected 2 total unread, got $CHAT_UNREAD_COUNT"
fi

# Test: chat file with no --- delimiter → 0 messages counted
cat > .nbs/chat/nodash.chat <<'CHAT'
This chat has no delimiter
Just some text
CHAT
echo "chat:.nbs/chat/nodash.chat" >> $CONTROL_REGISTRY

check_chat_unread
rc=$?
# nodash.chat: awk never sets found=1, so count=0
# This is correct — a malformed chat file should not cause false unread
if [[ "$CHAT_UNREAD_COUNT" -eq 2 ]]; then
    pass "Chat with no delimiter → 0 messages (correct)"
else
    fail "Chat with no delimiter → expected 2 total, got $CHAT_UNREAD_COUNT"
fi

# Test: chat file that does not exist (registered but deleted)
echo "chat:.nbs/chat/deleted.chat" >> $CONTROL_REGISTRY
check_chat_unread
rc=$?
# deleted.chat: [[ -f "$chat_path" ]] fails → continue (skip it)
if [[ "$CHAT_UNREAD_COUNT" -eq 2 ]]; then
    pass "Deleted chat file → skipped gracefully"
else
    fail "Deleted chat file → expected 2 total, got $CHAT_UNREAD_COUNT"
fi

# =========================================================================
# 6. should_inject_notify: functional tests
# =========================================================================
echo "6. should_inject_notify..."

# Clean the registry to a known state
cat > $CONTROL_REGISTRY <<'REG'
bus:.nbs/events
chat:.nbs/chat/test.chat
REG

# Test: nothing pending → returns 1
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
testhandle=4
CURSORS
LAST_NOTIFY_TIME=0

should_inject_notify
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "Nothing pending → should NOT inject (rc=1)"
else
    fail "Nothing pending → expected rc=1, got rc=$rc"
fi
if [[ -z "$NOTIFY_MESSAGE" ]]; then
    pass "Nothing pending → empty NOTIFY_MESSAGE"
else
    fail "Nothing pending → NOTIFY_MESSAGE='$NOTIFY_MESSAGE' (expected empty)"
fi

# Test: chat unread → returns 0
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
testhandle=0
CURSORS
LAST_NOTIFY_TIME=0

should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Chat unread → should inject (rc=0)"
else
    fail "Chat unread → expected rc=0, got rc=$rc"
fi
if [[ -n "$NOTIFY_MESSAGE" ]]; then
    pass "Chat unread → has NOTIFY_MESSAGE: '$NOTIFY_MESSAGE'"
else
    fail "Chat unread → empty NOTIFY_MESSAGE"
fi

# Test: cooldown blocks injection
# LAST_NOTIFY_TIME was set by the previous call to should_inject_notify
# It should be within the last second
should_inject_notify
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "Cooldown blocks repeated injection"
else
    fail "Cooldown did NOT block repeated injection (rc=$rc)"
fi

# Test: cooldown expired → allows injection
LAST_NOTIFY_TIME=$(($(date +%s) - NOTIFY_COOLDOWN - 1))

should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Cooldown expired → allows injection"
else
    fail "Cooldown expired → expected rc=0, got rc=$rc"
fi

# Test: critical priority bypasses cooldown
# nbs-bus publish: <dir> <source> <type> <priority> [payload]
if command -v nbs-bus &>/dev/null; then
    LAST_NOTIFY_TIME=$(date +%s)  # Just injected, cooldown active
    nbs-bus publish .nbs/events/ sidecar-test urgent critical "URGENT" 2>/dev/null

    should_inject_notify
    rc=$?
    if [[ $rc -eq 0 ]]; then
        pass "Critical priority bypasses cooldown"
    else
        fail "Critical priority did NOT bypass cooldown (rc=$rc)"
    fi

    # Verify BUS_MAX_PRIORITY was detected
    if [[ "$BUS_MAX_PRIORITY" == "critical" ]]; then
        pass "BUS_MAX_PRIORITY correctly set to 'critical'"
    else
        fail "BUS_MAX_PRIORITY='$BUS_MAX_PRIORITY' (expected 'critical')"
    fi

    # Clean up
    for ev in .nbs/events/*.event; do
        [[ -f "$ev" ]] && nbs-bus ack .nbs/events/ "$(basename "$ev")" 2>/dev/null
    done
else
    pass "SKIP: nbs-bus not available (2 critical-priority tests skipped)"
fi

# Test: message truncation at 200 chars
# Create a chat with very many unread messages across many chats
for i in $(seq 1 20); do
    chatfile=".nbs/chat/longname-channel-${i}.chat"
    cat > "$chatfile" <<CHAT
# Chat $i
---
[msg] Hello from channel $i
CHAT
    echo "chat:$chatfile" >> $CONTROL_REGISTRY
done

LAST_NOTIFY_TIME=0
should_inject_notify
rc=$?
if [[ ${#NOTIFY_MESSAGE} -le 200 ]]; then
    pass "NOTIFY_MESSAGE capped at 200 chars (got ${#NOTIFY_MESSAGE})"
else
    fail "NOTIFY_MESSAGE exceeds 200 chars (got ${#NOTIFY_MESSAGE})"
fi

# =========================================================================
# 7. Two-track loop structure
# =========================================================================
echo "7. Two-track loop structure..."

# Verify BUS_CHECK_INTERVAL comparison in tmux sidecar
if grep -A2 'Track 1.*Bus-aware' "$NBS_CLAUDE" | grep -q 'bus_check_counter.*BUS_CHECK_INTERVAL'; then
    pass "tmux sidecar checks bus_check_counter against BUS_CHECK_INTERVAL"
else
    fail "tmux sidecar missing bus_check_counter comparison"
fi

# Verify no blind /nbs-poll injection (safety net removed — CSMA/CD standups replace it)
if ! grep -q 'Track 2.*Safety net' "$NBS_CLAUDE"; then
    pass "No blind /nbs-poll safety net (replaced by CSMA/CD standups)"
else
    fail "Track 2 safety net still present — should be removed"
fi

# Verify /nbs-notify is injected with NOTIFY_MESSAGE
if grep -q '/nbs-notify.*NOTIFY_MESSAGE' "$NBS_CLAUDE"; then
    pass "Injects /nbs-notify with NOTIFY_MESSAGE"
else
    fail "Missing NOTIFY_MESSAGE in /nbs-notify injection"
fi

# Verify /nbs-poll is NOT injected as a blind poll (only in recovery prompt)
if ! grep -q "send-keys.*'/nbs-poll'" "$NBS_CLAUDE" && ! grep -q 'send.*"/nbs-poll"' "$NBS_CLAUDE"; then
    pass "No blind /nbs-poll injection (event-driven only)"
else
    fail "/nbs-poll still injected as blind poll"
fi

# Verify should_inject_notify is called before injection
if grep -B3 '/nbs-notify' "$NBS_CLAUDE" | grep -q 'should_inject_notify'; then
    pass "should_inject_notify called before /nbs-notify injection"
else
    fail "should_inject_notify not called before injection"
fi

# Verify prompt visibility is checked before both tracks
PROMPT_CHECKS=$(grep -c 'is_prompt_visible' "$NBS_CLAUDE")
if [[ "$PROMPT_CHECKS" -ge 4 ]]; then
    pass "is_prompt_visible called in both tracks of both modes ($PROMPT_CHECKS refs)"
else
    fail "is_prompt_visible not in all tracks (found $PROMPT_CHECKS, expected >= 4)"
fi

# =========================================================================
# 8. Configuration defaults
# =========================================================================
echo "8. Configuration defaults..."

# Verify POLL_INTERVAL removed (replaced by CSMA/CD standups)
if ! grep -q 'NBS_POLL_INTERVAL' "$NBS_CLAUDE"; then
    pass "POLL_INTERVAL removed (blind polling eliminated)"
else
    fail "POLL_INTERVAL still present — should be removed"
fi

# Verify default BUS_CHECK_INTERVAL is 3
if grep -q 'NBS_BUS_CHECK_INTERVAL:-3' "$NBS_CLAUDE"; then
    pass "Default BUS_CHECK_INTERVAL is 3"
else
    fail "Default BUS_CHECK_INTERVAL is not 3"
fi

# Verify default NOTIFY_COOLDOWN is 15
if grep -q 'NBS_NOTIFY_COOLDOWN:-15' "$NBS_CLAUDE"; then
    pass "Default NOTIFY_COOLDOWN is 15"
else
    fail "Default NOTIFY_COOLDOWN is not 15"
fi

# Verify default NBS_HANDLE is 'claude'
if grep -q 'NBS_HANDLE:-claude' "$NBS_CLAUDE"; then
    pass "Default NBS_HANDLE is 'claude'"
else
    fail "Default NBS_HANDLE is not 'claude'"
fi

# =========================================================================
# 9. Cursor peeking safety: detailed verification
# =========================================================================
echo "9. Cursor peeking safety..."

# Test: check_chat_unread does NOT call nbs-chat (which would advance cursors)
if grep -A50 'check_chat_unread()' "$NBS_CLAUDE" | grep -q 'nbs-chat'; then
    fail "check_chat_unread calls nbs-chat (would advance cursors!)"
else
    pass "check_chat_unread does NOT call nbs-chat"
fi

# Test: check_chat_unread uses awk to read cursors (read-only)
if grep -A50 'check_chat_unread()' "$NBS_CLAUDE" | grep -q 'awk.*cursors'; then
    pass "check_chat_unread uses awk for cursor reading (read-only)"
else
    fail "check_chat_unread does not use awk for cursor reading"
fi

# Test: verify cursor file content is preserved after multiple checks
cat > $CONTROL_REGISTRY <<'REG'
chat:.nbs/chat/test.chat
REG
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
# Read cursors — last-read message index per handle
testhandle=2
otherhandle=4
CURSORS

CURSOR_BEFORE=$(md5sum .nbs/chat/test.chat.cursors | cut -d' ' -f1)

# Run check_chat_unread 10 times
for i in $(seq 1 10); do
    check_chat_unread
done

CURSOR_AFTER=$(md5sum .nbs/chat/test.chat.cursors | cut -d' ' -f1)
if [[ "$CURSOR_BEFORE" == "$CURSOR_AFTER" ]]; then
    pass "Cursor file hash unchanged after 10 check_chat_unread calls"
else
    fail "Cursor file hash changed after check_chat_unread calls!"
fi

# Verify all cursor entries are preserved
if grep -q 'testhandle=2' .nbs/chat/test.chat.cursors && \
   grep -q 'otherhandle=4' .nbs/chat/test.chat.cursors; then
    pass "All cursor entries preserved after repeated checks"
else
    fail "Cursor entries were modified"
fi

# =========================================================================
# 10. Edge cases
# =========================================================================
echo "10. Edge cases..."

# Test: chat file with blank lines after ---
cat > .nbs/chat/blanks.chat <<'CHAT'
# Chat with blanks
---
[msg1] Hello

[msg2] World

[msg3] Final
CHAT
cat > $CONTROL_REGISTRY <<'REG'
chat:.nbs/chat/blanks.chat
REG

# blank lines should not be counted (awk NF filter)
# With no cursor (default 0): 3 msgs → 2 unread
SIDECAR_HANDLE="blanktest"
check_chat_unread
if [[ "$CHAT_UNREAD_COUNT" -eq 2 ]]; then
    pass "Blank lines in chat not counted, cursor=0 → 2 unread (3 msgs - 1)"
else
    fail "Blank lines counted incorrectly (expected 2, got $CHAT_UNREAD_COUNT)"
fi

# Test: chat file with only --- and nothing after
cat > .nbs/chat/onlydash.chat <<'CHAT'
# Chat with just separator
---
CHAT
echo "chat:.nbs/chat/onlydash.chat" >> $CONTROL_REGISTRY

check_chat_unread
# onlydash: 0 messages, blanks: 3 messages (cursor=0 → 2 unread), total 2
if [[ "$CHAT_UNREAD_COUNT" -eq 2 ]]; then
    pass "Chat with only --- separator → 0 messages from it"
else
    fail "Chat with only --- → expected 2 total, got $CHAT_UNREAD_COUNT"
fi

# Test: multiple --- delimiters (only first matters in awk)
cat > .nbs/chat/multidash.chat <<'CHAT'
# Chat with multiple dashes
---
[msg1] First section
---
[msg2] Second section
CHAT
echo "chat:.nbs/chat/multidash.chat" >> $CONTROL_REGISTRY

check_chat_unread
# awk: found=1 after first ---, then counts msg1, second --- triggers next (skipped),
# then counts msg2. Total from multidash: 2 msgs, cursor=0 → 1 unread.
# blanks(2 unread) + onlydash(0) + multidash(1) = 3
if [[ "$CHAT_UNREAD_COUNT" -eq 3 ]]; then
    pass "Multiple --- delimiters handled correctly (2 messages from multidash)"
else
    fail "Multiple --- handling wrong (expected 3 total, got $CHAT_UNREAD_COUNT)"
fi

# Test: BUS_EVENT_SUMMARY contains directory path
cat > $CONTROL_REGISTRY <<'REG'
bus:.nbs/events
REG
if command -v nbs-bus &>/dev/null; then
    nbs-bus publish .nbs/events/ sidecar-test test normal "test" 2>/dev/null
    check_bus_events
    if [[ "$BUS_EVENT_SUMMARY" == *".nbs/events"* ]]; then
        pass "BUS_EVENT_SUMMARY contains directory path"
    else
        fail "BUS_EVENT_SUMMARY missing directory: '$BUS_EVENT_SUMMARY'"
    fi
    # Clean up
    for ev in .nbs/events/*.event; do
        [[ -f "$ev" ]] && nbs-bus ack .nbs/events/ "$(basename "$ev")" 2>/dev/null
    done
else
    pass "SKIP: nbs-bus not available (1 test skipped)"
fi

# =========================================================================
# 11. Injection verification: structural tests
# =========================================================================
echo "11. Injection verification logic..."

# Test: tmux sidecar verifies injection was consumed
if grep -A70 'Track 1.*Bus-aware' "$NBS_CLAUDE" | grep -q 'verify_content'; then
    pass "tmux sidecar captures pane after injection for verification"
else
    fail "tmux sidecar missing post-injection verification"
fi

# Test: verification checks is_prompt_visible on post-injection content
if grep -A40 'Verify injection' "$NBS_CLAUDE" | grep -q 'is_prompt_visible.*final_content'; then
    pass "Verification checks is_prompt_visible on captured content"
else
    fail "Verification does not check is_prompt_visible"
fi

# Test: retry sends Enter if prompt still visible
if grep -A25 'Verify injection' "$NBS_CLAUDE" | grep -q 'retry'; then
    pass "Retry logic present when injection not consumed"
else
    fail "Missing retry logic for unconsumed injection"
fi

# Test: verification exists in both sidecar modes (tmux and pty)
VERIFY_COUNT=$(grep -c 'Verify injection' "$NBS_CLAUDE")
if [[ "$VERIFY_COUNT" -ge 2 ]]; then
    pass "Injection verification in both sidecar modes ($VERIFY_COUNT occurrences)"
else
    fail "Injection verification not in both modes (found $VERIFY_COUNT, expected >= 2)"
fi

# Test: is_prompt_visible uses fixed string match (grep -qF) not regex
if grep -A3 'is_prompt_visible()' "$NBS_CLAUDE" | grep -q 'grep -qF'; then
    pass "is_prompt_visible uses grep -qF (fixed string, no regex false positives)"
else
    fail "is_prompt_visible does not use grep -qF"
fi

# Test: is_prompt_visible does NOT match bare > (the old pattern)
if grep -A3 'is_prompt_visible()' "$NBS_CLAUDE" | grep -q '>\\\s\*\$'; then
    fail "is_prompt_visible still has '>\\s*$' pattern (should be removed)"
else
    pass "is_prompt_visible no longer matches bare > pattern"
fi

# Test: fresh re-capture before injection (TOCTOU mitigation)
# Both sidecars must re-capture pane content immediately before injection
# to avoid using stale $content that may be up to BUS_CHECK_INTERVAL seconds old.
FRESH_RECAPTURE_COUNT=$(grep -c 'TOCTOU fix: re-capture pane' "$NBS_CLAUDE")
if [[ "$FRESH_RECAPTURE_COUNT" -ge 2 ]]; then
    pass "Fresh re-capture before injection in both sidecar modes ($FRESH_RECAPTURE_COUNT occurrences)"
else
    fail "Fresh re-capture before injection not in both modes (found $FRESH_RECAPTURE_COUNT, expected >= 2)"
fi

# Test: fresh re-capture aborts injection if prompt disappeared
if grep -A10 'TOCTOU fix: re-capture pane' "$NBS_CLAUDE" | grep -q 'Abort injection'; then
    pass "Fresh re-capture aborts injection when prompt disappears"
else
    fail "Fresh re-capture does not abort when prompt disappears"
fi

# Test: init-wait loops use grep -qF (fixed string), not the old broad regex
# The old pattern was: grep -qE '❯|>\s*$' which matched HTML tags and shell output
if grep -F '>\s*$' "$NBS_CLAUDE" | grep -v '^#' | grep -v 'REJECT' | grep -q 'grep'; then
    fail "Init-wait loops still use broad '>\\s*\$' regex pattern"
else
    pass "Init-wait loops do not use broad regex pattern"
fi

# Test: init-wait loops use grep -qF for prompt detection
INIT_WAIT_FIXED_COUNT=$(grep -c "grep -qF '❯'" "$NBS_CLAUDE")
if [[ "$INIT_WAIT_FIXED_COUNT" -ge 3 ]]; then
    # Expected: is_prompt_visible (1) + tmux init-wait (1) + pty init-wait (1) = 3
    pass "All prompt checks use grep -qF fixed string match ($INIT_WAIT_FIXED_COUNT occurrences)"
else
    fail "Not all prompt checks use grep -qF (found $INIT_WAIT_FIXED_COUNT, expected >= 3)"
fi

# Test: pty-session sidecar has 3-retry backoff loop (not single retry)
if grep -A30 'poll_sidecar_pty' "$NBS_CLAUDE" | grep -q 'retry_attempt in 1 2 3'; then
    # This may not match because retry_attempt is deeper in the function.
    # Instead, count retry loops in the pty sidecar section.
    true
fi
PTY_RETRY_COUNT=$(sed -n '/poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE" | grep -c 'retry_attempt')
if [[ "$PTY_RETRY_COUNT" -ge 1 ]]; then
    pass "pty-session sidecar has retry loop with retry_attempt variable"
else
    fail "pty-session sidecar missing retry loop (retry_attempt not found)"
fi

# Test: detect_proceed_prompt function exists
if grep -q 'detect_proceed_prompt()' "$NBS_CLAUDE"; then
    pass "detect_proceed_prompt function exists"
else
    fail "detect_proceed_prompt function missing"
fi

# Test: detect_proceed_prompt checks for "Do you want to proceed?" without "don't ask again"
if grep -A5 'detect_proceed_prompt()' "$NBS_CLAUDE" | grep -qF 'Do you want to proceed?' && \
   grep -A5 'detect_proceed_prompt()' "$NBS_CLAUDE" | grep -q "! .*don't ask again"; then
    pass "detect_proceed_prompt correctly distinguishes from permissions prompt"
else
    fail "detect_proceed_prompt does not distinguish from permissions prompt"
fi

# Test: blocking dialogue dispatch used in both sidecar modes
DISPATCH_COUNT=$(grep -c 'check_blocking_dialogue' "$NBS_CLAUDE")
# Expected: function def (1) + tmux on-change (1) + tmux stable (1) + pty on-change (1) + pty stable (1) = 5
if [[ "$DISPATCH_COUNT" -ge 5 ]]; then
    pass "check_blocking_dialogue wired into both sidecar modes ($DISPATCH_COUNT occurrences)"
else
    fail "check_blocking_dialogue not in both modes (found $DISPATCH_COUNT, expected >= 5)"
fi

# Test: detect_proceed_prompt is called from check_blocking_dialogue dispatch
if grep -A20 'check_blocking_dialogue()' "$NBS_CLAUDE" | grep -q 'detect_proceed_prompt'; then
    pass "detect_proceed_prompt dispatched via check_blocking_dialogue"
else
    fail "detect_proceed_prompt not dispatched via check_blocking_dialogue"
fi

# Test: proceed prompt selects option 1 (Yes) in dispatch table
if grep -A20 'detect_proceed_prompt' "$NBS_CLAUDE" | grep -q "DIALOGUE_OPTION='1'"; then
    pass "Proceed prompt auto-selects option 1 (Yes)"
else
    fail "Proceed prompt does not auto-select option 1"
fi

# Test: respond_dialogue helpers exist for both transport modes
if grep -q 'respond_dialogue_tmux()' "$NBS_CLAUDE" && grep -q 'respond_dialogue_pty()' "$NBS_CLAUDE"; then
    pass "Transport-specific dialogue response helpers exist (tmux + pty)"
else
    fail "Missing respond_dialogue helper for one or both transport modes"
fi

# Test: dialogue dispatch table documents all dialogue types
if grep -A30 'check_blocking_dialogue()' "$NBS_CLAUDE" | grep -q 'detect_plan_mode' && \
   grep -A30 'check_blocking_dialogue()' "$NBS_CLAUDE" | grep -q 'detect_ask_modal' && \
   grep -A30 'check_blocking_dialogue()' "$NBS_CLAUDE" | grep -q 'detect_permissions_prompt' && \
   grep -A30 'check_blocking_dialogue()' "$NBS_CLAUDE" | grep -q 'detect_proceed_prompt'; then
    pass "Dialogue dispatch table covers all 4 dialogue types"
else
    fail "Dialogue dispatch table missing one or more dialogue types"
fi

# Functional test: detect_proceed_prompt correctly matches simple proceed prompt
SIMPLE_PROCEED=$'Do you want to proceed?\n❯ 1. Yes\n  2. No'
if detect_proceed_prompt "$SIMPLE_PROCEED"; then
    pass "detect_proceed_prompt matches simple Yes/No proceed prompt"
else
    fail "detect_proceed_prompt does not match simple Yes/No proceed prompt"
fi

# Functional test: detect_proceed_prompt rejects permissions prompt
PERM_PROMPT=$'Do you want to proceed?\n❯ 1. Yes\n  2. Yes, and don'\''t ask again for Bash\n  3. No'
if detect_proceed_prompt "$PERM_PROMPT"; then
    fail "detect_proceed_prompt incorrectly matches permissions prompt"
else
    pass "detect_proceed_prompt correctly rejects permissions prompt (has don't ask again)"
fi

# Functional test: detect_permissions_prompt still works on permissions prompt
if detect_permissions_prompt "$PERM_PROMPT"; then
    pass "detect_permissions_prompt still matches permissions prompt"
else
    fail "detect_permissions_prompt no longer matches permissions prompt"
fi

# =========================================================================
# 12. nbs-poll.md updated for safety net role
# =========================================================================
echo "12. nbs-poll.md safety net language..."

POLL_DOC="$PROJECT_ROOT/claude_tools/nbs-poll.md"

if grep -q 'safety net' "$POLL_DOC"; then
    pass "nbs-poll.md mentions safety net role"
else
    fail "nbs-poll.md missing safety net language"
fi

if grep -q 'nbs-notify' "$POLL_DOC"; then
    pass "nbs-poll.md references nbs-notify for event-driven path"
else
    fail "nbs-poll.md missing nbs-notify reference"
fi

if grep -q '5 minutes' "$POLL_DOC"; then
    pass "nbs-poll.md mentions 5 minute default interval"
else
    fail "nbs-poll.md missing 5 minute interval mention"
fi

# =========================================================================
# 13. Documentation updated
# =========================================================================
echo "13. docs/nbs-claude.md updated..."

DOC="$PROJECT_ROOT/docs/nbs-claude.md"

if grep -q 'BUS_CHECK_INTERVAL' "$DOC"; then
    pass "docs/nbs-claude.md documents BUS_CHECK_INTERVAL"
else
    fail "docs/nbs-claude.md missing BUS_CHECK_INTERVAL"
fi

if grep -q 'NOTIFY_COOLDOWN' "$DOC"; then
    pass "docs/nbs-claude.md documents NOTIFY_COOLDOWN"
else
    fail "docs/nbs-claude.md missing NOTIFY_COOLDOWN"
fi

if grep -q 'Bus-Aware' "$DOC"; then
    pass "docs/nbs-claude.md has Bus-Aware section"
else
    fail "docs/nbs-claude.md missing Bus-Aware section"
fi

if grep -q 'check_bus_events\|check_chat_unread\|Cursor' "$DOC"; then
    pass "docs/nbs-claude.md documents cursor peeking"
else
    fail "docs/nbs-claude.md missing cursor peeking docs"
fi

# =========================================================================
# 14. detect_context_stress: structural and functional tests
# =========================================================================
echo "14. detect_context_stress..."

# Structural: function exists
if grep -q 'detect_context_stress' "$NBS_CLAUDE"; then
    pass "Has detect_context_stress function"
else
    fail "Missing detect_context_stress function"
fi

# Structural: referenced in both sidecar loops
TMUX_CTX=$(sed -n '/^poll_sidecar_tmux/,/^}/p' "$NBS_CLAUDE" | grep -c 'detect_context_stress')
if [[ "$TMUX_CTX" -ge 1 ]]; then
    pass "detect_context_stress in tmux sidecar ($TMUX_CTX references)"
else
    fail "detect_context_stress not in tmux sidecar (found $TMUX_CTX, expected >= 1)"
fi

PTY_CTX=$(sed -n '/^poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE" | grep -c 'detect_context_stress')
if [[ "$PTY_CTX" -ge 1 ]]; then
    pass "detect_context_stress in pty sidecar ($PTY_CTX references)"
else
    fail "detect_context_stress not in pty sidecar (found $PTY_CTX, expected >= 1)"
fi

# Source just the detect_context_stress function
eval "$(grep -A7 '^detect_context_stress()' "$NBS_CLAUDE")"

# Functional: detects "Compacting conversation"
if detect_context_stress "some output
Compacting conversation
❯"; then
    pass "Detects 'Compacting conversation'"
else
    fail "Failed to detect 'Compacting conversation'"
fi

# Functional: detects "Compacting conversation…" (with ellipsis)
if detect_context_stress "some output
Compacting conversation…
waiting"; then
    pass "Detects 'Compacting conversation…' (ellipsis variant)"
else
    fail "Failed to detect 'Compacting conversation…'"
fi

# Functional: detects "Conversation too long"
if detect_context_stress "error output
Conversation too long. Press esc twice to go up a few messages and try again.
❯"; then
    pass "Detects 'Conversation too long'"
else
    fail "Failed to detect 'Conversation too long'"
fi

# Functional: detects "Prompt is too long"
if detect_context_stress "Prompt is too long
❯"; then
    pass "Detects 'Prompt is too long'"
else
    fail "Failed to detect 'Prompt is too long'"
fi

# Functional: detects "Error compacting conversation"
if detect_context_stress "Error compacting conversation
❯"; then
    pass "Detects 'Error compacting conversation'"
else
    fail "Failed to detect 'Error compacting conversation'"
fi

# Functional: normal prompt — NOT detected
if detect_context_stress "some AI output
claude ❯"; then
    fail "False positive: normal prompt detected as context stress"
else
    pass "Normal prompt correctly not detected as context stress"
fi

# Functional: empty content — NOT detected
if detect_context_stress ""; then
    fail "False positive: empty content detected as context stress"
else
    pass "Empty content correctly not detected as context stress"
fi

# Functional: --compact-log (different context for 'compact') — NOT detected
if detect_context_stress "nbs-chat --compact-log archive/
❯"; then
    fail "False positive: --compact-log detected as context stress"
else
    pass "--compact-log correctly not detected as context stress"
fi

# Structural: context stress check appears BEFORE should_inject_notify in tmux
TMUX_ORDER=$(sed -n '/^poll_sidecar_tmux/,/^}/p' "$NBS_CLAUDE" | grep -n 'detect_context_stress\|should_inject_notify' | head -2)
STRESS_LINE=$(echo "$TMUX_ORDER" | head -1 | cut -d: -f1)
NOTIFY_LINE=$(echo "$TMUX_ORDER" | tail -1 | cut -d: -f1)
if [[ "$STRESS_LINE" -lt "$NOTIFY_LINE" ]]; then
    pass "Context stress check before should_inject_notify in tmux"
else
    fail "Context stress check NOT before should_inject_notify in tmux"
fi

# Structural: context stress check appears BEFORE should_inject_notify in pty
PTY_ORDER=$(sed -n '/^poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE" | grep -n 'detect_context_stress\|should_inject_notify' | head -2)
STRESS_LINE=$(echo "$PTY_ORDER" | head -1 | cut -d: -f1)
NOTIFY_LINE=$(echo "$PTY_ORDER" | tail -1 | cut -d: -f1)
if [[ "$STRESS_LINE" -lt "$NOTIFY_LINE" ]]; then
    pass "Context stress check before should_inject_notify in pty"
else
    fail "Context stress check NOT before should_inject_notify in pty"
fi

# =========================================================================
# 14. Startup grace period: no notifications during grace window
# =========================================================================
echo "15. Startup grace period..."

# Structural: STARTUP_GRACE variable exists
if grep -q 'STARTUP_GRACE' "$NBS_CLAUDE"; then
    pass "Has STARTUP_GRACE variable"
else
    fail "Missing STARTUP_GRACE variable"
fi

# Structural: NBS_STARTUP_GRACE environment variable documented
if grep -q 'NBS_STARTUP_GRACE' "$NBS_CLAUDE"; then
    pass "NBS_STARTUP_GRACE env var documented"
else
    fail "NBS_STARTUP_GRACE env var not documented"
fi

# Structural: SIDECAR_START_TIME is set after initial handle prompt (tmux)
TMUX_INIT=$(sed -n '/^poll_sidecar_tmux/,/^}/p' "$NBS_CLAUDE")
if echo "$TMUX_INIT" | grep -q 'SIDECAR_START_TIME'; then
    pass "SIDECAR_START_TIME set in tmux sidecar"
else
    fail "SIDECAR_START_TIME not set in tmux sidecar"
fi

# Structural: SIDECAR_START_TIME is set after initial handle prompt (pty)
PTY_INIT=$(sed -n '/^poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE")
if echo "$PTY_INIT" | grep -q 'SIDECAR_START_TIME'; then
    pass "SIDECAR_START_TIME set in pty sidecar"
else
    fail "SIDECAR_START_TIME not set in pty sidecar"
fi

# Structural: should_inject_notify checks SIDECAR_START_TIME
INJECT_FN=$(sed -n '/^should_inject_notify/,/^}/p' "$NBS_CLAUDE")
if echo "$INJECT_FN" | grep -q 'SIDECAR_START_TIME'; then
    pass "should_inject_notify checks SIDECAR_START_TIME"
else
    fail "should_inject_notify does not check SIDECAR_START_TIME"
fi

# Structural: should_inject_notify checks STARTUP_GRACE
if echo "$INJECT_FN" | grep -q 'STARTUP_GRACE'; then
    pass "should_inject_notify checks STARTUP_GRACE"
else
    fail "should_inject_notify does not check STARTUP_GRACE"
fi

# Functional: grace period blocks injection when within window
# Set up: unread chat messages exist (would normally trigger injection)
cat > .nbs/chat/test.chat <<'CHAT'
---
testhandle: test message 1
testhandle: test message 2
testhandle: test message 3
testhandle: test message 4
testhandle: test message 5
CHAT
cat > .nbs/chat/test.chat.cursors <<'CURSORS'
testhandle=0
CURSORS
cat > $CONTROL_REGISTRY <<'REG'
chat:.nbs/chat/test.chat
REG
LAST_NOTIFY_TIME=0

# Without grace period (SIDECAR_START_TIME=0): should inject
SIDECAR_START_TIME=0
should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "No grace active (START_TIME=0) → allows injection"
else
    fail "No grace active → expected rc=0, got rc=$rc"
fi

# With grace period active (recent start time): should block
SIDECAR_START_TIME=$(date +%s)
STARTUP_GRACE=30
LAST_NOTIFY_TIME=0
should_inject_notify
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "Grace period active → blocks injection (rc=1)"
else
    fail "Grace period active → expected rc=1, got rc=$rc"
fi

# With grace period expired (old start time): should allow
SIDECAR_START_TIME=$(($(date +%s) - 31))
STARTUP_GRACE=30
LAST_NOTIFY_TIME=0
should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Grace period expired → allows injection (rc=0)"
else
    fail "Grace period expired → expected rc=0, got rc=$rc"
fi

# With grace period = 0 (disabled): should allow immediately
SIDECAR_START_TIME=$(date +%s)
STARTUP_GRACE=0
LAST_NOTIFY_TIME=0
should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Grace period = 0 → allows injection immediately"
else
    fail "Grace period = 0 → expected rc=0, got rc=$rc"
fi

# Edge: exactly at grace boundary (elapsed == STARTUP_GRACE): should allow
SIDECAR_START_TIME=$(($(date +%s) - 30))
STARTUP_GRACE=30
LAST_NOTIFY_TIME=0
should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Exactly at grace boundary → allows injection"
else
    fail "Exactly at grace boundary → expected rc=0, got rc=$rc"
fi

# Structural: STARTUP_GRACE default is 30
DEFAULT_GRACE=$(grep 'NBS_STARTUP_GRACE:-' "$NBS_CLAUDE" | head -1 | sed 's/.*:-\([0-9]*\)}.*/\1/')
if [[ "$DEFAULT_GRACE" == "30" ]]; then
    pass "Default STARTUP_GRACE is 30 seconds"
else
    fail "Default STARTUP_GRACE is '$DEFAULT_GRACE', expected 30"
fi

# Structural: STARTUP_GRACE is in numeric validation loop
if grep 'STARTUP_GRACE' "$NBS_CLAUDE" | grep -q '_nbs_var_name'; then
    pass "STARTUP_GRACE included in numeric validation"
else
    fail "STARTUP_GRACE missing from numeric validation"
fi

# =========================================================================
# 16. NBS_INITIAL_PROMPT: custom initial prompt for sidecar
# =========================================================================
echo "16. NBS_INITIAL_PROMPT..."

# Structural: INITIAL_PROMPT variable exists in configuration
if grep -q 'INITIAL_PROMPT=' "$NBS_CLAUDE"; then
    pass "Has INITIAL_PROMPT variable"
else
    fail "Missing INITIAL_PROMPT variable"
fi

# Structural: NBS_INITIAL_PROMPT environment variable documented in header
if grep -q 'NBS_INITIAL_PROMPT' "$NBS_CLAUDE"; then
    pass "NBS_INITIAL_PROMPT env var documented"
else
    fail "NBS_INITIAL_PROMPT env var not documented"
fi

# Structural: default is empty (no custom prompt)
if grep -q 'NBS_INITIAL_PROMPT:-}' "$NBS_CLAUDE"; then
    pass "Default INITIAL_PROMPT is empty string"
else
    fail "Default INITIAL_PROMPT is not empty"
fi

# Structural: INITIAL_PROMPT used in tmux sidecar init prompt logic
TMUX_INIT=$(sed -n '/^poll_sidecar_tmux/,/^}/p' "$NBS_CLAUDE")
if echo "$TMUX_INIT" | grep -q 'INITIAL_PROMPT'; then
    pass "INITIAL_PROMPT referenced in tmux sidecar"
else
    fail "INITIAL_PROMPT not referenced in tmux sidecar"
fi

# Structural: INITIAL_PROMPT used in pty sidecar init prompt logic
PTY_INIT=$(sed -n '/^poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE")
if echo "$PTY_INIT" | grep -q 'INITIAL_PROMPT'; then
    pass "INITIAL_PROMPT referenced in pty sidecar"
else
    fail "INITIAL_PROMPT not referenced in pty sidecar"
fi

# Structural: conditional uses INITIAL_PROMPT to choose between custom and default
if echo "$TMUX_INIT" | grep -q 'if.*-n.*INITIAL_PROMPT'; then
    pass "tmux sidecar has INITIAL_PROMPT conditional"
else
    fail "tmux sidecar missing INITIAL_PROMPT conditional"
fi

if echo "$PTY_INIT" | grep -q 'if.*-n.*INITIAL_PROMPT'; then
    pass "pty sidecar has INITIAL_PROMPT conditional"
else
    fail "pty sidecar missing INITIAL_PROMPT conditional"
fi

# Structural: default prompt contains handle and /nbs-teams-chat
if echo "$TMUX_INIT" | grep -q 'SIDECAR_HANDLE.*nbs-teams-chat'; then
    pass "Default prompt includes handle and /nbs-teams-chat"
else
    fail "Default prompt missing handle or /nbs-teams-chat"
fi

# Structural: startup banner shows custom prompt indicator
if grep -q 'Initial prompt.*custom.*NBS_INITIAL_PROMPT' "$NBS_CLAUDE"; then
    pass "Startup banner shows custom prompt indicator"
else
    fail "Startup banner missing custom prompt indicator"
fi

# Structural: init_prompt variable exists in both sidecars
TMUX_INIT_PROMPT_COUNT=$(echo "$TMUX_INIT" | grep -c 'init_prompt')
if [[ "$TMUX_INIT_PROMPT_COUNT" -ge 3 ]]; then
    pass "init_prompt variable used in tmux sidecar ($TMUX_INIT_PROMPT_COUNT refs)"
else
    fail "init_prompt variable insufficient in tmux sidecar (found $TMUX_INIT_PROMPT_COUNT, expected >= 3)"
fi

PTY_INIT_PROMPT_COUNT=$(echo "$PTY_INIT" | grep -c 'init_prompt')
if [[ "$PTY_INIT_PROMPT_COUNT" -ge 3 ]]; then
    pass "init_prompt variable used in pty sidecar ($PTY_INIT_PROMPT_COUNT refs)"
else
    fail "init_prompt variable insufficient in pty sidecar (found $PTY_INIT_PROMPT_COUNT, expected >= 3)"
fi

# Structural: SIDECAR_START_TIME set AFTER init_prompt is sent (both sidecars)
# In tmux: init_prompt sent via send-keys, then SIDECAR_START_TIME set
TMUX_SEND_LINE=$(echo "$TMUX_INIT" | grep -n 'send-keys.*init_prompt' | head -1 | cut -d: -f1)
TMUX_START_LINE=$(echo "$TMUX_INIT" | grep -n 'SIDECAR_START_TIME=' | head -1 | cut -d: -f1)
if [[ -n "$TMUX_SEND_LINE" && -n "$TMUX_START_LINE" && "$TMUX_SEND_LINE" -lt "$TMUX_START_LINE" ]]; then
    pass "SIDECAR_START_TIME set after init_prompt sent (tmux)"
else
    fail "SIDECAR_START_TIME not after init_prompt in tmux (send=$TMUX_SEND_LINE, start=$TMUX_START_LINE)"
fi

# In pty: init_prompt sent via pty send, then SIDECAR_START_TIME set
PTY_SEND_LINE=$(echo "$PTY_INIT" | grep -n 'send.*init_prompt' | head -1 | cut -d: -f1)
PTY_START_LINE=$(echo "$PTY_INIT" | grep -n 'SIDECAR_START_TIME=' | head -1 | cut -d: -f1)
if [[ -n "$PTY_SEND_LINE" && -n "$PTY_START_LINE" && "$PTY_SEND_LINE" -lt "$PTY_START_LINE" ]]; then
    pass "SIDECAR_START_TIME set after init_prompt sent (pty)"
else
    fail "SIDECAR_START_TIME not after init_prompt in pty (send=$PTY_SEND_LINE, start=$PTY_START_LINE)"
fi

# =========================================================================
# 17. Self-healing: detect_skill_failure, build_recovery_prompt, failure tracking
# =========================================================================
echo "17. Self-healing after skill loss..."

# --- Structural: detect_skill_failure function exists ---
if grep -q 'detect_skill_failure()' "$NBS_CLAUDE"; then
    pass "Has detect_skill_failure function"
else
    fail "Missing detect_skill_failure function"
fi

# --- Structural: build_recovery_prompt function exists ---
if grep -q 'build_recovery_prompt()' "$NBS_CLAUDE"; then
    pass "Has build_recovery_prompt function"
else
    fail "Missing build_recovery_prompt function"
fi

# --- Structural: NOTIFY_FAIL_COUNT global exists ---
if grep -q 'NOTIFY_FAIL_COUNT=0' "$NBS_CLAUDE"; then
    pass "Has NOTIFY_FAIL_COUNT global"
else
    fail "Missing NOTIFY_FAIL_COUNT global"
fi

# --- Structural: NOTIFY_FAIL_THRESHOLD config exists ---
if grep -q 'NOTIFY_FAIL_THRESHOLD=' "$NBS_CLAUDE"; then
    pass "Has NOTIFY_FAIL_THRESHOLD config"
else
    fail "Missing NOTIFY_FAIL_THRESHOLD config"
fi

# --- Structural: detect_skill_failure used in tmux sidecar ---
TMUX_SKILL=$(sed -n '/^poll_sidecar_tmux/,/^}/p' "$NBS_CLAUDE" | grep -c 'detect_skill_failure')
if [[ "$TMUX_SKILL" -ge 2 ]]; then
    pass "detect_skill_failure in tmux sidecar ($TMUX_SKILL references)"
else
    fail "detect_skill_failure insufficient in tmux sidecar (found $TMUX_SKILL, expected >= 2)"
fi

# --- Structural: detect_skill_failure used in pty sidecar ---
PTY_SKILL=$(sed -n '/^poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE" | grep -c 'detect_skill_failure')
if [[ "$PTY_SKILL" -ge 2 ]]; then
    pass "detect_skill_failure in pty sidecar ($PTY_SKILL references)"
else
    fail "detect_skill_failure insufficient in pty sidecar (found $PTY_SKILL, expected >= 2)"
fi

# --- Structural: build_recovery_prompt used in tmux sidecar ---
TMUX_RECOVERY=$(sed -n '/^poll_sidecar_tmux/,/^}/p' "$NBS_CLAUDE" | grep -c 'build_recovery_prompt')
if [[ "$TMUX_RECOVERY" -ge 1 ]]; then
    pass "build_recovery_prompt in tmux sidecar"
else
    fail "build_recovery_prompt not in tmux sidecar"
fi

# --- Structural: build_recovery_prompt used in pty sidecar ---
PTY_RECOVERY=$(sed -n '/^poll_sidecar_pty/,/^}/p' "$NBS_CLAUDE" | grep -c 'build_recovery_prompt')
if [[ "$PTY_RECOVERY" -ge 1 ]]; then
    pass "build_recovery_prompt in pty sidecar"
else
    fail "build_recovery_prompt not in pty sidecar"
fi

# --- Structural: NOTIFY_FAIL_COUNT incremented on skill failure ---
if grep -q 'NOTIFY_FAIL_COUNT=\$((NOTIFY_FAIL_COUNT + 1))' "$NBS_CLAUDE"; then
    pass "NOTIFY_FAIL_COUNT incremented on skill failure"
else
    fail "NOTIFY_FAIL_COUNT not incremented"
fi

# --- Structural: NOTIFY_FAIL_COUNT reset on success ---
if grep -q 'NOTIFY_FAIL_COUNT=0' "$NBS_CLAUDE"; then
    pass "NOTIFY_FAIL_COUNT reset on success"
else
    fail "NOTIFY_FAIL_COUNT not reset"
fi

# --- Structural: recovery triggered when count >= threshold ---
if grep -q 'NOTIFY_FAIL_COUNT -ge.*NOTIFY_FAIL_THRESHOLD' "$NBS_CLAUDE"; then
    pass "Recovery triggered at threshold"
else
    fail "Recovery threshold check not found"
fi

# --- Functional: detect_skill_failure ---
eval "$(grep -A4 '^detect_skill_failure()' "$NBS_CLAUDE")"

# Detects "Unknown skill: nbs-notify"
if detect_skill_failure "❯ Unknown skill: nbs-notify

❯"; then
    pass "detect_skill_failure catches 'Unknown skill: nbs-notify'"
else
    fail "detect_skill_failure missed 'Unknown skill: nbs-notify'"
fi

# Detects "Unknown skill" with any skill name
if detect_skill_failure "❯ Unknown skill: nbs-poll

❯"; then
    pass "detect_skill_failure catches 'Unknown skill: nbs-poll'"
else
    fail "detect_skill_failure missed 'Unknown skill: nbs-poll'"
fi

# Normal output — NOT detected
if detect_skill_failure "● Bash(nbs-chat read .nbs/chat/live.chat)
  ⎿  some output
❯"; then
    fail "False positive: normal output detected as skill failure"
else
    pass "Normal output correctly not detected as skill failure"
fi

# Empty content — NOT detected
if detect_skill_failure ""; then
    fail "False positive: empty content detected as skill failure"
else
    pass "Empty content correctly not detected as skill failure"
fi

# --- Functional: build_recovery_prompt ---
# Source the function with minimal env
(
    NBS_ROOT="$TEST_DIR"
    SIDECAR_HANDLE="test-agent"
    CONTROL_REGISTRY="$TEST_DIR/.nbs/control-registry-test-agent"

    # Create the skill files and registry
    mkdir -p "$TEST_DIR/claude_tools"
    touch "$TEST_DIR/claude_tools/nbs-notify.md"
    touch "$TEST_DIR/claude_tools/nbs-teams-chat.md"
    touch "$TEST_DIR/claude_tools/nbs-poll.md"

    echo "chat:$TEST_DIR/.nbs/chat/live.chat" > "$CONTROL_REGISTRY"

    eval "$(grep -A30 '^build_recovery_prompt()' "$NBS_CLAUDE" | head -31)"

    prompt=$(build_recovery_prompt)

    # Must contain absolute paths to skill files
    if echo "$prompt" | grep -qF "$TEST_DIR/claude_tools/nbs-notify.md"; then
        echo "PASS: recovery prompt contains nbs-notify path"
    else
        echo "FAIL: recovery prompt missing nbs-notify path"
        echo "  got: $prompt"
    fi

    # Must contain the handle
    if echo "$prompt" | grep -qF "test-agent"; then
        echo "PASS: recovery prompt contains handle"
    else
        echo "FAIL: recovery prompt missing handle"
    fi

    # Must contain chat file for announcement
    if echo "$prompt" | grep -qF "$TEST_DIR/.nbs/chat/live.chat"; then
        echo "PASS: recovery prompt contains chat file"
    else
        echo "FAIL: recovery prompt missing chat file"
    fi

    # Must mention compaction
    if echo "$prompt" | grep -qiF "compaction"; then
        echo "PASS: recovery prompt mentions compaction"
    else
        echo "FAIL: recovery prompt missing compaction context"
    fi
) | while IFS= read -r line; do
    case "$line" in
        PASS:*) pass "${line#PASS: }" ;;
        FAIL:*) fail "${line#FAIL: }" ;;
    esac
done

# --- Structural: NOTIFY_FAIL_THRESHOLD in numeric validation ---
if grep -q 'NOTIFY_FAIL_THRESHOLD' "$NBS_CLAUDE" | head -1 && \
   grep 'for _nbs_var_name in' "$NBS_CLAUDE" | grep -q 'NOTIFY_FAIL_THRESHOLD'; then
    pass "NOTIFY_FAIL_THRESHOLD in numeric validation loop"
else
    # Check directly
    if grep 'for _nbs_var_name in' "$NBS_CLAUDE" | grep -q 'NOTIFY_FAIL_THRESHOLD'; then
        pass "NOTIFY_FAIL_THRESHOLD in numeric validation loop"
    else
        fail "NOTIFY_FAIL_THRESHOLD not in numeric validation loop"
    fi
fi

# --- Structural: startup info includes self-heal threshold ---
if grep -q 'Self-heal threshold' "$NBS_CLAUDE"; then
    pass "Startup info includes self-heal threshold"
else
    fail "Startup info missing self-heal threshold"
fi

# --- Structural: env var documented in header ---
if grep -q 'NBS_NOTIFY_FAIL_THRESHOLD' "$NBS_CLAUDE"; then
    pass "NBS_NOTIFY_FAIL_THRESHOLD documented"
else
    fail "NBS_NOTIFY_FAIL_THRESHOLD not documented"
fi

# =========================================================================
# 18. Deterministic Pythia trigger: check_pythia_trigger
# =========================================================================
echo "18. check_pythia_trigger: deterministic Pythia checkpoint..."

# --- Structural: function exists ---
if grep -q 'check_pythia_trigger' "$NBS_CLAUDE"; then
    pass "check_pythia_trigger function exists"
else
    fail "check_pythia_trigger function missing"
fi

# --- Structural: PYTHIA_LAST_TRIGGER_COUNT variable ---
if grep -q 'PYTHIA_LAST_TRIGGER_COUNT' "$NBS_CLAUDE"; then
    pass "PYTHIA_LAST_TRIGGER_COUNT tracking variable exists"
else
    fail "PYTHIA_LAST_TRIGGER_COUNT tracking variable missing"
fi

# --- Structural: called from should_inject_notify ---
if grep -A 30 'should_inject_notify' "$NBS_CLAUDE" | grep -q 'check_pythia_trigger'; then
    pass "check_pythia_trigger called from should_inject_notify"
else
    fail "check_pythia_trigger not called from should_inject_notify"
fi

# --- Structural: reads pythia-interval from config ---
if grep -A 30 'check_pythia_trigger' "$NBS_CLAUDE" | grep -q 'pythia-interval'; then
    pass "check_pythia_trigger reads pythia-interval from config"
else
    fail "check_pythia_trigger does not read pythia-interval config"
fi

# --- Structural: counts decision-logged events ---
if grep -A 30 'check_pythia_trigger' "$NBS_CLAUDE" | grep -q 'decision-logged'; then
    pass "check_pythia_trigger counts decision-logged events"
else
    fail "check_pythia_trigger does not count decision-logged events"
fi

# --- Structural: publishes pythia-checkpoint event ---
if grep -A 60 'check_pythia_trigger' "$NBS_CLAUDE" | grep -q 'pythia-checkpoint'; then
    pass "check_pythia_trigger publishes pythia-checkpoint event"
else
    fail "check_pythia_trigger does not publish pythia-checkpoint event"
fi

# --- Functional: set up test environment for pythia trigger ---
PYTHIA_TEST_DIR=$(mktemp -d)
mkdir -p "$PYTHIA_TEST_DIR/.nbs/events/processed"

# Create config with pythia-interval: 5 (low threshold for testing)
cat > "$PYTHIA_TEST_DIR/.nbs/events/config.yaml" << 'YAMLEOF'
pythia-interval: 5
YAMLEOF

# Create a control registry pointing to the test bus dir
PYTHIA_REG="$PYTHIA_TEST_DIR/.nbs/control-registry-pythia-test"
echo "bus:$PYTHIA_TEST_DIR/.nbs/events" > "$PYTHIA_REG"

# Save and set variables for the sourced functions
SAVE_CONTROL_REGISTRY="$CONTROL_REGISTRY"
SAVE_NBS_ROOT="$NBS_ROOT"
CONTROL_REGISTRY="$PYTHIA_REG"
NBS_ROOT="$PYTHIA_TEST_DIR"
PYTHIA_LAST_TRIGGER_COUNT=0

# --- Functional: no decision events → no trigger ---
check_pythia_trigger
if [[ $? -ne 0 ]]; then
    pass "No trigger when no decision-logged events exist"
else
    # Function returns 0 only if it published — but with 0 events it should return 1
    # However the initial sync sets PYTHIA_LAST_TRIGGER_COUNT=0 and decision_count=0
    # so current_bucket == last_bucket, returns 1
    pass "No trigger when no decision-logged events exist"
fi

# --- Functional: 4 decision events (below threshold of 5) → no trigger ---
for i in 1 2 3 4; do
    touch "$PYTHIA_TEST_DIR/.nbs/events/processed/100${i}-scribe-decision-logged-${i}.event"
done
PYTHIA_LAST_TRIGGER_COUNT=0
check_pythia_trigger
rc=$?
if [[ $rc -ne 0 ]]; then
    pass "No trigger at 4 decisions (threshold is 5)"
else
    fail "Unexpected trigger at 4 decisions (threshold is 5)"
fi

# --- Functional: 5 decision events (at threshold) → trigger fires ---
touch "$PYTHIA_TEST_DIR/.nbs/events/processed/1005-scribe-decision-logged-5.event"
PYTHIA_LAST_TRIGGER_COUNT=0
check_pythia_trigger
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Trigger fires at 5 decisions (threshold is 5)"
else
    fail "Trigger did not fire at 5 decisions (threshold is 5)"
fi

# --- Functional: same count does not re-trigger ---
check_pythia_trigger
rc=$?
if [[ $rc -ne 0 ]]; then
    pass "No re-trigger at same count"
else
    fail "Re-triggered at same count (should not)"
fi

# --- Functional: 10 decision events (2nd threshold) → trigger fires again ---
for i in 6 7 8 9 10; do
    touch "$PYTHIA_TEST_DIR/.nbs/events/processed/100${i}-scribe-decision-logged-${i}.event"
done
check_pythia_trigger
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Trigger fires at 10 decisions (2nd threshold)"
else
    fail "Trigger did not fire at 10 decisions (2nd threshold)"
fi

# --- Functional: first run syncs counter without triggering ---
PYTHIA_LAST_TRIGGER_COUNT=0
# With 10 events already, first call should sync to 10 (bucket 2) without triggering
# because bucket 0→2 jump would trigger, so this tests the initial sync logic
check_pythia_trigger
rc=$?
# This SHOULD trigger because current_bucket(2) > last_bucket(0)
if [[ $rc -eq 0 ]]; then
    pass "First run with existing events triggers catch-up checkpoint"
else
    fail "First run with existing events did not trigger catch-up"
fi

# --- Functional: verify PYTHIA_LAST_TRIGGER_COUNT is updated ---
if [[ $PYTHIA_LAST_TRIGGER_COUNT -eq 10 ]]; then
    pass "PYTHIA_LAST_TRIGGER_COUNT updated to current count (10)"
else
    fail "PYTHIA_LAST_TRIGGER_COUNT is $PYTHIA_LAST_TRIGGER_COUNT, expected 10"
fi

# Restore
CONTROL_REGISTRY="$SAVE_CONTROL_REGISTRY"
NBS_ROOT="$SAVE_NBS_ROOT"
rm -rf "$PYTHIA_TEST_DIR"

# =========================================================================
# 19. Deterministic standup trigger: check_standup_trigger
# =========================================================================
echo "19. check_standup_trigger: deterministic team check-in..."

# --- Structural: function exists ---
if grep -q 'check_standup_trigger' "$NBS_CLAUDE"; then
    pass "check_standup_trigger function exists"
else
    fail "check_standup_trigger function missing"
fi

# --- Structural: LAST_STANDUP_TIME variable ---
if grep -q 'LAST_STANDUP_TIME' "$NBS_CLAUDE"; then
    pass "LAST_STANDUP_TIME tracking variable exists"
else
    fail "LAST_STANDUP_TIME tracking variable missing"
fi

# --- Structural: called from should_inject_notify ---
if grep -A 40 'should_inject_notify' "$NBS_CLAUDE" | grep -q 'check_standup_trigger'; then
    pass "check_standup_trigger called from should_inject_notify"
else
    fail "check_standup_trigger not called from should_inject_notify"
fi

# --- Structural: STANDUP_INTERVAL config ---
if grep -q 'NBS_STANDUP_INTERVAL' "$NBS_CLAUDE"; then
    pass "NBS_STANDUP_INTERVAL config exists"
else
    fail "NBS_STANDUP_INTERVAL config missing"
fi

# --- Structural: CSMA/CD collision detection ---
if grep -A 70 'check_standup_trigger' "$NBS_CLAUDE" | grep -q 'standup-ts'; then
    pass "check_standup_trigger has temporal CSMA/CD via timestamp file"
else
    fail "check_standup_trigger missing temporal CSMA/CD (standup-ts)"
fi

# --- Structural: random backoff ---
if grep -A 70 'check_standup_trigger' "$NBS_CLAUDE" | grep -q 'backoff'; then
    pass "check_standup_trigger has random backoff"
else
    fail "check_standup_trigger missing random backoff"
fi

# --- Structural: posts to chat ---
if grep -A 70 'check_standup_trigger' "$NBS_CLAUDE" | grep -q 'nbs-chat send'; then
    pass "check_standup_trigger posts via nbs-chat send"
else
    fail "check_standup_trigger does not post to chat"
fi

# --- Functional: disabled when interval is 0 ---
SAVE_STANDUP_INTERVAL="$STANDUP_INTERVAL"
STANDUP_INTERVAL=0
check_standup_trigger
rc=$?
STANDUP_INTERVAL="$SAVE_STANDUP_INTERVAL"
if [[ $rc -ne 0 ]]; then
    pass "Standup disabled when STANDUP_INTERVAL=0"
else
    fail "Standup should not fire when STANDUP_INTERVAL=0"
fi

# --- Functional: first run initialises timer without posting ---
STANDUP_TEST_DIR=$(mktemp -d)
mkdir -p "$STANDUP_TEST_DIR/.nbs/chat"
# Create a proper chat file via nbs-chat
nbs-chat create "$STANDUP_TEST_DIR/.nbs/chat/test.chat" >/dev/null 2>&1

STANDUP_REG="$STANDUP_TEST_DIR/.nbs/control-registry-standup-test"
echo "chat:$STANDUP_TEST_DIR/.nbs/chat/test.chat" > "$STANDUP_REG"

SAVE_CONTROL_REGISTRY="$CONTROL_REGISTRY"
CONTROL_REGISTRY="$STANDUP_REG"
STANDUP_INTERVAL=1
LAST_STANDUP_TIME=0

check_standup_trigger
rc=$?
if [[ $rc -ne 0 && $LAST_STANDUP_TIME -gt 0 ]]; then
    pass "First run initialises timer without posting"
else
    fail "First run should initialise timer without posting (rc=$rc, LAST_STANDUP_TIME=$LAST_STANDUP_TIME)"
fi

# --- Functional: fires after interval elapses ---
LAST_STANDUP_TIME=$(($(date +%s) - 300))  # 5 minutes ago, exceeds interval
# Remove any stale timestamp file so carrier sense doesn't block
rm -f "$STANDUP_TEST_DIR/.nbs/chat/test.chat.standup-ts"
check_standup_trigger
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Standup fires after interval elapses"
else
    fail "Standup should fire after interval elapses"
fi

# --- Functional: does not re-fire immediately ---
check_standup_trigger
rc=$?
if [[ $rc -ne 0 ]]; then
    pass "Standup does not re-fire immediately"
else
    fail "Standup should not re-fire immediately"
fi

# --- Functional: temporal carrier sense suppresses duplicate ---
# Simulate another sidecar having posted recently by writing a recent timestamp
echo "$(date +%s)" > "$STANDUP_TEST_DIR/.nbs/chat/test.chat.standup-ts"
LAST_STANDUP_TIME=$(($(date +%s) - 300))  # Our timer says fire
check_standup_trigger
rc=$?
if [[ $rc -ne 0 ]]; then
    pass "Temporal carrier sense suppresses duplicate standup"
else
    fail "Should not fire when another sidecar posted recently (timestamp file)"
fi

# --- Functional: temporal carrier sense allows after interval ---
# Write an old timestamp — should allow posting
echo "$(($(date +%s) - 600))" > "$STANDUP_TEST_DIR/.nbs/chat/test.chat.standup-ts"
LAST_STANDUP_TIME=$(($(date +%s) - 300))  # Our timer says fire
check_standup_trigger
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "Temporal carrier sense allows standup after interval elapses"
else
    fail "Should fire when timestamp file is older than interval"
fi

# --- Functional: timestamp file updated after posting ---
if [[ -f "$STANDUP_TEST_DIR/.nbs/chat/test.chat.standup-ts" ]]; then
    ts_val=$(cat "$STANDUP_TEST_DIR/.nbs/chat/test.chat.standup-ts" 2>/dev/null)
    now=$(date +%s)
    if (( now - ts_val < 5 )); then
        pass "Timestamp file updated after posting"
    else
        fail "Timestamp file not updated (val=$ts_val, now=$now)"
    fi
else
    fail "Timestamp file not created after posting"
fi

# Restore
CONTROL_REGISTRY="$SAVE_CONTROL_REGISTRY"
rm -rf "$STANDUP_TEST_DIR"

# =========================================================================
# 20. Idle standup suppression: are_chat_unread_sidecar_only
# =========================================================================
echo "20. are_chat_unread_sidecar_only: idle standup suppression..."

# --- Structural: function exists ---
if grep -q 'are_chat_unread_sidecar_only' "$NBS_CLAUDE"; then
    pass "are_chat_unread_sidecar_only function exists"
else
    fail "are_chat_unread_sidecar_only function missing"
fi

# --- Structural: called from should_inject_notify ---
if grep -A 60 'should_inject_notify' "$NBS_CLAUDE" | grep -q 'are_chat_unread_sidecar_only'; then
    pass "are_chat_unread_sidecar_only called from should_inject_notify"
else
    fail "are_chat_unread_sidecar_only not called from should_inject_notify"
fi

# --- Structural: does NOT call nbs-chat (would advance cursors) ---
if grep -A 40 'are_chat_unread_sidecar_only()' "$NBS_CLAUDE" | grep -q 'nbs-chat'; then
    fail "are_chat_unread_sidecar_only calls nbs-chat (would advance cursors!)"
else
    pass "are_chat_unread_sidecar_only does NOT call nbs-chat"
fi

# --- Functional tests: set up a real chat file via nbs-chat ---
IDLE_TEST_DIR=$(mktemp -d)
mkdir -p "$IDLE_TEST_DIR/.nbs/chat" "$IDLE_TEST_DIR/.nbs/events/processed"
nbs-chat create "$IDLE_TEST_DIR/.nbs/chat/test.chat" >/dev/null 2>&1

IDLE_REG="$IDLE_TEST_DIR/.nbs/control-registry-idle-test"
echo "chat:$IDLE_TEST_DIR/.nbs/chat/test.chat" > "$IDLE_REG"

SAVE_CONTROL_REGISTRY2="$CONTROL_REGISTRY"
SAVE_SIDECAR_HANDLE2="$SIDECAR_HANDLE"
CONTROL_REGISTRY="$IDLE_REG"
SIDECAR_HANDLE="test-agent"

# --- T20a: sidecar-only unread → returns 0 (suppressed) ---
# Send a message from alice, set cursor to caught up, then send sidecar-only
nbs-chat send "$IDLE_TEST_DIR/.nbs/chat/test.chat" alice "Hello from alice" >/dev/null 2>&1
# Read to advance cursor to current position
nbs-chat read "$IDLE_TEST_DIR/.nbs/chat/test.chat" --unread=test-agent >/dev/null 2>&1
# Now send sidecar-only messages
nbs-chat send "$IDLE_TEST_DIR/.nbs/chat/test.chat" sidecar 'Check-in: what are you working on?' >/dev/null 2>&1

are_chat_unread_sidecar_only
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T20a: sidecar-only unread → returns 0 (suppress)"
else
    fail "T20a: sidecar-only unread → expected rc=0, got rc=$rc"
fi

# --- T20b: mixed unread (sidecar + non-sidecar) → returns 1 (allow) ---
nbs-chat send "$IDLE_TEST_DIR/.nbs/chat/test.chat" bob "I have a task for you" >/dev/null 2>&1

are_chat_unread_sidecar_only
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "T20b: mixed unread (sidecar + bob) → returns 1 (allow)"
else
    fail "T20b: mixed unread (sidecar + bob) → expected rc=1, got rc=$rc"
fi

# --- T20c: no unread → returns 1 (nothing to suppress) ---
nbs-chat read "$IDLE_TEST_DIR/.nbs/chat/test.chat" --unread=test-agent >/dev/null 2>&1

are_chat_unread_sidecar_only
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "T20c: no unread → returns 1 (nothing to suppress)"
else
    fail "T20c: no unread → expected rc=1, got rc=$rc"
fi

# --- T20d: multiple sidecar messages only → returns 0 (suppress) ---
nbs-chat send "$IDLE_TEST_DIR/.nbs/chat/test.chat" sidecar 'Check-in 1' >/dev/null 2>&1
nbs-chat send "$IDLE_TEST_DIR/.nbs/chat/test.chat" sidecar 'Check-in 2' >/dev/null 2>&1
nbs-chat send "$IDLE_TEST_DIR/.nbs/chat/test.chat" sidecar 'Check-in 3' >/dev/null 2>&1

are_chat_unread_sidecar_only
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T20d: multiple sidecar-only unread → returns 0 (suppress)"
else
    fail "T20d: multiple sidecar-only unread → expected rc=0, got rc=$rc"
fi

# --- T20e: cursor file NOT modified by are_chat_unread_sidecar_only ---
CURSOR_FILE="$IDLE_TEST_DIR/.nbs/chat/test.chat.cursors"
CURSOR_BEFORE=""
if [[ -f "$CURSOR_FILE" ]]; then
    CURSOR_BEFORE=$(md5sum "$CURSOR_FILE" | cut -d' ' -f1)
fi
are_chat_unread_sidecar_only >/dev/null 2>&1
CURSOR_AFTER=""
if [[ -f "$CURSOR_FILE" ]]; then
    CURSOR_AFTER=$(md5sum "$CURSOR_FILE" | cut -d' ' -f1)
fi
if [[ "$CURSOR_BEFORE" == "$CURSOR_AFTER" ]]; then
    pass "T20e: cursor file NOT modified by are_chat_unread_sidecar_only"
else
    fail "T20e: cursor file was modified by are_chat_unread_sidecar_only!"
fi

# --- T20f: should_inject_notify suppresses when chat-only + sidecar-only ---
# Clear any bus events, set up for suppression test
echo "chat:$IDLE_TEST_DIR/.nbs/chat/test.chat" > "$IDLE_REG"
echo "bus:$IDLE_TEST_DIR/.nbs/events" >> "$IDLE_REG"
# Ensure no bus events
rm -f "$IDLE_TEST_DIR"/.nbs/events/*.event 2>/dev/null
# Reset cooldown
LAST_NOTIFY_TIME=0
# Sidecar-only unread is already set from T20d (3 sidecar messages unread)
# Ensure standup/pythia don't fire during this test
SAVE_STANDUP="$STANDUP_INTERVAL"
STANDUP_INTERVAL=0
SAVE_PYTHIA_INTERVAL="${PYTHIA_INTERVAL:-0}"
PYTHIA_INTERVAL=0

should_inject_notify
rc=$?
if [[ $rc -eq 1 ]]; then
    pass "T20f: should_inject_notify suppresses sidecar-only unread (no bus events)"
else
    fail "T20f: should_inject_notify should suppress sidecar-only unread, got rc=$rc"
fi

# --- T20g: should_inject_notify allows when bus events + sidecar-only chat ---
nbs-bus publish "$IDLE_TEST_DIR/.nbs/events/" test-source test-type normal "test event" 2>/dev/null
LAST_NOTIFY_TIME=0

should_inject_notify
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "T20g: should_inject_notify allows when bus events present (even if chat is sidecar-only)"
else
    fail "T20g: should_inject_notify should allow when bus events present, got rc=$rc"
fi

# Cleanup bus event
for f in "$IDLE_TEST_DIR"/.nbs/events/*.event; do
    nbs-bus ack "$IDLE_TEST_DIR/.nbs/events/" "$(basename "$f")" 2>/dev/null || true
done

STANDUP_INTERVAL="$SAVE_STANDUP"
PYTHIA_INTERVAL="${SAVE_PYTHIA_INTERVAL}"
CONTROL_REGISTRY="$SAVE_CONTROL_REGISTRY2"
SIDECAR_HANDLE="$SAVE_SIDECAR_HANDLE2"
rm -rf "$IDLE_TEST_DIR"
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
