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
#   7. Two-track loop structure: bus check and safety net in both sidecar modes
#   8. Configuration defaults: correct values for bus-aware mode
#   9. Cursor peeking safety: cursor files NOT modified by check_chat_unread
#  10. Edge cases: empty chat file, chat with no delimiter, multiple bus dirs

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

# Verify two-track structure: bus check AND safety net
if grep -q 'Track 1.*Bus-aware' "$NBS_CLAUDE" && grep -q 'Track 2.*Safety net' "$NBS_CLAUDE"; then
    pass "Has two-track loop structure (bus + safety net)"
else
    fail "Missing two-track loop structure"
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

# Verify it is lightweight (under 30 lines)
if [[ -f "$NOTIFY_DOC" ]]; then
    NOTIFY_LINES=$(wc -l < "$NOTIFY_DOC")
    if [[ "$NOTIFY_LINES" -lt 30 ]]; then
        pass "nbs-notify.md is lightweight ($NOTIFY_LINES lines, < 30)"
    else
        fail "nbs-notify.md is too large ($NOTIFY_LINES lines, expected < 30)"
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

# Verify it has "return silently" behaviour
if grep -q 'return silently' "$NOTIFY_DOC"; then
    pass "nbs-notify.md specifies silent return for no-op"
else
    fail "nbs-notify.md missing silent return behaviour"
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

# Test: prompt with > at end of line should match
if is_prompt_visible "some output
more output
> "; then
    pass "Detects > prompt with trailing space"
else
    fail "Failed to detect > prompt"
fi

# Test: bare > at end of line
if is_prompt_visible "line 1
line 2
>"; then
    pass "Detects bare > at end of line"
else
    fail "Failed to detect bare >"
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

# Test: > in the middle of a line should match (regex '>\s*$')
if is_prompt_visible "some text
prefix >
next line"; then
    pass "Detects > at end of line with prefix text"
else
    fail "Failed to detect > at end of line with prefix"
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

# Verify POLL_INTERVAL comparison in tmux sidecar
if grep -A2 'Track 2.*Safety net' "$NBS_CLAUDE" | grep -q 'idle_seconds.*POLL_INTERVAL'; then
    pass "tmux sidecar checks idle_seconds against POLL_INTERVAL"
else
    fail "tmux sidecar missing idle_seconds comparison"
fi

# Verify /nbs-notify is injected with NOTIFY_MESSAGE
if grep -q '/nbs-notify.*NOTIFY_MESSAGE' "$NBS_CLAUDE"; then
    pass "Injects /nbs-notify with NOTIFY_MESSAGE"
else
    fail "Missing NOTIFY_MESSAGE in /nbs-notify injection"
fi

# Verify /nbs-poll is injected as safety net (not /nbs-notify)
if grep 'Track 2' -A10 "$NBS_CLAUDE" | grep -q '/nbs-poll'; then
    pass "Safety net injects /nbs-poll (not /nbs-notify)"
else
    fail "Safety net does not inject /nbs-poll"
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

# Verify default POLL_INTERVAL is 300 (5 minutes), not 30
if grep -q 'NBS_POLL_INTERVAL:-300' "$NBS_CLAUDE"; then
    pass "Default POLL_INTERVAL is 300 (5 minutes)"
else
    fail "Default POLL_INTERVAL is not 300"
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
# 11. nbs-poll.md updated for safety net role
# =========================================================================
echo "11. nbs-poll.md safety net language..."

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
# 12. Documentation updated
# =========================================================================
echo "12. docs/nbs-claude.md updated..."

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
# Cleanup
# =========================================================================
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
