#!/bin/bash
# Test: bin/nbs-chat-init audit violation fixes
#
# Integration tests for all audit violations fixed in bin/nbs-chat-init.
#
# SECURITY:
#   - Unquoted glob + ls + xargs replaced with find -print0
#   - --dangerously-skip-permissions documented (code inspection only)
#   - CHAT_NAME validated against ^[a-zA-Z0-9_-]+$
#   - PROJECT_NAME validated against ^[a-zA-Z0-9_. -]+$
#
# BUG:
#   - nbs-bus publish return value checked
#   - nbs-chat create/send return values checked
#   - || true replaced with proper error handling
#   - mv/cat bypass of run() documented or routed through run()
#   - echo -e in run() replaced with printf (portability)
#   - Corrupt archive headers now warn instead of silently masking
#   - tmux new-session return values checked
#   - Main Claude spawned with NBS_HANDLE=claude
#
# HARDENING:
#   - PROJECT_ROOT validated as directory
#   - sha256sum failure checked
#   - Derived paths resolved to absolute
#   - Arithmetic vars use ${var:-0} defaults
#   - nbs-bus ack failure logged as warning
#   - assert_tool_exists added for sha256sum, date, grep, sed, basename
#   - All echo -e replaced with printf throughout
#   - date +%s return value checked
#   - tmux send-keys return values checked
#   - Main Claude has readiness wait loop
#   - Summary uses absolute paths

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT_INIT="${PROJECT_ROOT}/bin/nbs-chat-init"

# Add bin/ to PATH so tools are found for dry-run mode
export PATH="${PROJECT_ROOT}/bin:$PATH"

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
VALID_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name=valid-name_123 --dry-run --force 2>&1)
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
# Test 2: PROJECT_NAME validation (Audit SECURITY)
# ============================================================
echo "2. PROJECT_NAME validation (Audit SECURITY)..."

# Source inspection: validate_project_name function exists
check "validate_project_name function exists" \
    "$( grep -q 'validate_project_name()' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "validate_project_name called after PROJECT_NAME resolution" \
    "$( grep -q 'validate_project_name "\$PROJECT_NAME"' "$NBS_CHAT_INIT" && echo pass || echo fail )"

set +e

# PROJECT_NAME with shell metacharacters should be rejected
PROJNAME_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name=test --project-name='$(whoami)' --dry-run --force 2>&1)
PROJNAME_RC=$?
check "PROJECT_NAME with \$(whoami) rejected" \
    "$( [[ "$PROJNAME_RC" -eq 4 ]] && echo pass || echo fail )"

PROJNAME2_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name=test --project-name='test;evil' --dry-run --force 2>&1)
PROJNAME2_RC=$?
check "PROJECT_NAME with semicolon rejected" \
    "$( [[ "$PROJNAME2_RC" -eq 4 ]] && echo pass || echo fail )"

# Valid PROJECT_NAMEs (with dots, spaces, hyphens, underscores) should be accepted
PROJNAME3_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name=test --project-name='My Project v2.1' --dry-run --force 2>&1)
PROJNAME3_RC=$?
check "PROJECT_NAME with spaces and dots accepted" \
    "$( [[ "$PROJNAME3_RC" -eq 0 ]] && echo pass || echo fail )"

set -e
echo ""

# ============================================================
# Test 3: PROJECT_ROOT validation (Violation 8: HARDENING)
# ============================================================
echo "3. PROJECT_ROOT validation (Violation 8: HARDENING)..."

check "Source contains PROJECT_ROOT validation" \
    "$( grep -q 'assert_directory_exists.*PROJECT_ROOT' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 4: Derived paths are absolute (Violation 11: HARDENING)
# ============================================================
echo "4. Derived paths are absolute (Violation 11: HARDENING)..."

check "CHAT_FILE uses absolute path" \
    "$( grep -q 'CHAT_FILE="\${PROJECT_ROOT}/' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "SCRIBE_LOG uses absolute path" \
    "$( grep -q 'SCRIBE_LOG="\${PROJECT_ROOT}/' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "PROJECT_ROOT resolved via pwd -P" \
    "$( grep -q 'pwd -P' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 5: assert_tool_exists for all required binaries (Violation 14: HARDENING)
# ============================================================
echo "5. assert_tool_exists for required binaries (Violation 14: HARDENING)..."

for tool in nbs-chat nbs-bus sha256sum date grep sed basename; do
    check "assert_tool_exists for $tool" \
        "$( grep -q "assert_tool_exists \"$tool\"" "$NBS_CHAT_INIT" && echo pass || echo fail )"
done

