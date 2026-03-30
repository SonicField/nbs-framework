#!/bin/bash
# Test: nbs-chat-repair — auto-repair of corrupt chat file lines
#
# TDD tests for the feature described in feature-requests/chat-auto-repair.md
#
# Falsifiable tests covering:
#   1.  No corruption — clean file, exit 0, no changes
#   2.  Single ASCII raw text line — blanked, recovery message appended
#   3.  UTF-8 text (multi-byte) — correctly preserved in recovery
#   4.  Multi-line raw text — consecutive corrupt lines joined into one recovery section
#   5.  Binary garbage (non-UTF-8) — hex dump in recovery, bytes blanked
#   6.  Truncated base64 — blanked, partial decode attempted
#   7.  Control characters / ANSI escapes — stripped from recovery
#   8.  Long line (>64KB) — truncated with note in recovery
#   9.  Dry run — reports corruption but does not modify file
#  10.  Mixed valid + corrupt + valid — only corrupt lines affected
#  11.  Empty file (header only, no messages) — exit 0, no changes
#  12.  Already repaired (space-padded lines) — no duplicate recovery
#  13.  Invalid arguments — exit 4
#  14.  Missing file — exit 1
#  15.  File byte count preserved after in-place replacement
#  16.  Message indices unchanged after repair (cursor safety)
#  17.  Multi-byte UTF-8 byte offset — two corrupt lines (LC_ALL=C falsifier)
#  18.  Multiple separate corruption groups — correct sections and line labels
#  19.  Short corrupt line (4 bytes → 3 spaces + newline)
#  20.  Last line without trailing newline
#  21.  Binary garbage hex dump format verification
#  22.  CR to newline conversion in sanitisation
#  23.  Mixed corruption types (ASCII + binary + truncated + multi-line)
#  24.  Previous repair spaces + new corruption coexistence
#  25.  file-length header consistency after repair

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_REPAIR="${NBS_REPAIR_BIN:-$PROJECT_ROOT/bin/nbs-chat-repair}"

TEST_DIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $label"
        FAIL=$((FAIL + 1))
    fi
}

# Helper: inject a raw (corrupt) line into a chat file after the --- separator
# Appends raw text directly, bypassing nbs-chat send (which would base64-encode it)
inject_raw_line() {
    local chat_file="$1"
    local raw_text="$2"
    printf '%s\n' "$raw_text" >> "$chat_file"
}

# Helper: inject raw bytes (from printf format) into a chat file
inject_raw_bytes() {
    local chat_file="$1"
    shift
    printf "$@" >> "$chat_file"
}

# Helper: get the byte offset of the --- separator line end
get_body_start() {
    local chat_file="$1"
    grep -n '^---$' "$chat_file" | head -1 | cut -d: -f1
}

# Helper: count messages that nbs-chat read can decode
# Counts timestamp-prefixed lines (each message starts with [YYYY-...])
count_valid_messages() {
    local chat_file="$1"
    local count
    count=$("$NBS_CHAT" read "$chat_file" 2>/dev/null | grep -c '^\[' || true)
    echo "$count"
}

echo "=== nbs-chat-repair Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: No corruption — clean file exits 0, no changes ---
echo "1. No corruption — clean file..."
CHAT="$TEST_DIR/t1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "hello" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" bob "world" >/dev/null 2>&1
SIZE_BEFORE=$(wc -c < "$CHAT")
MSGS_BEFORE=$(count_valid_messages "$CHAT")
set +e
OUTPUT=$("$NBS_REPAIR" "$CHAT" 2>&1)
RC=$?
set -e
SIZE_AFTER=$(wc -c < "$CHAT")
MSGS_AFTER=$(count_valid_messages "$CHAT")
check "Exit 0 for clean file" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "No size change" "$( [[ "$SIZE_BEFORE" -eq "$SIZE_AFTER" ]] && echo pass || echo fail )"
check "No message count change" "$( [[ "$MSGS_BEFORE" -eq "$MSGS_AFTER" ]] && echo pass || echo fail )"
check "Reports no corruption" "$( echo "$OUTPUT" | grep -qi 'no corruption' && echo pass || echo fail )"
echo ""

