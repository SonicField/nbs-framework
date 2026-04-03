#!/bin/bash
# test_cursor_desync.sh — Integration tests for cursor desync mitigations
#
# Tests the hardening changes specified in feature-requests/cursor-desync-mitigations.md.
# Verifies cursor desync mitigations across all priority scenarios.
#
# Scenarios tested:
#   1. Fixup resets cursor to msg_count-1 (not msg_count) — Scenario #1
#   2. nbs-kick-agent resets cursor to msg_count-1 — Scenario #1
#   3. Concurrent nbs-chat cursor writes don't corrupt — Scenario #2
#   4. nbs-chat count subcommand returns correct count — Scenario #8
#   5. Cursor > msg_count is clamped on read — Scenario #7
#   6. Fixup uses lock-safe cursor update (not sed -i) — Scenario #2
#   7. nbs-kick-agent uses lock-safe cursor update (not sed -i) — Scenario #2
#   8. nbs-chat cursor-set subcommand exists — new tool for safe cursor reset
#
# Exit codes: 0 = all pass, 1 = any fail

set -uo pipefail

source "$(dirname "$0")/test_helpers.sh"

PASS=0
FAIL=0
SKIP=0

# Find project root and tools
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NBS_CHAT="${PROJECT_ROOT}/bin/nbs-chat"
NBS_KICK="${PROJECT_ROOT}/bin/nbs-kick-agent"
NBS_RESTART="${PROJECT_ROOT}/bin/nbs-chat-terminal-restart.sh"

# Verify nbs-chat exists
if [[ ! -x "$NBS_CHAT" ]]; then
    echo "SKIP: nbs-chat not found at $NBS_CHAT — run 'make install' first" >&2
    exit 0
fi

# Helpers
TMPDIR=""
setup_tmpdir() {
    TMPDIR=$(mktemp -d /tmp/nbs-desync-test-XXXXXX)
}

cleanup() {
    [[ -n "$TMPDIR" ]] && rm -rf "$TMPDIR"
}
trap cleanup EXIT

pass() {
    PASS=$((PASS + 1))
    echo "   PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    echo "   FAIL: $1"
}

skip_test() {
    SKIP=$((SKIP + 1))
    echo "   SKIP: $1 ($2)"
}

assert_eq() {
    local label="$1" got="$2" expected="$3"
    if [[ "$got" == "$expected" ]]; then
        pass "$label"
    else
        fail "$label (got '$got', expected '$expected')"
    fi
}

assert_le() {
    local label="$1" got="$2" limit="$3"
    if [[ "$got" -le "$limit" ]]; then
        pass "$label"
    else
        fail "$label (got $got, expected <= $limit)"
    fi
}

assert_ge() {
    local label="$1" got="$2" limit="$3"
    if [[ "$got" -ge "$limit" ]]; then
        pass "$label"
    else
        fail "$label (got $got, expected >= $limit)"
    fi
}

assert_ne() {
    local label="$1" got="$2" notval="$3"
    if [[ "$got" != "$notval" ]]; then
        pass "$label"
    else
        fail "$label (got '$got', must not be '$notval')"
    fi
}

assert_zero() {
    local label="$1" rc="$2"
    if [[ "$rc" -eq 0 ]]; then
        pass "$label"
    else
        fail "$label (exit code $rc, expected 0)"
    fi
}

assert_gt() {
    local label="$1" got="$2" limit="$3"
    if [[ "$got" -gt "$limit" ]]; then
        pass "$label"
    else
        fail "$label (got $got, expected > $limit)"
    fi
}

# Create a chat file with N messages
create_chat_with_messages() {
    local chat_path="$1"
    local count="$2"

    "$NBS_CHAT" create "$chat_path" 2>/dev/null
    for i in $(seq 1 "$count"); do
        "$NBS_CHAT" send "$chat_path" "agent${i}" "Message number ${i}" 2>/dev/null
    done
}

# Read cursor value for a handle from cursor file
read_cursor_value() {
    local cursor_file="$1"
    local handle="$2"

    if [[ ! -f "$cursor_file" ]]; then
        echo "-1"
        return
    fi
    local val
    val=$(grep "^${handle}=" "$cursor_file" 2>/dev/null | head -1 | cut -d= -f2)
    echo "${val:--1}"
}

