#!/bin/bash
# Test: bin/nbs-chat-init audit violation fixes
#
# Integration tests for the 14 violations fixed in bin/nbs-chat-init:
#
# SECURITY (3):
#   1. Unquoted glob + ls + xargs replaced with find -print0
#   2. --dangerously-skip-permissions documented (code inspection only)
#   3. CHAT_NAME validated against ^[a-zA-Z0-9_-]+$
#
# BUG (4):
#   4. nbs-bus publish return value checked
#   5. nbs-chat create/send return values checked
#   6. || true replaced with proper error handling
#   7. mv/cat bypass of run() documented or routed through run()
#
# HARDENING (7):
#   8.  PROJECT_ROOT validated as directory
#   9.  $* already quoted inside double-quoted echo (no-op — verified by inspection)
#   10. sha256sum failure checked
#   11. Derived paths resolved to absolute
#   12. Arithmetic vars use ${var:-0} defaults
#   13. nbs-bus ack failure logged as warning
#   14. assert_tool_exists added for sha256sum, date, grep, sed, basename
#
# Some violations (2, 7, 9, 12) are structural/code-inspection only. These
# tests verify the externally observable fixes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT_INIT="${PROJECT_ROOT}/bin/nbs-chat-init"

# Verify script exists
if [[ ! -x "$NBS_CHAT_INIT" ]]; then
    echo "SKIP: nbs-chat-init not found or not executable at $NBS_CHAT_INIT"
    exit 0
fi

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

echo "=== bin/nbs-chat-init Audit Fix Tests ==="
echo "Test dir: $TEST_DIR"
echo ""

# ============================================================
# Test 1: CHAT_NAME validation (Violation 3: SECURITY)
# ============================================================
echo "1. CHAT_NAME validation (Violation 3: SECURITY)..."

set +e

# Valid names should be accepted (dry-run to avoid needing real tools)
VALID_OUT=$("$NBS_CHAT_INIT" --name=valid-name_123 --dry-run 2>&1)
VALID_RC=$?
check "Valid CHAT_NAME accepted (alphanumeric, hyphen, underscore)" \
    "$( [[ "$VALID_RC" -eq 0 ]] && echo pass || echo fail )"

# Names with spaces should be rejected
SPACE_OUT=$("$NBS_CHAT_INIT" --name="bad name" --dry-run 2>&1)
SPACE_RC=$?
check "CHAT_NAME with spaces rejected" \
    "$( [[ "$SPACE_RC" -eq 4 ]] && echo pass || echo fail )"
check "CHAT_NAME space error mentions 'invalid'" \
    "$( echo "$SPACE_OUT" | grep -qi 'invalid' && echo pass || echo fail )"

# Names with shell metacharacters should be rejected
SHELL_OUT=$("$NBS_CHAT_INIT" --name='test;rm -rf /' --dry-run 2>&1)
SHELL_RC=$?
check "CHAT_NAME with semicolon rejected" \
    "$( [[ "$SHELL_RC" -eq 4 ]] && echo pass || echo fail )"

SLASH_OUT=$("$NBS_CHAT_INIT" --name='../etc/passwd' --dry-run 2>&1)
SLASH_RC=$?
check "CHAT_NAME with slashes rejected" \
    "$( [[ "$SLASH_RC" -eq 4 ]] && echo pass || echo fail )"

DOLLAR_OUT=$("$NBS_CHAT_INIT" --name='test$(whoami)' --dry-run 2>&1)
DOLLAR_RC=$?
check "CHAT_NAME with dollar-parens rejected" \
    "$( [[ "$DOLLAR_RC" -eq 4 ]] && echo pass || echo fail )"

BACKTICK_OUT=$("$NBS_CHAT_INIT" --name='test\`id\`' --dry-run 2>&1)
BACKTICK_RC=$?
check "CHAT_NAME with backticks rejected" \
    "$( [[ "$BACKTICK_RC" -eq 4 ]] && echo pass || echo fail )"

# Empty name should still be rejected (pre-existing check)
EMPTY_OUT=$("$NBS_CHAT_INIT" --name= 2>&1)
EMPTY_RC=$?
check "Empty CHAT_NAME rejected" \
    "$( [[ "$EMPTY_RC" -eq 4 ]] && echo pass || echo fail )"

set -e
echo ""

# ============================================================
# Test 2: PROJECT_ROOT validation (Violation 8: HARDENING)
# ============================================================
echo "2. PROJECT_ROOT validation (Violation 8: HARDENING)..."

