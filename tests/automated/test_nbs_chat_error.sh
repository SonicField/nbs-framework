#!/bin/bash
# Test: nbs-chat error subcommand and [SIDECAR-ERROR] rendering
#
# Falsifiable tests covering:
#   1.  error subcommand posts message with handle [SIDECAR-ERROR]
#   2.  send rejects [SIDECAR-ERROR] handle (bracket guard)
#   3.  error with too few args exits 4
#   4.  error on missing file exits 2
#   5.  export routes [SIDECAR-ERROR] through error renderer (distinct ANSI)
#   6.  export does NOT change [MEDIC-WARNING] rendering (regression)
#   7.  help text lists error subcommand
#   8.  error message content survives round-trip
#   9.  [SIDECAR-ERROR] and [MEDIC-WARNING] use distinct ANSI codes (no collision)
#  10.  export --handle filter works with bracket handles

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

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

echo "=== nbs-chat error Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: error subcommand posts with [SIDECAR-ERROR] handle ---
echo "1. error subcommand posts with [SIDECAR-ERROR] handle..."
CHAT="$TEST_DIR/test1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" error "$CHAT" "query failed — could not capture output"
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "Handle is [SIDECAR-ERROR]" "$( echo "$OUTPUT" | grep -qF '[SIDECAR-ERROR]:' && echo pass || echo fail )"
check "Content preserved" "$( echo "$OUTPUT" | grep -qF 'query failed' && echo pass || echo fail )"

echo ""

# --- Test 2: send rejects [SIDECAR-ERROR] handle ---
echo "2. send rejects bracket handles..."
CHAT="$TEST_DIR/test2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
set +e
STDERR=$("$NBS_CHAT" send "$CHAT" "[SIDECAR-ERROR]" "sneaky message" 2>&1)
RC=$?
set -e
check "Exit non-zero" "$( [[ $RC -ne 0 ]] && echo pass || echo fail )"
check "Stderr mentions reserved" "$( echo "$STDERR" | grep -qi 'reserved\|bracket' && echo pass || echo fail )"
# Verify nothing was written
MSG_COUNT=$("$NBS_CHAT" read "$CHAT" 2>&1 | wc -l)
check "No message written" "$( [[ $MSG_COUNT -eq 0 ]] && echo pass || echo fail )"

echo ""

# --- Test 3: error with too few args ---
echo "3. error with too few args exits 4..."
set +e
"$NBS_CHAT" error "$TEST_DIR/dummy.chat" >/dev/null 2>&1
RC=$?
set -e
check "Exit 4 for missing message arg" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

echo ""

# --- Test 4: error on missing file ---
echo "4. error on missing file..."
set +e
"$NBS_CHAT" error "$TEST_DIR/nonexistent.chat" "test" >/dev/null 2>&1
RC=$?
set -e
# Should fail (exit 1 or 2) because file doesn't exist
check "Exit non-zero for missing file" "$( [[ $RC -ne 0 ]] && echo pass || echo fail )"

echo ""

# --- Test 5: export renders [SIDECAR-ERROR] with distinct ANSI ---
echo "5. export renders [SIDECAR-ERROR] with distinct ANSI..."
CHAT="$TEST_DIR/test5.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" "supervisor" "Normal message"
"$NBS_CHAT" error "$CHAT" "Something went wrong"

OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)

# Extract the ANSI colour code used for the error handle vs normal handle
# Normal handle gets a palette colour via render_message (e.g. \033[38;5;Nm\033[1m)
# Error handle gets NBS_STYLE_SIDECAR_ERROR via render_message_error
ERROR_LINE=$(echo "$OUTPUT" | grep 'SIDECAR-ERROR')
NORMAL_LINE=$(echo "$OUTPUT" | grep 'supervisor')

# Both lines should have ANSI escapes
check "Error line has ANSI" "$( echo "$ERROR_LINE" | grep -qP '\033\[' && echo pass || echo fail )"
check "Normal line has ANSI" "$( echo "$NORMAL_LINE" | grep -qP '\033\[' && echo pass || echo fail )"

# The error line should NOT use the same colour as the normal line
# NBS_STYLE_SIDECAR_ERROR is fg:167, so we expect \033[38;5;167m or similar
check "Error uses colour 167" "$( echo "$ERROR_LINE" | grep -qP '38;5;167' && echo pass || echo fail )"