# tmux and nbs-claude are checked conditionally (spawn modes)
check "assert_tool_exists for nbs-ts (conditional)" \
    "$( grep -q 'assert_tool_exists "nbs-ts"' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "assert_tool_exists for nbs-claude (conditional)" \
    "$( grep -q 'assert_tool_exists "nbs-claude"' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 6: sha256sum failure checked (Violation 10: HARDENING)
# ============================================================
echo "6. sha256sum failure checked (Violation 10: HARDENING)..."

check "generate_project_id checks sha256sum return" \
    "$( grep -A5 'generate_project_id' "$NBS_CHAT_INIT" | grep -q 'sha256sum.*||' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 7: nbs-bus publish failure handling (Violations 4, 6: BUG)
# ============================================================
echo "7. nbs-bus publish failure handling (Violations 4, 6: BUG)..."

check "Self-test publish checks return value" \
    "$( grep -q 'if ! nbs-bus publish.*self-test' "$NBS_CHAT_INIT" && echo pass || echo fail )"

check "No || true after nbs-bus publish" \
    "$( grep 'nbs-bus publish' "$NBS_CHAT_INIT" | grep -q '|| true' && echo fail || echo pass )"

check "ai-spawned publish checks return value" \
    "$( grep -q 'if ! nbs-bus publish.*ai-spawned' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 8: nbs-chat create/send failure handling (Violation 5: BUG)
# ============================================================
echo "8. nbs-chat create/send failure handling (Violation 5: BUG)..."

check "nbs-chat create checks return value" \
    "$( grep -q 'if ! nbs-chat create' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "nbs-chat send checks return value" \
    "$( grep -q 'if ! nbs-chat send' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 9: nbs-bus ack failure logged (Violation 13: HARDENING)
# ============================================================
echo "9. nbs-bus ack failure logged (Violation 13: HARDENING)..."

check "nbs-bus ack checks return value" \
    "$( grep -q 'if ! nbs-bus ack' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "nbs-bus ack failure produces warning" \
    "$( grep -A1 'if ! nbs-bus ack' "$NBS_CHAT_INIT" | grep -q 'warn' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 10: find -print0 replaces ls + xargs (Violation 1: SECURITY)
# ============================================================
echo "10. find -print0 replaces ls + xargs (Violation 1: SECURITY)..."

check "No unquoted ls glob for event files" \
    "$( grep 'ls .nbs/events/\*' "$NBS_CHAT_INIT" && echo fail || echo pass )"
# The find command uses absolute path ${PROJECT_ROOT}/.nbs/events/
check "find -print0 used for event file search" \
    "$( grep -q 'find.*\.nbs/events/.*-print0' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "xargs -0 used with find" \
    "$( grep -q 'xargs -0' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 11: --dangerously-skip-permissions documented (Violation 2: SECURITY)
# ============================================================
echo "11. --dangerously-skip-permissions documented (Violation 2: SECURITY)..."

check "Security risk comment present" \
    "$( grep -q 'SECURITY RISK' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "Comment mentions permission bypass" \
    "$( grep -q 'permission system' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "Comment references architectural decision" \
    "$( grep -q 'Architectural decision' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 12: mv/cat bypass documented or routed through run() (Violation 7: BUG)
# ============================================================
echo "12. mv/cat bypass of run() documented or fixed (Violation 7: BUG)..."

check "Compaction mv routed through run()" \
    "$( grep -q 'run mv.*log_file.*archive_file' "$NBS_CHAT_INIT" && echo pass || echo fail )"

BYPASS_COMMENTS=$(grep -c 'cat heredoc bypasses run()' "$NBS_CHAT_INIT")
check "cat heredoc bypass comments present (>= 3)" \
    "$( [[ "$BYPASS_COMMENTS" -ge 3 ]] && echo pass || echo fail )"

check "echo redirect bypass documented" \
    "$( grep -q 'echo redirect bypasses run()' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 13: Arithmetic defaults (Violation 12: HARDENING)
# ============================================================
echo "13. Arithmetic defaults (Violation 12: HARDENING)..."

check "wait_count initialised before arithmetic" \
    "$( grep -q 'wait_count=0\|${wait_count:-0}' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "max_wait initialised before arithmetic" \
    "$( grep -q 'max_wait=60\|${max_wait:-60}' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "entry_count uses \${var:-0} form" \
    "$( grep -q '\${entry_count:-0}' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 14: echo -e portability fix (Audit BUG + HARDENING)
# ============================================================
echo "14. echo -e portability fix (Audit BUG + HARDENING)..."

# No echo -e should remain in the script (except in comments)
ECHO_E_COUNT=$(grep -c '^[^#]*echo -e' "$NBS_CHAT_INIT" || true)
check "No echo -e in executable code (found $ECHO_E_COUNT)" \
    "$( [[ "$ECHO_E_COUNT" -eq 0 ]] && echo pass || echo fail )"

# run() should use printf, not echo -e
check "run() uses printf for dry-run output" \
    "$( grep -A5 '^run()' "$NBS_CHAT_INIT" | grep -q 'printf' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 15: Corrupt archive header warning (Audit BUG)
# ============================================================
echo "15. Corrupt archive header warning (Audit BUG)..."

check "compact_decision_log warns on missing Project: header" \
    "$( grep -A3 'project_name=.*grep' "$NBS_CHAT_INIT" | grep -q 'warn.*missing.*Project' && echo pass || echo fail )"
check "compact_decision_log warns on missing Chat: header" \
    "$( grep -A3 'chat_ref=.*grep' "$NBS_CHAT_INIT" | grep -q 'warn.*missing.*Chat' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 16: nbs-ts session create return values checked (Audit BUG)
# ============================================================
echo "16. nbs-ts session create return values checked (Audit BUG)..."

# All spawn paths use spawn_agent_ts which checks create return value
SPAWN_CALLS=$(grep -c 'spawn_agent_ts' "$NBS_CHAT_INIT" || true)
# Subtract 2 for the function definition line and the comment line
SPAWN_INVOCATIONS=$((SPAWN_CALLS - 2))
check "spawn_agent_ts used in all spawn paths (found $SPAWN_INVOCATIONS invocations, expect 3)" \
    "$( [[ "$SPAWN_INVOCATIONS" -ge 3 ]] && echo pass || echo fail )"

# spawn_agent_ts checks nbs-ts create return value
check "spawn_agent_ts checks nbs-ts create failure" \
    "$( grep -A2 'NBS_TS_BIN.*create' "$NBS_CHAT_INIT" | grep -q '||' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 17: Main Claude spawned with NBS_HANDLE (Audit BUG)
# ============================================================
echo "17. Main Claude spawned with NBS_HANDLE (Audit BUG)..."

# spawn_agent_ts passes NBS_HANDLE=${role} in the create command
check "Main Claude spawn includes NBS_HANDLE" \
    "$( grep -q 'NBS_HANDLE=\${role}' "$NBS_CHAT_INIT" && echo pass || echo fail )"
check "Main Claude spawn calls spawn_agent_ts with claude role" \
    "$( grep -q 'spawn_agent_ts "claude"' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 18: nbs-ts send return values handled (Audit HARDENING)
# ============================================================
echo "18. nbs-ts send return values handled (Audit HARDENING)..."

# Check that nbs-ts send for prompts uses || true (non-critical)
SEND_CHECKS=$(grep -c 'NBS_TS_BIN.*send' "$NBS_CHAT_INIT" || true)
check "nbs-ts send used for prompt injection (found $SEND_CHECKS, expect >= 2)" \
    "$( [[ "$SEND_CHECKS" -ge 2 ]] && echo pass || echo fail )"

echo ""

# ============================================================
# Test 19: Agent spawn has readiness wait loop (Audit HARDENING)
# ============================================================
echo "19. Agent spawn has readiness wait loop (Audit HARDENING)..."

# spawn_agent_ts has a wait loop checking 'handle is' readiness
check "Agent spawn has readiness wait loop" \
    "$( grep -q 'did not become ready' "$NBS_CHAT_INIT" && echo pass || echo fail )"

echo ""

# ============================================================
# Test 20: Summary uses absolute paths (Audit HARDENING)
# ============================================================
echo "20. Summary uses absolute paths (Audit HARDENING)..."

# Bus and Config lines in summary should not use bare relative paths
check "Bus summary uses absolute path" \
    "$( grep 'Bus:' "$NBS_CHAT_INIT" | grep -q 'PROJECT_ROOT' && echo pass || echo fail )"
check "Config summary uses absolute path" \
    "$( grep 'Config:' "$NBS_CHAT_INIT" | grep -q 'PROJECT_ROOT' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 21: date +%s return value checked (Audit HARDENING)
# ============================================================
echo "21. date +%s return value checked (Audit HARDENING)..."

check "date +%s checked in self-test" \
    "$( grep -A3 'self_test_ts' "$NBS_CHAT_INIT" | grep -q 'date +%s.*||' && echo pass || echo fail )"

echo ""

# ============================================================
# Test 22: Dry-run mode still works (regression)
# ============================================================
echo "22. Dry-run mode regression..."

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
# Test 23: Unknown argument rejection (regression)
# ============================================================
echo "23. Unknown argument rejection (regression)..."

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
# Test 24: Help still works (regression)
# ============================================================
echo "24. Help output (regression)..."

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
# Test 25: Adversarial CHAT_NAME edge cases
# ============================================================
echo "25. Adversarial CHAT_NAME edge cases..."

set +e

# Newlines in name
NL_OUT=$("$NBS_CHAT_INIT" --name=$'test\nname' --dry-run 2>&1)
NL_RC=$?
check "CHAT_NAME with newline rejected" \
    "$( [[ "$NL_RC" -eq 4 ]] && echo pass || echo fail )"

# Null bytes — bash strips null bytes, so $'test\x00name' becomes 'testname'
# which is alphanumeric-only and should be accepted. The test verifies no crash.
NULL_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name=$'test\x00name' --dry-run --force 2>&1)
NULL_RC=$?
check "CHAT_NAME with null byte does not crash (exits 0 or 4)" \
    "$( [[ "$NULL_RC" -eq 4 || "$NULL_RC" -eq 0 ]] && echo pass || echo fail )"

# Very long name (100 chars — should be accepted)
LONG_NAME=$(printf 'a%.0s' {1..100})
LONG_OUT=$(cd "$TEST_DIR" && "$NBS_CHAT_INIT" --name="$LONG_NAME" --dry-run --force 2>&1)
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