# The script resolves PROJECT_ROOT from pwd. We verify the validation
# exists by checking the source for assert_directory_exists.
check "Source contains PROJECT_ROOT validation" \
    "$( grep -q 'assert_directory_exists.*PROJECT_ROOT' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 3: Derived paths are absolute (Violation 11: HARDENING)
# ============================================================
echo "3. Derived paths are absolute (Violation 11: HARDENING)..."

# Check that CHAT_FILE and SCRIBE_LOG use PROJECT_ROOT prefix
check "CHAT_FILE uses absolute path" \
    "$( grep -q 'CHAT_FILE="\${PROJECT_ROOT}/' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "SCRIBE_LOG uses absolute path" \
    "$( grep -q 'SCRIBE_LOG="\${PROJECT_ROOT}/' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "PROJECT_ROOT resolved via pwd -P" \
    "$( grep -q 'pwd -P' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 4: assert_tool_exists for all required binaries (Violation 14: HARDENING)
# ============================================================
echo "4. assert_tool_exists for required binaries (Violation 14: HARDENING)..."

for tool in nbs-chat nbs-bus sha256sum date grep sed basename; do
    check "assert_tool_exists for $tool" \
        "$( grep -q "assert_tool_exists \"$tool\"" "$NBS_CHAT_INIT" && echo pass || echo fail )"
done