echo ""

# --- Test 6: [MEDIC-WARNING] rendering unchanged (regression guard) ---
echo "6. [MEDIC-WARNING] rendering unchanged..."
CHAT="$TEST_DIR/test6.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" warn "$CHAT" "Reasoning quality degraded"
"$NBS_CHAT" error "$CHAT" "Query failure"

OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)

MEDIC_LINE=$(echo "$OUTPUT" | grep 'MEDIC-WARNING')
ERROR_LINE=$(echo "$OUTPUT" | grep 'SIDECAR-ERROR')

# Medic should use colour 173 (terracotta) — that's NBS_STYLE_MEDIC_WARNING
check "Medic uses colour 173" "$( echo "$MEDIC_LINE" | grep -qP '38;5;173' && echo pass || echo fail )"
# Error should use colour 167 — distinct from 173
check "Error uses colour 167" "$( echo "$ERROR_LINE" | grep -qP '38;5;167' && echo pass || echo fail )"
# They must be different
check "Medic and error colours differ" "$( [[ "$(echo "$MEDIC_LINE" | grep -oP '38;5;\d+' | head -1)" != "$(echo "$ERROR_LINE" | grep -oP '38;5;\d+' | head -1)" ]] && echo pass || echo fail )"

echo ""

# --- Test 7: help text lists error subcommand ---
echo "7. help text lists error subcommand..."
HELP_OUTPUT=$("$NBS_CHAT" help 2>&1)
check "Help mentions error" "$( echo "$HELP_OUTPUT" | grep -q 'error' && echo pass || echo fail )"
check "Help mentions SIDECAR-ERROR" "$( echo "$HELP_OUTPUT" | grep -qF '[SIDECAR-ERROR]' && echo pass || echo fail )"
# Verify warn is still there too
check "Help still mentions warn" "$( echo "$HELP_OUTPUT" | grep -q 'warn' && echo pass || echo fail )"

echo ""

# --- Test 8: error message content round-trip ---
echo "8. error message content round-trip..."
CHAT="$TEST_DIR/test8.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
# Test with special characters that exercise base64 encoding
"$NBS_CHAT" error "$CHAT" "Failed: @handle? query — 'timeout' after 10s (exit=1)"
OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "Special chars preserved" "$( echo "$OUTPUT" | grep -qF "@handle? query" && echo pass || echo fail )"
check "Quotes preserved" "$( echo "$OUTPUT" | grep -qF "'timeout'" && echo pass || echo fail )"
check "Parens preserved" "$( echo "$OUTPUT" | grep -qF "(exit=1)" && echo pass || echo fail )"

echo ""

# --- Test 9: [SIDECAR-ERROR] and [MEDIC-WARNING] use distinct ANSI ---
echo "9. No colour collision between error and medic styles..."
CHAT="$TEST_DIR/test9.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" warn "$CHAT" "medic warning"
"$NBS_CHAT" error "$CHAT" "sidecar error"
"$NBS_CHAT" send "$CHAT" "agent" "normal message"

OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)

# Extract the 256-colour code from each line type
MEDIC_COLOUR=$(echo "$OUTPUT" | grep 'MEDIC-WARNING' | grep -oP '38;5;\d+' | head -1)
ERROR_COLOUR=$(echo "$OUTPUT" | grep 'SIDECAR-ERROR' | grep -oP '38;5;\d+' | head -1)
NORMAL_COLOUR=$(echo "$OUTPUT" | grep 'agent' | grep -oP '38;5;\d+' | head -1)

check "Medic colour is 173" "$( [[ "$MEDIC_COLOUR" == "38;5;173" ]] && echo pass || echo fail )"
check "Error colour is 167" "$( [[ "$ERROR_COLOUR" == "38;5;167" ]] && echo pass || echo fail )"
check "All three are distinct" "$( [[ "$MEDIC_COLOUR" != "$ERROR_COLOUR" && "$ERROR_COLOUR" != "$NORMAL_COLOUR" && "$MEDIC_COLOUR" != "$NORMAL_COLOUR" ]] && echo pass || echo fail )"

echo ""