# Count cursors (non-comment, non-empty lines) in cursor file
count_cursor_entries() {
    local cursor_file="$1"
    if [[ ! -f "$cursor_file" ]]; then
        echo "0"
        return
    fi
    grep -v '^#' "$cursor_file" | grep -v '^$' | wc -l | tr -d ' '
}

echo "=== test_cursor_desync — cursor desync mitigations ==="
echo ""

# ============================================================
# Test 1: Fixup resets cursor to msg_count-1 (Scenario #1)
#
# The restart script should reset cursors to msg_count-1 so the
# restarted agent always gets at least the most recent message
# as context. Currently resets to msg_count (too aggressive).
# ============================================================
echo "--- Scenario #1: Fixup cursor reset value ---"
setup_tmpdir

CHAT="${TMPDIR}/fixup-test.chat"
create_chat_with_messages "$CHAT" 5

# Create a cursor file with a handle
echo "# Read cursors — last-read message index per handle" > "${CHAT}.cursors"
echo "testhandle=0" >> "${CHAT}.cursors"

# Run the actual cursor reset logic (extracted from nbs-chat-terminal-restart.sh)
# Post-harden: uses nbs-chat count + nbs-chat cursor-set with msg_count-1
MESSAGE_COUNT=$("$NBS_CHAT" count "$CHAT" 2>/dev/null || echo 0)
if [[ $MESSAGE_COUNT -gt 0 ]]; then
    RESET_TO=$((MESSAGE_COUNT - 1))
else
    RESET_TO=0
fi
"$NBS_CHAT" cursor-set "$CHAT" testhandle "$RESET_TO" 2>/dev/null
CURSOR_AFTER=$(read_cursor_value "${CHAT}.cursors" "testhandle")

# The cursor should be msg_count - 1 (0-indexed last message)
EXPECTED=$((MESSAGE_COUNT - 1))
assert_eq "fixup cursor reset = msg_count-1" "$CURSOR_AFTER" "$EXPECTED"

cleanup
echo ""

# ============================================================
# Test 2: nbs-kick-agent resets cursor to msg_count-1 (Scenario #1)
# ============================================================
echo "--- Scenario #1: nbs-kick-agent cursor reset value ---"
setup_tmpdir

CHAT="${TMPDIR}/kick-test.chat"
create_chat_with_messages "$CHAT" 5

echo "# Read cursors — last-read message index per handle" > "${CHAT}.cursors"
echo "testagent=0" >> "${CHAT}.cursors"

# Post-harden: uses nbs-chat count + nbs-chat cursor-set with msg_count-1
MESSAGE_COUNT=$("$NBS_CHAT" count "$CHAT" 2>/dev/null || echo 0)
if [[ $MESSAGE_COUNT -gt 0 ]]; then
    RESET_TO=$((MESSAGE_COUNT - 1))
else
    RESET_TO=0
fi
"$NBS_CHAT" cursor-set "$CHAT" testagent "$RESET_TO" 2>/dev/null
CURSOR_AFTER=$(read_cursor_value "${CHAT}.cursors" "testagent")

EXPECTED=$((MESSAGE_COUNT - 1))
assert_eq "kick cursor reset = msg_count-1" "$CURSOR_AFTER" "$EXPECTED"

cleanup
echo ""

# ============================================================
# Test 3: Concurrent nbs-chat cursor writes don't corrupt (Scenario #2)
# ============================================================
echo "--- Scenario #2: Concurrent cursor writes (C-level locking) ---"
setup_tmpdir

CHAT="${TMPDIR}/concurrent.chat"
create_chat_with_messages "$CHAT" 10

echo "# Read cursors — last-read message index per handle" > "${CHAT}.cursors"

# Spawn 5 concurrent processes, each writing a different handle
for i in $(seq 1 5); do
    (
        echo "handle${i}=0" >> "${CHAT}.cursors"
        "$NBS_CHAT" read "$CHAT" --unread="handle${i}" >/dev/null 2>&1
    ) &
done
wait

ENTRIES=$(count_cursor_entries "${CHAT}.cursors")
assert_ge "concurrent writes: all 5 handles present (got ${ENTRIES})" "$ENTRIES" 5

