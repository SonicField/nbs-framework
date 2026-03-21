#!/bin/bash
# Test: nbs-chat-terminal --goal-file injection
#
# Tests all error paths and the happy path for goal file injection.
# Does NOT test --restart (that requires agent infrastructure).
# Does NOT leave any state — all temp files cleaned up.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
TERMINAL="${PROJECT_ROOT}/bin/nbs-chat-terminal"
CHAT="${PROJECT_ROOT}/bin/nbs-chat"

PASS=0
FAIL=0
TOTAL=0

check() {
    local name="$1"
    local result="$2"
    TOTAL=$((TOTAL + 1))
    if [[ "$result" == "0" ]]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

check_exit() {
    local name="$1"
    local expected="$2"
    local actual="$3"
    TOTAL=$((TOTAL + 1))
    if [[ "$actual" == "$expected" ]]; then
        echo "  PASS: $name (exit $actual)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected exit $expected, got $actual)"
        FAIL=$((FAIL + 1))
    fi
}

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

CHAT_FILE="${TMPDIR}/test.chat"
GOAL_FILE="${TMPDIR}/goal.md"

[[ -x "$TERMINAL" ]] || { echo "FATAL: nbs-chat-terminal not found"; exit 1; }
[[ -x "$CHAT" ]] || { echo "FATAL: nbs-chat not found"; exit 1; }

# Create test chat file
"$CHAT" create "$CHAT_FILE" >/dev/null 2>&1

echo "Test: nbs-chat-terminal --goal-file"
echo ""

# ═══════════════════════════════════════════════════════════════
# Error path tests — must abort without modifying chat
# ═══════════════════════════════════════════════════════════════

echo "=== Error paths (must abort, chat unchanged) ==="

# Record initial chat state
INITIAL_SIZE=$(stat -c %s "$CHAT_FILE")

# Test 1: missing goal file
echo ""
echo "Test 1: missing goal file"
rc=0; timeout 3 "$TERMINAL" "$CHAT_FILE" alex --goal-file=/nonexistent >/dev/null 2>&1 || rc=$?
# timeout returns 124 on timeout, but we expect exit 1 before timeout
check_exit "missing file aborts" 1 "$rc"
SIZE=$(stat -c %s "$CHAT_FILE")
[[ "$SIZE" == "$INITIAL_SIZE" ]]; check "chat unchanged after missing file" $?

# Test 2: empty goal file
echo ""
echo "Test 2: empty goal file"
touch "${TMPDIR}/empty.md"
rc=0; timeout 3 "$TERMINAL" "$CHAT_FILE" alex --goal-file="${TMPDIR}/empty.md" >/dev/null 2>&1 || rc=$?
check_exit "empty file aborts" 1 "$rc"
SIZE=$(stat -c %s "$CHAT_FILE")
[[ "$SIZE" == "$INITIAL_SIZE" ]]; check "chat unchanged after empty file" $?

# Test 3: directory instead of file
echo ""
echo "Test 3: directory instead of file"
mkdir -p "${TMPDIR}/adir"
rc=0; timeout 3 "$TERMINAL" "$CHAT_FILE" alex --goal-file="${TMPDIR}/adir" >/dev/null 2>&1 || rc=$?
check_exit "directory aborts" 1 "$rc"
SIZE=$(stat -c %s "$CHAT_FILE")
[[ "$SIZE" == "$INITIAL_SIZE" ]]; check "chat unchanged after directory" $?

# Test 4: file too large (>64KB)
echo ""
echo "Test 4: file too large"
dd if=/dev/urandom bs=1 count=65537 of="${TMPDIR}/big.md" 2>/dev/null
rc=0; timeout 3 "$TERMINAL" "$CHAT_FILE" alex --goal-file="${TMPDIR}/big.md" >/dev/null 2>&1 || rc=$?
check_exit "too-large file aborts" 1 "$rc"
SIZE=$(stat -c %s "$CHAT_FILE")
[[ "$SIZE" == "$INITIAL_SIZE" ]]; check "chat unchanged after too-large file" $?

# Test 5: binary file (contains null bytes)
echo ""
echo "Test 5: binary file"
printf 'goal\x00binary' > "${TMPDIR}/binary.md"
rc=0; timeout 3 "$TERMINAL" "$CHAT_FILE" alex --goal-file="${TMPDIR}/binary.md" >/dev/null 2>&1 || rc=$?
check_exit "binary file aborts" 1 "$rc"
SIZE=$(stat -c %s "$CHAT_FILE")
[[ "$SIZE" == "$INITIAL_SIZE" ]]; check "chat unchanged after binary file" $?

# Test 6: unreadable file
echo ""
echo "Test 6: unreadable file"
echo "secret goal" > "${TMPDIR}/noperm.md"
chmod 000 "${TMPDIR}/noperm.md"
rc=0; timeout 3 "$TERMINAL" "$CHAT_FILE" alex --goal-file="${TMPDIR}/noperm.md" >/dev/null 2>&1 || rc=$?
check_exit "unreadable file aborts" 1 "$rc"
SIZE=$(stat -c %s "$CHAT_FILE")
[[ "$SIZE" == "$INITIAL_SIZE" ]]; check "chat unchanged after unreadable file" $?
chmod 644 "${TMPDIR}/noperm.md"  # restore for cleanup

# ═══════════════════════════════════════════════════════════════
# Happy path tests — goal injected, chat modified
# ═══════════════════════════════════════════════════════════════

echo ""
echo "=== Happy path (goal injected into chat) ==="

# Test 7: basic injection
echo ""
echo "Test 7: basic injection"
cat > "$GOAL_FILE" << 'GOAL'
# Session Goal: Test Optimisation

Terminal goal: benchmark X >= 1.0x.
GOAL
rc=0; timeout 5 "$TERMINAL" "$CHAT_FILE" testuser --goal-file="$GOAL_FILE" >/dev/null 2>&1 || rc=$?
# Will exit via timeout (124) since no stdin, but goal should be injected
CONTENT=$("$CHAT" read "$CHAT_FILE" --last=1 2>/dev/null)
echo "$CONTENT" | grep -q 'Session Goal: Test Optimisation'; check "goal content in chat" $?
echo "$CONTENT" | grep -q 'testuser'; check "posted as correct handle" $?

# Test 8: multiline content preserved
echo ""
echo "Test 8: multiline content preserved"
echo "$CONTENT" | grep -q 'Terminal goal: benchmark X >= 1.0x.'; check "multiline content preserved" $?

# Test 9: special characters preserved
echo ""
echo "Test 9: special characters"
cat > "${TMPDIR}/special.md" << 'GOAL'
Goal with special chars: <html> & "quotes" 'apostrophes' $VARS `backticks`
GOAL
"$CHAT" create "${TMPDIR}/special.chat" >/dev/null 2>&1
rc=0; timeout 5 "$TERMINAL" "${TMPDIR}/special.chat" alex --goal-file="${TMPDIR}/special.md" >/dev/null 2>&1 || rc=$?
CONTENT=$("$CHAT" read "${TMPDIR}/special.chat" --last=1 2>/dev/null)
echo "$CONTENT" | grep -q '<html>'; check "HTML chars preserved" $?
echo "$CONTENT" | grep -q '&'; check "ampersand preserved" $?
echo "$CONTENT" | grep -q '\$VARS'; check "dollar sign preserved" $?

# Test 10: goal appears before restart would read
echo ""
echo "Test 10: ordering (goal before restart)"
"$CHAT" create "${TMPDIR}/order.chat" >/dev/null 2>&1
"$CHAT" send "${TMPDIR}/order.chat" prior "message before goal" >/dev/null 2>&1
cat > "${TMPDIR}/order_goal.md" << 'GOAL'
NEW SESSION GOAL
GOAL
rc=0; timeout 5 "$TERMINAL" "${TMPDIR}/order.chat" alex --goal-file="${TMPDIR}/order_goal.md" >/dev/null 2>&1 || rc=$?
LAST=$("$CHAT" read "${TMPDIR}/order.chat" --last=1 2>/dev/null)
echo "$LAST" | grep -q 'NEW SESSION GOAL'; check "goal is the last message (before restart)" $?

# Test 11: exactly 64KB file (boundary)
echo ""
echo "Test 11: exactly 64KB file (boundary)"
"$CHAT" create "${TMPDIR}/boundary.chat" >/dev/null 2>&1
dd if=/dev/zero bs=1 count=65536 of="${TMPDIR}/boundary.md" 2>/dev/null
# Replace nulls with 'x' to make it text
tr '\0' 'x' < "${TMPDIR}/boundary.md" > "${TMPDIR}/boundary_text.md"
rc=0; timeout 5 "$TERMINAL" "${TMPDIR}/boundary.chat" alex --goal-file="${TMPDIR}/boundary_text.md" >/dev/null 2>&1 || rc=$?
# Should succeed (64KB = limit, not over)
CONTENT=$("$CHAT" read "${TMPDIR}/boundary.chat" --last=1 2>/dev/null)
[[ -n "$CONTENT" ]]; check "64KB file accepted (at boundary)" $?

# ═══════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════

echo ""
echo "────────────────────────────────────────"
echo "Results: ${PASS} pass, ${FAIL} fail, ${TOTAL} total"
echo "────────────────────────────────────────"

[[ "$FAIL" -eq 0 ]]