# --- Test 2: Single ASCII raw text line ---
echo "2. Single ASCII raw text line..."
CHAT="$TEST_DIR/t2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "before the corruption" >/dev/null 2>&1
inject_raw_line "$CHAT" "@supervisor I fixed the bug"
"$NBS_CHAT" send "$CHAT" bob "after the corruption" >/dev/null 2>&1
MSGS_BEFORE=$(count_valid_messages "$CHAT")
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
check "Exit 0 after repair" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# Recovery message should be appended — read the file and check
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Recovery message appended" "$( echo "$CHAT_OUTPUT" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"
check "Recovered text contains original" "$( echo "$CHAT_OUTPUT" | grep -qF '@supervisor I fixed the bug' && echo pass || echo fail )"
check "Handle is chat-repair" "$( echo "$CHAT_OUTPUT" | grep -qF 'chat-repair' && echo pass || echo fail )"
# Original valid messages still readable
check "alice message preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'before the corruption' && echo pass || echo fail )"
check "bob message preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'after the corruption' && echo pass || echo fail )"
echo ""

# --- Test 3: UTF-8 text (multi-byte) ---
echo "3. UTF-8 multi-byte text..."
CHAT="$TEST_DIR/t3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "context message" >/dev/null 2>&1
inject_raw_line "$CHAT" "这是中文测试消息"
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "UTF-8 preserved in recovery" "$( echo "$CHAT_OUTPUT" | grep -qF '这是中文测试消息' && echo pass || echo fail )"
echo ""

# --- Test 4: Multi-line raw text (consecutive corrupt lines) ---
echo "4. Multi-line raw text — consecutive corrupt lines joined..."
CHAT="$TEST_DIR/t4.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "normal message" >/dev/null 2>&1
inject_raw_line "$CHAT" "@supervisor I fixed three bugs:"
inject_raw_line "$CHAT" "1. The segfault"
inject_raw_line "$CHAT" "2. The shutdown crash"
"$NBS_CHAT" send "$CHAT" bob "another normal" >/dev/null 2>&1
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# All three lines should appear in ONE recovery section, not three separate ones
# Count recovery sections — should be exactly 1 (one group of consecutive corrupt lines)
RECOVERY_SECTIONS=$(echo "$CHAT_OUTPUT" | grep -c 'Recovered text' || true)
check "Single recovery section for consecutive lines" "$( [[ "$RECOVERY_SECTIONS" -eq 1 ]] && echo pass || echo fail )"
check "Contains first line" "$( echo "$CHAT_OUTPUT" | grep -qF '@supervisor I fixed three bugs:' && echo pass || echo fail )"
check "Contains second line" "$( echo "$CHAT_OUTPUT" | grep -qF '1. The segfault' && echo pass || echo fail )"
check "Contains third line" "$( echo "$CHAT_OUTPUT" | grep -qF '2. The shutdown crash' && echo pass || echo fail )"
echo ""

# --- Test 5: Binary garbage (non-UTF-8 bytes) ---
echo "5. Binary garbage..."
CHAT="$TEST_DIR/t5.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid message" >/dev/null 2>&1
# Inject bytes that are not valid UTF-8: 0x80 0xFE 0xFF followed by newline
inject_raw_bytes "$CHAT" '\x80\xFE\xFF\xC0\xC1\xDE\xAD\xBE\xEF\n'
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Recovery mentions unrecoverable or hex" "$( echo "$CHAT_OUTPUT" | grep -qiE 'unrecoverable|hex|byte' && echo pass || echo fail )"
echo ""

# --- Test 6: Truncated base64 ---
echo "6. Truncated base64 (length not multiple of 4)..."
CHAT="$TEST_DIR/t6.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid" >/dev/null 2>&1
# Valid base64 "YWxpY2V8MTcwMDAwMDAwMDogaGVsbG8=" truncated to non-multiple-of-4
inject_raw_line "$CHAT" "YWxpY2V8MTcwMDAwMDAwMDogaGVsbG"
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Recovery message present" "$( echo "$CHAT_OUTPUT" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"
echo ""

# --- Test 7: Control characters and ANSI escapes ---
echo "7. Control characters and ANSI escapes stripped..."
CHAT="$TEST_DIR/t7.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid" >/dev/null 2>&1
# Inject text with ANSI escape (bold red "ERROR") and some control chars
inject_raw_bytes "$CHAT" 'This has \x1b[1;31mERROR\x1b[0m and \x01\x02 control chars\n'
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "ANSI escapes stripped" "$( echo "$CHAT_OUTPUT" | grep -qF $'\x1b' && echo fail || echo pass )"
check "Text content preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'ERROR' && echo pass || echo fail )"
check "Control chars stripped" "$( echo "$CHAT_OUTPUT" | grep -qP '\x01|\x02' && echo fail || echo pass )"
echo ""

# --- Test 8: Long line (>64KB) truncated ---
echo "8. Long line truncated at 64KB..."
CHAT="$TEST_DIR/t8.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid" >/dev/null 2>&1
# Generate a 100KB line of raw text (spaces make it non-base64)
LONG_LINE=$(python3 -c "print('This is a very long corrupt message. ' * 2844, end='')")
inject_raw_line "$CHAT" "$LONG_LINE"
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Recovery mentions truncation" "$( echo "$CHAT_OUTPUT" | grep -qiE 'truncat' && echo pass || echo fail )"
echo ""

# --- Test 9: Dry run ---
echo "9. Dry run reports but does not modify..."
CHAT="$TEST_DIR/t9.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid message" >/dev/null 2>&1
inject_raw_line "$CHAT" "this is raw corrupt text"
cp "$CHAT" "$TEST_DIR/t9.chat.backup"
SIZE_BEFORE=$(wc -c < "$CHAT")
set +e
OUTPUT=$("$NBS_REPAIR" "$CHAT" --dry-run 2>&1)
RC=$?
set -e
SIZE_AFTER=$(wc -c < "$CHAT")
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "File unchanged" "$( diff -q "$CHAT" "$TEST_DIR/t9.chat.backup" >/dev/null 2>&1 && echo pass || echo fail )"
check "Size unchanged" "$( [[ "$SIZE_BEFORE" -eq "$SIZE_AFTER" ]] && echo pass || echo fail )"
check "Output describes corruption" "$( echo "$OUTPUT" | grep -qiE 'corrupt|raw|repair' && echo pass || echo fail )"
echo ""

# --- Test 10: Mixed valid + corrupt + valid ---
echo "10. Mixed — only corrupt lines affected..."
CHAT="$TEST_DIR/t10.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "message one" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" bob "message two" >/dev/null 2>&1
inject_raw_line "$CHAT" "corrupt line here"
"$NBS_CHAT" send "$CHAT" charlie "message three" >/dev/null 2>&1
VALID_BEFORE=$(count_valid_messages "$CHAT")
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
# Should have original 3 valid + 1 recovery = 4 valid messages
VALID_AFTER=$(count_valid_messages "$CHAT")
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Original messages preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'message one' && echo "$CHAT_OUTPUT" | grep -qF 'message two' && echo "$CHAT_OUTPUT" | grep -qF 'message three' && echo pass || echo fail )"
check "Recovery appended" "$( echo "$CHAT_OUTPUT" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"
check "Valid count increased by 1 (recovery msg)" "$( [[ "$VALID_AFTER" -eq $((VALID_BEFORE + 1)) ]] && echo pass || echo fail )"
echo ""

# --- Test 11: Empty file (header only) ---
echo "11. Empty file — header only, no messages..."
CHAT="$TEST_DIR/t11.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
SIZE_BEFORE=$(wc -c < "$CHAT")
set +e
OUTPUT=$("$NBS_REPAIR" "$CHAT" 2>&1)
RC=$?
set -e
SIZE_AFTER=$(wc -c < "$CHAT")
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "No size change" "$( [[ "$SIZE_BEFORE" -eq "$SIZE_AFTER" ]] && echo pass || echo fail )"
check "Reports no corruption" "$( echo "$OUTPUT" | grep -qi 'no corruption' && echo pass || echo fail )"
echo ""

# --- Test 12: Already repaired — space-padded lines cause no duplicate recovery ---
echo "12. Already repaired — idempotent..."
CHAT="$TEST_DIR/t12.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid" >/dev/null 2>&1
inject_raw_line "$CHAT" "corrupt text here"
# First repair
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
set -e
MSGS_AFTER_FIRST=$(count_valid_messages "$CHAT")
# Second repair — should find no corruption
set +e
OUTPUT=$("$NBS_REPAIR" "$CHAT" 2>&1)
RC=$?
set -e
MSGS_AFTER_SECOND=$(count_valid_messages "$CHAT")
check "Exit 0 on second run" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "No new messages on second run" "$( [[ "$MSGS_AFTER_FIRST" -eq "$MSGS_AFTER_SECOND" ]] && echo pass || echo fail )"
check "Reports no corruption on second run" "$( echo "$OUTPUT" | grep -qi 'no corruption' && echo pass || echo fail )"
echo ""

# --- Test 13: Invalid arguments — exit 4 ---
echo "13. Invalid arguments..."
set +e
"$NBS_REPAIR" >/dev/null 2>&1
RC=$?
set -e
check "No args exits 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
echo ""

# --- Test 14: Missing file — exit 1 ---
echo "14. Missing file..."
set +e
"$NBS_REPAIR" "$TEST_DIR/nonexistent.chat" >/dev/null 2>&1
RC=$?
set -e
check "Missing file exits 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
echo ""

# --- Test 15: File byte count preserved after in-place replacement ---
echo "15. File byte count preserved..."
CHAT="$TEST_DIR/t15.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "message" >/dev/null 2>&1
inject_raw_line "$CHAT" "this corrupt line is exactly here"
"$NBS_CHAT" send "$CHAT" bob "another" >/dev/null 2>&1
# Record file size including the corrupt line but BEFORE repair appends recovery
# We need to check that the in-place blank preserves the byte count of the
# region before the appended recovery message.
# Strategy: compute expected size = original size + recovery message size
# Simpler: check that lines before the recovery message have same total bytes
SIZE_BEFORE=$(wc -c < "$CHAT")
# Count the number of lines before repair
LINES_BEFORE=$(wc -l < "$CHAT")
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
set -e
# The file grew (recovery message appended), but the original region should
# have the same byte count. Check by examining the corrupt line is now spaces.
# Find the line that was corrupt — it should now be all spaces
BODY_START=$(get_body_start "$CHAT")
# The corrupt line was the 3rd line after --- (alice msg, corrupt, bob msg)
CORRUPT_LINE_NUM=$((BODY_START + 2))
REPLACED_LINE=$(sed -n "${CORRUPT_LINE_NUM}p" "$CHAT")
ORIG_LEN=${#REPLACED_LINE}
# Check it's all spaces (after stripping trailing newline from sed)
STRIPPED=$(echo "$REPLACED_LINE" | tr -d ' ')
check "Corrupt line replaced with spaces" "$( [[ -z "$STRIPPED" ]] && echo pass || echo fail )"
# Verify the replaced line has same length as original corrupt line
# "this corrupt line is exactly here" = 34 chars, replaced with 34 spaces
ORIG_CORRUPT="this corrupt line is exactly here"
check "Replacement preserves line byte count" "$( [[ "$ORIG_LEN" -eq "${#ORIG_CORRUPT}" ]] && echo pass || echo fail )"
echo ""

# --- Test 16: Message indices unchanged after repair (cursor safety) ---
echo "16. Cursor safety — message indices unchanged..."
CHAT="$TEST_DIR/t16.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "first" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" bob "second" >/dev/null 2>&1
inject_raw_line "$CHAT" "corrupt in the middle"
"$NBS_CHAT" send "$CHAT" charlie "third" >/dev/null 2>&1
# Read messages before repair — should get 3 valid messages (corrupt skipped)
BEFORE_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
BEFORE_MSG1=$(echo "$BEFORE_OUTPUT" | head -1)
BEFORE_MSG2=$(echo "$BEFORE_OUTPUT" | sed -n '2p')
BEFORE_MSG3=$(echo "$BEFORE_OUTPUT" | sed -n '3p')
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
set -e
# Read first 3 messages after repair — should be identical to before
AFTER_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
AFTER_MSG1=$(echo "$AFTER_OUTPUT" | head -1)
AFTER_MSG2=$(echo "$AFTER_OUTPUT" | sed -n '2p')
AFTER_MSG3=$(echo "$AFTER_OUTPUT" | sed -n '3p')
check "Message 1 unchanged" "$( [[ "$BEFORE_MSG1" == "$AFTER_MSG1" ]] && echo pass || echo fail )"
check "Message 2 unchanged" "$( [[ "$BEFORE_MSG2" == "$AFTER_MSG2" ]] && echo pass || echo fail )"
check "Message 3 unchanged" "$( [[ "$BEFORE_MSG3" == "$AFTER_MSG3" ]] && echo pass || echo fail )"
# 4th message should be the recovery
AFTER_MSG4=$(echo "$AFTER_OUTPUT" | sed -n '4p')
check "4th message is recovery" "$( echo "$AFTER_MSG4" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"
echo ""

# --- Test 17: Multi-byte UTF-8 byte offset — two corrupt lines ---
# Falsifier: if byte offsets use character count instead of byte count,
# a multi-byte corrupt line (14 chars = 42 bytes) shifts all subsequent
# offsets by 28 bytes. A second dd write at the wrong offset would
# overwrite a valid base64 message instead of the second corrupt line.
echo "17. Multi-byte UTF-8 byte offset — two corrupt lines..."
CHAT="$TEST_DIR/t17.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "message before" >/dev/null 2>&1
# First corrupt line: 14 Chinese chars × 3 bytes = 42 bytes (but ${#} = 14 in UTF-8 locale)
inject_raw_line "$CHAT" "这是中文测试消息需要正确处理"
"$NBS_CHAT" send "$CHAT" bob "message between corruptions" >/dev/null 2>&1
# Second corrupt line: ASCII
inject_raw_line "$CHAT" "second corrupt line after utf8"
"$NBS_CHAT" send "$CHAT" charlie "message after second corruption" >/dev/null 2>&1
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "alice preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'message before' && echo pass || echo fail )"
check "bob preserved between corruptions" "$( echo "$CHAT_OUTPUT" | grep -qF 'message between corruptions' && echo pass || echo fail )"
check "charlie preserved after second corruption" "$( echo "$CHAT_OUTPUT" | grep -qF 'message after second corruption' && echo pass || echo fail )"
check "First corrupt text recovered" "$( echo "$CHAT_OUTPUT" | grep -qF '这是中文测试消息需要正确处理' && echo pass || echo fail )"
check "Second corrupt text recovered" "$( echo "$CHAT_OUTPUT" | grep -qF 'second corrupt line after utf8' && echo pass || echo fail )"
echo ""

# --- Test 18: Multiple separate corruption groups ---
# Spec: "If multiple groups of consecutive corrupt lines exist, each group
# gets its own section with line numbers."
echo "18. Multiple separate corruption groups..."
CHAT="$TEST_DIR/t18.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "first valid" >/dev/null 2>&1
inject_raw_line "$CHAT" "group one line one"
inject_raw_line "$CHAT" "group one line two"
"$NBS_CHAT" send "$CHAT" bob "second valid" >/dev/null 2>&1
inject_raw_line "$CHAT" "group two isolated"
"$NBS_CHAT" send "$CHAT" charlie "third valid" >/dev/null 2>&1
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# Should have 2 recovery sections (group 1: consecutive, group 2: isolated)
RECOVERY_SECTIONS=$(echo "$CHAT_OUTPUT" | grep -cE 'Recovered text|Unrecoverable' || true)
check "Two recovery sections" "$( [[ "$RECOVERY_SECTIONS" -eq 2 ]] && echo pass || echo fail )"
# Group 1 should say "lines" (plural), group 2 should say "line" (singular)
check "Group 1 uses plural 'lines'" "$( echo "$CHAT_OUTPUT" | grep -qF 'lines ' && echo pass || echo fail )"
check "Group 1 content present" "$( echo "$CHAT_OUTPUT" | grep -qF 'group one line one' && echo pass || echo fail )"
check "Group 2 content present" "$( echo "$CHAT_OUTPUT" | grep -qF 'group two isolated' && echo pass || echo fail )"
check "All valid messages preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'first valid' && echo "$CHAT_OUTPUT" | grep -qF 'second valid' && echo "$CHAT_OUTPUT" | grep -qF 'third valid' && echo pass || echo fail )"
echo ""

# --- Test 19: Short corrupt line (4 bytes → 3 spaces + newline) ---
# Spec edge case: "A 4-byte corrupt line becomes 3 spaces + newline.
# chat_read sees a 3-character line, checks 3 % 4 != 0, skips it."
echo "19. Short corrupt line (4 bytes)..."
CHAT="$TEST_DIR/t19.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "before short" >/dev/null 2>&1
# "bug" is 3 chars — not multiple of 4 and contains no base64-invalid chars
# but length 3 % 4 != 0, so it's corrupt
inject_raw_line "$CHAT" "bug"
"$NBS_CHAT" send "$CHAT" bob "after short" >/dev/null 2>&1
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Recovery contains 'bug'" "$( echo "$CHAT_OUTPUT" | grep -qF 'bug' && echo pass || echo fail )"
check "bob still readable" "$( echo "$CHAT_OUTPUT" | grep -qF 'after short' && echo pass || echo fail )"
# Verify the replaced line is 3 spaces (the original "bug" was 3 bytes)
BODY_START=$(get_body_start "$CHAT")
CORRUPT_LINE_NUM=$((BODY_START + 2))
REPLACED=$(LC_ALL=C sed -n "${CORRUPT_LINE_NUM}p" "$CHAT")
check "Replaced line is 3 spaces" "$( [[ "$REPLACED" == "   " ]] && echo pass || echo fail )"
echo ""

# --- Test 20: Last line without trailing newline ---
# Spec edge case: "If the last line of the file has no trailing newline,
# the replacement is N space characters with no newline."
echo "20. Last line without trailing newline..."
CHAT="$TEST_DIR/t20.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid message" >/dev/null 2>&1
# Append corrupt text WITHOUT trailing newline (use printf -n equivalent)
printf '%s' "no newline at end" >> "$CHAT"
SIZE_BEFORE=$(wc -c < "$CHAT")
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Recovery present" "$( echo "$CHAT_OUTPUT" | grep -qF '[AUTO-REPAIR]' && echo pass || echo fail )"
check "alice preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'valid message' && echo pass || echo fail )"
echo ""

# --- Test 21: Binary garbage hex dump format ---
# Spec: "report byte count and hex dump of the first 40 bytes for diagnostic"
echo "21. Binary garbage hex dump format..."
CHAT="$TEST_DIR/t21.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid" >/dev/null 2>&1
# Inject known binary bytes so we can verify the hex dump
inject_raw_bytes "$CHAT" '\xDE\xAD\xBE\xEF\xCA\xFE\n'
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Hex dump contains deadbeef" "$( echo "$CHAT_OUTPUT" | grep -qi 'deadbeef' && echo pass || echo fail )"
check "Hex dump contains cafe" "$( echo "$CHAT_OUTPUT" | grep -qi 'cafe' && echo pass || echo fail )"
check "Reports byte count" "$( echo "$CHAT_OUTPUT" | grep -qE '[0-9]+ bytes' && echo pass || echo fail )"
echo ""

# --- Test 22: CR to newline conversion ---
# Spec: "0x0D (carriage return) — convert to newline"
echo "22. CR to newline conversion..."
CHAT="$TEST_DIR/t22.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "valid" >/dev/null 2>&1
# Inject text with embedded \r (carriage return within the line)
inject_raw_bytes "$CHAT" 'line one\rline two\n'
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# CR should be converted to newline, not left as \r
check "No CR in recovery" "$( echo "$CHAT_OUTPUT" | grep -qP '\r' && echo fail || echo pass )"
check "Both parts preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'line one' && echo "$CHAT_OUTPUT" | grep -qF 'line two' && echo pass || echo fail )"
echo ""

# --- Test 23: Mixed corruption types in one file ---
# Real-world scenario: a file with ASCII corruption, binary garbage, and
# a truncated base64 line, all in the same file. Tests interaction between
# different recovery paths and correct grouping.
echo "23. Mixed corruption types in one file..."
CHAT="$TEST_DIR/t23.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "message one" >/dev/null 2>&1
# Type 1: raw ASCII
inject_raw_line "$CHAT" "@supervisor status update"
"$NBS_CHAT" send "$CHAT" bob "message two" >/dev/null 2>&1
# Type 5: binary garbage
inject_raw_bytes "$CHAT" '\xFF\xFE\xFD\xFC\xFB\n'
"$NBS_CHAT" send "$CHAT" charlie "message three" >/dev/null 2>&1
# Type 3: truncated base64 (length not multiple of 4)
inject_raw_line "$CHAT" "YWxpY2V8MTcwMDAwMDAwMDog"
# Type 6: multi-line raw text (consecutive)
inject_raw_line "$CHAT" "first line of multi"
inject_raw_line "$CHAT" "second line of multi"
"$NBS_CHAT" send "$CHAT" dave "message four" >/dev/null 2>&1
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# All 4 valid messages must survive
check "alice preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'message one' && echo pass || echo fail )"
check "bob preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'message two' && echo pass || echo fail )"
check "charlie preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'message three' && echo pass || echo fail )"
check "dave preserved" "$( echo "$CHAT_OUTPUT" | grep -qF 'message four' && echo pass || echo fail )"
# Recovery should contain the ASCII text
check "ASCII corruption recovered" "$( echo "$CHAT_OUTPUT" | grep -qF '@supervisor status update' && echo pass || echo fail )"
# Binary should be unrecoverable
check "Binary garbage flagged" "$( echo "$CHAT_OUTPUT" | grep -qiE 'unrecoverable|hex|byte' && echo pass || echo fail )"
# Multi-line should be grouped
check "Multi-line content present" "$( echo "$CHAT_OUTPUT" | grep -qF 'first line of multi' && echo pass || echo fail )"
# Should have multiple recovery/unrecoverable sections
SECTIONS=$(echo "$CHAT_OUTPUT" | grep -cE 'Recovered text|Unrecoverable' || true)
check "Multiple recovery sections" "$( [[ "$SECTIONS" -ge 3 ]] && echo pass || echo fail )"
echo ""

# --- Test 24: Space-padded from previous repair + new corruption ---
# Verifies that space-padded lines from a previous repair are skipped
# while genuinely new corrupt lines are still detected and repaired.
echo "24. Previous repair spaces + new corruption..."
CHAT="$TEST_DIR/t24.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "original" >/dev/null 2>&1
inject_raw_line "$CHAT" "first corruption"
"$NBS_CHAT" send "$CHAT" bob "middle" >/dev/null 2>&1
# First repair — blanks "first corruption"
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
set -e
MSGS_AFTER_FIRST=$(count_valid_messages "$CHAT")
# Now inject NEW corruption after the first repair
inject_raw_line "$CHAT" "brand new corruption after repair"
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
MSGS_AFTER_SECOND=$(count_valid_messages "$CHAT")
CHAT_OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "New corruption recovered" "$( echo "$CHAT_OUTPUT" | grep -qF 'brand new corruption after repair' && echo pass || echo fail )"
# Should have exactly 1 more message (the new recovery)
check "One new recovery message" "$( [[ "$MSGS_AFTER_SECOND" -eq $((MSGS_AFTER_FIRST + 1)) ]] && echo pass || echo fail )"
# The original recovery from first repair should still be there
RECOVERY_COUNT=$(echo "$CHAT_OUTPUT" | grep -c '\[AUTO-REPAIR\]' || true)
check "Both repair messages present" "$( [[ "$RECOVERY_COUNT" -eq 2 ]] && echo pass || echo fail )"
echo ""

# --- Test 25: file-length header consistency after repair ---
# Spec: "file-length header remains valid" after in-place replacement.
# The blank preserves byte count, then nbs-chat send updates file-length
# when appending the recovery. Verify the header matches actual file size.
echo "25. file-length header consistency..."
CHAT="$TEST_DIR/t25.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" alice "test message" >/dev/null 2>&1
inject_raw_line "$CHAT" "corrupt data for header test"
"$NBS_CHAT" send "$CHAT" bob "another test" >/dev/null 2>&1
set +e
"$NBS_REPAIR" "$CHAT" >/dev/null 2>&1
RC=$?
set -e
# Extract file-length from header and compare to actual file size
HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | awk '{print $2}')
ACTUAL_LENGTH=$(wc -c < "$CHAT")
check "Exit 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "file-length header matches actual size" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"
echo ""

# --- Summary ---
echo "================================"
echo "Results: $PASS passed, $FAIL failed out of $((PASS + FAIL))"
echo "================================"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