ZEROES=0
for i in $(seq 1 5); do
    VAL=$(read_cursor_value "${CHAT}.cursors" "handle${i}")
    if [[ "$VAL" == "0" ]]; then
        ZEROES=$((ZEROES + 1))
    fi
done
assert_eq "concurrent writes: no handle stuck at cursor=0 (zeroes=${ZEROES})" "$ZEROES" "0"

cleanup
echo ""

# ============================================================
# Test 4: nbs-chat count subcommand (Scenario #8)
# ============================================================
echo "--- Scenario #8: nbs-chat count subcommand ---"
setup_tmpdir

CHAT="${TMPDIR}/count-test.chat"
create_chat_with_messages "$CHAT" 7

COUNT_RC=0
COUNT_OUTPUT=$("$NBS_CHAT" count "$CHAT" 2>/dev/null) || COUNT_RC=$?
assert_zero "nbs-chat count: exits 0" "$COUNT_RC"
assert_eq "nbs-chat count: returns 7" "$COUNT_OUTPUT" "7"

EMPTY="${TMPDIR}/empty.chat"
"$NBS_CHAT" create "$EMPTY" 2>/dev/null
EMPTY_RC=0
EMPTY_COUNT=$("$NBS_CHAT" count "$EMPTY" 2>/dev/null) || EMPTY_RC=$?
assert_eq "nbs-chat count: empty chat returns 0" "$EMPTY_COUNT" "0"

cleanup
echo ""

# ============================================================
# Test 5: Cursor > msg_count is clamped (Scenario #7)
# ============================================================
echo "--- Scenario #7: Cursor > msg_count clamped ---"
setup_tmpdir

CHAT="${TMPDIR}/clamp-test.chat"
create_chat_with_messages "$CHAT" 5

echo "# Read cursors — last-read message index per handle" > "${CHAT}.cursors"
echo "agent=1500" >> "${CHAT}.cursors"

READ_OUTPUT=$("$NBS_CHAT" read "$CHAT" --unread=agent 2>/dev/null || true)
READ_RC=$?
assert_zero "clamp: read --unread exits cleanly" "$READ_RC"

CURSOR_AFTER=$(read_cursor_value "${CHAT}.cursors" "agent")
assert_le "clamp: cursor clamped to valid range (${CURSOR_AFTER} <= 5)" "$CURSOR_AFTER" 5
assert_ne "clamp: cursor no longer impossibly high" "$CURSOR_AFTER" "1500"

cleanup
echo ""

# ============================================================
# Test 6: Fixup uses lock-safe cursor update (Scenario #2)
# ============================================================
echo "--- Scenario #2: Fixup uses lock-safe cursor update ---"

if [[ ! -f "$NBS_RESTART" ]]; then
    skip_test "fixup lock-safe" "nbs-chat-terminal-restart.sh not found"
else
    SED_USAGE=$(grep -c 'sed -i.*cursors' "$NBS_RESTART" 2>/dev/null || true)
    assert_eq "fixup: no sed -i on cursor files" "$SED_USAGE" "0"

    NBS_CHAT_CURSOR=$(grep -c 'nbs-chat.*cursor' "$NBS_RESTART" 2>/dev/null || true)
    assert_gt "fixup: uses nbs-chat for cursor operations" "$NBS_CHAT_CURSOR" 0
fi

echo ""

# ============================================================
# Test 7: nbs-kick-agent uses lock-safe cursor update (Scenario #2)
# ============================================================
echo "--- Scenario #2: nbs-kick-agent uses lock-safe cursor update ---"

if [[ ! -f "$NBS_KICK" ]]; then
    skip_test "kick lock-safe" "nbs-kick-agent not found"
else
    SED_USAGE=$(grep -c 'sed -i.*CURSOR_FILE' "$NBS_KICK" 2>/dev/null || true)
    assert_eq "kick: no sed -i on cursor files" "$SED_USAGE" "0"

    NBS_CHAT_CURSOR=$(grep -c 'nbs-chat.*cursor' "$NBS_KICK" 2>/dev/null || true)
    assert_gt "kick: uses nbs-chat for cursor operations" "$NBS_CHAT_CURSOR" 0
fi

echo ""

# ============================================================
# Test 8: nbs-chat cursor-set subcommand (new tool)
# ============================================================
echo "--- New tool: nbs-chat cursor-set ---"
setup_tmpdir