# tmux and nbs-claude are checked conditionally (spawn modes)
check "assert_tool_exists for tmux (conditional)" \
    "$( grep -q 'assert_tool_exists "tmux"' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "assert_tool_exists for nbs-claude (conditional)" \
    "$( grep -q 'assert_tool_exists "nbs-claude"' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 5: sha256sum failure checked (Violation 10: HARDENING)
# ============================================================
echo "5. sha256sum failure checked (Violation 10: HARDENING)..."

check "generate_project_id checks sha256sum return" \
    "$( grep -A5 'generate_project_id' "$NBS_CHAT_INIT" | grep -q 'sha256sum.*||' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 6: nbs-bus publish failure handling (Violations 4, 6: BUG)
# ============================================================
echo "6. nbs-bus publish failure handling (Violations 4, 6: BUG)..."

# Self-test publish should check return value
check "Self-test publish checks return value" \
    "$( grep -q 'if ! nbs-bus publish.*self-test' "$NBS_CHAT_INIT" && echo pass || echo fail )"

# || true should be removed
check "No || true after nbs-bus publish" \
    "$( grep 'nbs-bus publish' "$NBS_CHAT_INIT" | grep -q '|| true' && echo fail || echo pass )"

# ai-spawned publish should check return value
check "ai-spawned publish checks return value" \
    "$( grep -q 'if ! nbs-bus publish.*ai-spawned' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 7: nbs-chat create/send failure handling (Violation 5: BUG)
# ============================================================
echo "7. nbs-chat create/send failure handling (Violation 5: BUG)..."

check "nbs-chat create checks return value" \
    "$( grep -q 'if ! nbs-chat create' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "nbs-chat send checks return value" \
    "$( grep -q 'if ! nbs-chat send' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 8: nbs-bus ack failure logged (Violation 13: HARDENING)
# ============================================================
echo "8. nbs-bus ack failure logged (Violation 13: HARDENING)..."

check "nbs-bus ack checks return value" \
    "$( grep -q 'if ! nbs-bus ack' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "nbs-bus ack failure produces warning" \
    "$( grep -A1 'if ! nbs-bus ack' "$NBS_CHAT_INIT" | grep -q 'warn' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 9: find -print0 replaces ls + xargs (Violation 1: SECURITY)
# ============================================================
echo "9. find -print0 replaces ls + xargs (Violation 1: SECURITY)..."

check "No unquoted ls glob for event files" \
    "$( grep 'ls .nbs/events/\*' "$NBS_CHAT_INIT" && echo fail || echo pass )"
check "find -print0 used instead" \
    "$( grep -q 'find .nbs/events/.*-print0' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "xargs -0 used with find" \
    "$( grep -q 'xargs -0' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 10: --dangerously-skip-permissions documented (Violation 2: SECURITY)
# ============================================================
echo "10. --dangerously-skip-permissions documented (Violation 2: SECURITY)..."

check "Security risk comment present" \
    "$( grep -q 'SECURITY RISK' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "Comment mentions permission bypass" \
    "$( grep -q 'permission system' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "Comment references architectural decision" \
    "$( grep -q 'Architectural decision' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 11: mv/cat bypass documented or routed through run() (Violation 7: BUG)
# ============================================================
echo "11. mv/cat bypass of run() documented or fixed (Violation 7: BUG)..."

# The mv in compact_decision_log should go through run()
check "Compaction mv routed through run()" \
    "$( grep -q 'run mv.*log_file.*archive_file' "$NBS_CHAT_INIT" && echo pass || echo fail )"

# cat heredocs should have documentation comments
BYPASS_COMMENTS=$(grep -c 'cat heredoc bypasses run()' "$NBS_CHAT_INIT")
check "cat heredoc bypass comments present (>= 3)" \
    "$( [[ "$BYPASS_COMMENTS" -ge 3 ]] && echo pass || echo fail )"

# echo redirect should be documented
check "echo redirect bypass documented" \
    "$( grep -q 'echo redirect bypasses run()' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 12: Arithmetic defaults (Violation 12: HARDENING)
# ============================================================
echo "12. Arithmetic defaults (Violation 12: HARDENING)..."

check "wait_count uses \${var:-0} form" \
    "$( grep -q '\${wait_count:-0}' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "max_wait uses \${var:-60} form" \
    "$( grep -q '\${max_wait:-60}' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "entry_count uses \${var:-0} form" \
    "$( grep -q '\${entry_count:-0}' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 13: Dry-run mode still works (regression)
# ============================================================
echo "13. Dry-run mode regression..."

# Run with dry-run — should produce output without error
# (This exercises most code paths without needing nbs-bus/nbs-chat)
set +e
DRYRUN_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name=drytest --dry-run --force 2>&1)
DRYRUN_RC=$?
set -e

check "Dry-run exits 0" \
    "$( [[ "$DRYRUN_RC" -eq 0 ]] && echo pass || echo fail )"
check "Dry-run mentions DRY-RUN" \
    "$( echo "$DRYRUN_OUT" | grep -q 'DRY-RUN' && echo pass || echo fail )"
check "Dry-run shows phase output" \
    "$( echo "$DRYRUN_OUT" | grep -q 'Phase 1' && echo pass || echo fail )"
check "Dry-run shows absolute chat path" \
    "$( echo "$DRYRUN_OUT" | grep -q "${TEST_DIR}.*/\.nbs/chat/drytest.chat" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 14: Unknown argument rejection (regression)
# ============================================================
echo "14. Unknown argument rejection (regression)..."

set +e
UNKNOWN_OUT=$("$NBS_CHAT_INIT" --name=test --bogus-arg 2>&1)
UNKNOWN_RC=$?
set -e

check "Unknown argument exits 4" \
    "$( [[ "$UNKNOWN_RC" -eq 4 ]] && echo pass || echo fail )"
check "Unknown argument mentioned in error" \
    "$( echo "$UNKNOWN_OUT" | grep -q 'bogus-arg' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 15: Help still works (regression)
# ============================================================
echo "15. Help output (regression)..."

set +e
HELP_OUT=$("$NBS_CHAT_INIT" --help 2>&1)
HELP_RC=$?
set -e

check "Help exits 0" \
    "$( [[ "$HELP_RC" -eq 0 ]] && echo pass || echo fail )"
check "Help shows usage" \
    "$( echo "$HELP_OUT" | grep -q 'Usage' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 16: Adversarial CHAT_NAME edge cases
# ============================================================
echo "16. Adversarial CHAT_NAME edge cases..."

set +e

# Newlines in name
NL_OUT=$("$NBS_CHAT_INIT" --name=$'test\nname' --dry-run 2>&1)
NL_RC=$?
check "CHAT_NAME with newline rejected" \
    "$( [[ "$NL_RC" -eq 4 ]] && echo pass || echo fail )"

# Null bytes (shell will typically strip these, but verify no crash)
NULL_OUT=$("$NBS_CHAT_INIT" --name=$'test\x00name' --dry-run 2>&1)
NULL_RC=$?
check "CHAT_NAME with null byte does not crash" \
    "$( [[ "$NULL_RC" -eq 4 || "$NULL_RC" -eq 0 ]] && echo pass || echo fail )"

# Very long name (100 chars — should be accepted)
LONG_NAME=$(printf 'a%.0s' {1..100})
LONG_OUT=$("$NBS_CHAT_INIT" --name="$LONG_NAME" --dry-run --force 2>&1)
LONG_RC=$?
check "100-char alphanumeric CHAT_NAME accepted" \
    "$( [[ "$LONG_RC" -eq 0 ]] && echo pass || echo fail )"

# Dot in name (should be rejected — not in allowed charset)
DOT_OUT=$("$NBS_CHAT_INIT" --name='test.name' --dry-run 2>&1)
DOT_RC=$?
check "CHAT_NAME with dot rejected" \
    "$( [[ "$DOT_RC" -eq 4 ]] && echo pass || echo fail )"

set -e
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