# --- Test 10: export --handle filter works with bracket handles ---
echo "10. export --handle filter with bracket handles..."
CHAT="$TEST_DIR/test10.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" send "$CHAT" "supervisor" "Normal 1"
"$NBS_CHAT" error "$CHAT" "Error 1"
"$NBS_CHAT" warn "$CHAT" "Warning 1"
"$NBS_CHAT" send "$CHAT" "supervisor" "Normal 2"
"$NBS_CHAT" error "$CHAT" "Error 2"

# Filter to only error messages
OUTPUT=$("$NBS_CHAT" export "$CHAT" --handle='[SIDECAR-ERROR]' 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | grep -c '.' || true)
check "Filter to errors: 2 messages" "$( [[ $LINE_COUNT -eq 2 ]] && echo pass || echo fail )"
check "All are errors" "$( ! echo "$OUTPUT" | grep -q 'supervisor\|MEDIC' && echo pass || echo fail )"

# Filter to only medic warnings
OUTPUT=$("$NBS_CHAT" export "$CHAT" --handle='[MEDIC-WARNING]' 2>&1)
LINE_COUNT=$(echo "$OUTPUT" | grep -c '.' || true)
check "Filter to warnings: 1 message" "$( [[ $LINE_COUNT -eq 1 ]] && echo pass || echo fail )"

echo ""

# --- Test 11: UTF-8 round-trip through error subcommand ---
echo "11. UTF-8 round-trip through error..."
CHAT="$TEST_DIR/test11.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
# Multibyte: em dash (3 bytes), emoji (4 bytes), CJK (3 bytes)
"$NBS_CHAT" error "$CHAT" "Query failed — timeout 🔥 错误信息 résumé"
# Verify via read
READ_OUTPUT=$("$NBS_CHAT" read "$CHAT")
check "UTF-8 em dash preserved (read)" "$( echo "$READ_OUTPUT" | grep -qF '—' && echo pass || echo fail )"
check "UTF-8 emoji preserved (read)" "$( echo "$READ_OUTPUT" | grep -qF '🔥' && echo pass || echo fail )"
check "UTF-8 CJK preserved (read)" "$( echo "$READ_OUTPUT" | grep -qF '错误信息' && echo pass || echo fail )"
check "UTF-8 accented preserved (read)" "$( echo "$READ_OUTPUT" | grep -qF 'résumé' && echo pass || echo fail )"
# Verify via export (exercises render path)
EXPORT_OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)
check "UTF-8 em dash preserved (export)" "$( echo "$EXPORT_OUTPUT" | grep -qF '—' && echo pass || echo fail )"
check "UTF-8 emoji preserved (export)" "$( echo "$EXPORT_OUTPUT" | grep -qF '🔥' && echo pass || echo fail )"
check "UTF-8 CJK preserved (export)" "$( echo "$EXPORT_OUTPUT" | grep -qF '错误信息' && echo pass || echo fail )"
check "UTF-8 accented preserved (export)" "$( echo "$EXPORT_OUTPUT" | grep -qF 'résumé' && echo pass || echo fail )"

echo ""

# --- Test 12: ANSI styling structure for [SIDECAR-ERROR] in export ---
echo "12. ANSI styling structure for error messages..."
CHAT="$TEST_DIR/test12.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null 2>&1
"$NBS_CHAT" error "$CHAT" "test error styling"
"$NBS_CHAT" send "$CHAT" "agent" "test normal styling"

OUTPUT=$("$NBS_CHAT" export "$CHAT" 2>&1)
ERROR_LINE=$(echo "$OUTPUT" | grep 'SIDECAR-ERROR')
NORMAL_LINE=$(echo "$OUTPUT" | grep 'agent')

# Error line should have bold + colour 167 in a single SGR sequence
# NBS_STYLE_SIDECAR_ERROR = { 167, NBS_COLOUR_NONE, NBS_ATTR_BOLD }
# nbs_style_start emits \033[1;38;5;167m (bold first, then colour)
check "Error has bold+colour SGR" "$( echo "$ERROR_LINE" | grep -qP '\033\[1;38;5;167m' && echo pass || echo fail )"
# Error style should reset after handle
check "Error resets after handle" "$( echo "$ERROR_LINE" | grep -qP '\033\[0m' && echo pass || echo fail )"
# Normal line should NOT use colour 167
check "Normal does not use error colour" "$( ! echo "$NORMAL_LINE" | grep -q '38;5;167' && echo pass || echo fail )"

echo ""

echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