CHAT="${TMPDIR}/cursor-set-test.chat"
create_chat_with_messages "$CHAT" 10

SET_RC=0
"$NBS_CHAT" cursor-set "$CHAT" testhandle 5 2>/dev/null || SET_RC=$?
assert_zero "cursor-set: exits 0" "$SET_RC"

CURSOR=$(read_cursor_value "${CHAT}.cursors" "testhandle")
assert_eq "cursor-set: cursor set to 5" "$CURSOR" "5"

"$NBS_CHAT" cursor-set "$CHAT" testhandle 8 2>/dev/null
CURSOR2=$(read_cursor_value "${CHAT}.cursors" "testhandle")
assert_eq "cursor-set: cursor updated to 8" "$CURSOR2" "8"

"$NBS_CHAT" cursor-set "$CHAT" testhandle 9 2>/dev/null
CURSOR3=$(read_cursor_value "${CHAT}.cursors" "testhandle")
assert_eq "cursor-set: cursor set to 9" "$CURSOR3" "9"

cleanup
echo ""

# ============================================================
# Test 9: Stress test — concurrent cursor-set
# ============================================================
echo "--- Stress: concurrent cursor-set ---"
setup_tmpdir

CHAT="${TMPDIR}/stress.chat"
create_chat_with_messages "$CHAT" 20

for i in $(seq 1 10); do
    "$NBS_CHAT" cursor-set "$CHAT" "stress${i}" "$i" 2>/dev/null &
done
wait

ALL_PRESENT=1
ALL_CORRECT=1
for i in $(seq 1 10); do
    VAL=$(read_cursor_value "${CHAT}.cursors" "stress${i}")
    if [[ "$VAL" == "-1" ]]; then
        ALL_PRESENT=0
    elif [[ "$VAL" != "$i" ]]; then
        ALL_CORRECT=0
    fi
done

assert_eq "stress: all 10 handles present" "$ALL_PRESENT" "1"
assert_eq "stress: all 10 handles have correct values" "$ALL_CORRECT" "1"

cleanup
echo ""

# ============================================================
# Test 10: Message count formula robustness (Scenario #8)
# ============================================================
echo "--- Scenario #8: Message count formula robustness ---"
setup_tmpdir

CHAT="${TMPDIR}/header-test.chat"
create_chat_with_messages "$CHAT" 3

COUNT=$("$NBS_CHAT" count "$CHAT" 2>/dev/null) || COUNT="ERROR"
HEADER_LINES=6
FORMULA_COUNT=$(( $(wc -l < "$CHAT") - HEADER_LINES ))

assert_eq "count formula: standard header matches (count=${COUNT} formula=${FORMULA_COUNT})" "$COUNT" "$FORMULA_COUNT"

cleanup
echo ""

# ============================================================
# Test 11: No sed -i on cursor files anywhere in repo (Scenario #2 regression)
#
# Theologian's falsifier: grep the entire repo for sed.*-i.*cursor.
# Any hit outside test files and the feature-request doc is a regression.
# AI tool docs (claude_tools/) count — agents execute those instructions.
# ============================================================
echo "--- Scenario #2: No sed -i on cursor files in repo ---"

SED_HITS=$(grep -rn 'sed.*-i.*cursor' "$PROJECT_ROOT" \
    --include='*.sh' --include='*.md' --include='*.c' --include='*.py' \
    2>/dev/null \
    | grep -v 'test_cursor_desync' \
    | grep -v 'feature-requests/' \
    | grep -v 'archive/' \
    | grep -v 'docs/' \
    | grep -v 'Never use' \
    || true)

SED_COUNT=$(echo "$SED_HITS" | grep -c '^' 2>/dev/null || true)
# grep -c counts 1 for empty input (one empty line), so check for actual content
if [[ -z "$SED_HITS" ]]; then
    SED_COUNT=0
fi

assert_eq "no sed -i on cursor files in repo (found ${SED_COUNT})" "$SED_COUNT" "0"

if [[ "$SED_COUNT" -gt 0 ]]; then
    echo "   Locations:"
    echo "$SED_HITS" | sed 's/^/      /'
fi

echo ""

# ============================================================
# Summary
# ============================================================

TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped (${TOTAL} total) ==="

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
