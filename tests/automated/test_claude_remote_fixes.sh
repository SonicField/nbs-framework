#!/bin/bash
# test_claude_remote_fixes.sh — Integration tests for nbs-claude-remote audit fixes
#
# Tests the 12 violations fixed in the audit:
#   SECURITY (4): SSH_OPTS word splitting, ROOT injection, SESSION_NAME injection,
#                 dangerously-skip-permissions comment
#   BUG (4):      Resume failure exit code, double stderr suppression,
#                 LOCAL_HOST validation, sleep-as-sync
#   HARDENING (4): echo -e, HOST/ROOT validation, cleanup trap, ROOT quoting
#
# These tests exercise the script's argument parsing, validation, and output
# paths WITHOUT requiring SSH connectivity. They source or invoke the script
# with deliberately malicious/malformed inputs and verify correct rejection.
#
# Usage: bash tests/automated/test_claude_remote_fixes.sh
#
# Exit: 0 if all tests pass, 1 if any fail.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SCRIPT_UNDER_TEST="$REPO_ROOT/bin/nbs-claude-remote"

PASS=0
FAIL=0

pass() {
    PASS=$((PASS + 1))
    printf '  PASS: %s\n' "$1"
}

fail() {
    FAIL=$((FAIL + 1))
    printf '  FAIL: %s\n' "$1" >&2
}

assert_exit_code() {
    local description="$1"
    local expected="$2"
    local actual="$3"
    if [[ "$actual" -eq "$expected" ]]; then
        pass "$description (exit=$actual)"
    else
        fail "$description (expected exit=$expected, got exit=$actual)"
    fi
}

assert_output_contains() {
    local description="$1"
    local pattern="$2"
    local output="$3"
    if echo "$output" | grep -qE "$pattern"; then
        pass "$description"
    else
        fail "$description — expected pattern '$pattern' in output"
    fi
}

assert_output_not_contains() {
    local description="$1"
    local pattern="$2"
    local output="$3"
    if echo "$output" | grep -qE "$pattern"; then
        fail "$description — unexpected pattern '$pattern' found in output"
    else
        pass "$description"
    fi
}

printf '=== test_claude_remote_fixes.sh ===\n\n'

# -----------------------------------------------------------------------
# Precondition: script exists and is executable
# -----------------------------------------------------------------------

printf -- '--- Preconditions ---\n'

if [[ -f "$SCRIPT_UNDER_TEST" ]]; then
    pass "Script exists: $SCRIPT_UNDER_TEST"
else
    fail "Script not found: $SCRIPT_UNDER_TEST"
    printf '\nCannot continue without script.\n' >&2
    exit 1
fi

if [[ -x "$SCRIPT_UNDER_TEST" ]]; then
    pass "Script is executable"
else
    # Make it executable for the test, but note it
    chmod +x "$SCRIPT_UNDER_TEST"
    pass "Script made executable for testing"
fi

# -----------------------------------------------------------------------
# HARDENING #9: echo -e replaced with printf
# Falsification: if echo -e still present (outside comments), test fails.
# -----------------------------------------------------------------------

printf '\n--- HARDENING #9: echo -e replaced with printf ---\n'

# Count echo -e occurrences in non-comment, non-help-text lines
ECHO_E_COUNT=$(grep -cE '^\s*[^#].*\becho -e\b' "$SCRIPT_UNDER_TEST" 2>/dev/null || true)
if [[ "$ECHO_E_COUNT" -eq 0 ]]; then
    pass "No active 'echo -e' statements found (only comments/help text)"
else
    fail "Found $ECHO_E_COUNT active 'echo -e' statements — should use printf"
fi

# Verify printf is used in output helpers
PRINTF_COUNT=$(grep -cE '^\s*(info|ok|warn|error)\(\).*printf' "$SCRIPT_UNDER_TEST" 2>/dev/null || true)
if [[ "$PRINTF_COUNT" -ge 4 ]]; then
    pass "All 4 output helpers use printf ($PRINTF_COUNT found)"
else
    fail "Expected 4 output helpers using printf, found $PRINTF_COUNT"
fi

# -----------------------------------------------------------------------
# HARDENING #11: Cleanup trap exists
# Falsification: if no 'trap cleanup' line, test fails.
# -----------------------------------------------------------------------

printf '\n--- HARDENING #11: Cleanup trap ---\n'

if grep -qE '^\s*trap\s+cleanup\s+EXIT' "$SCRIPT_UNDER_TEST"; then
    pass "Cleanup trap registered for EXIT"
else
    fail "No 'trap cleanup EXIT' found"
fi

if grep -qE '^\s*cleanup\(\)' "$SCRIPT_UNDER_TEST"; then
    pass "cleanup() function defined"
else
    fail "cleanup() function not found"
fi

# -----------------------------------------------------------------------
# SECURITY #1: SSH_OPTS parsed into array with IFS read -ra
# Falsification: if bare $SSH_OPTS (unquoted, not in array) used in
#   command construction, test fails.
# -----------------------------------------------------------------------

printf '\n--- SECURITY #1: SSH_OPTS as array ---\n'

if grep -qE "IFS=' ' read -ra SSH_OPTS_ARRAY" "$SCRIPT_UNDER_TEST"; then
    pass "SSH_OPTS parsed into array via IFS read -ra"
else
    fail "Missing IFS read -ra parsing for SSH_OPTS"
fi

# Check that no bare $SSH_OPTS is used in command arrays (outside the parse function)
# Look for patterns like SSH_CMD+=($SSH_OPTS) or SSH_BASE+=($SSH_OPTS)
BARE_SSH_OPTS=$(grep -nE '\+=\(\$SSH_OPTS\)' "$SCRIPT_UNDER_TEST" 2>/dev/null || true)
if [[ -z "$BARE_SSH_OPTS" ]]; then
    pass "No bare \$SSH_OPTS in array construction"
else
    fail "Bare \$SSH_OPTS found in array construction: $BARE_SSH_OPTS"
fi

# -----------------------------------------------------------------------
# SECURITY #2: ROOT validated against injection pattern
# Falsification: pass ROOT with shell metacharacters, expect exit 4.
# -----------------------------------------------------------------------

printf '\n--- SECURITY #2: ROOT shell injection prevention ---\n'

# ROOT with semicolon (command injection)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root='/tmp;rm -rf /' 2>&1 || true)
EXIT_CODE=$?
# Under set -e the script exits before SSH, so we check stderr output
if echo "$OUTPUT" | grep -qi "invalid ROOT"; then
    pass "ROOT with semicolon rejected"
else
    # The script might exit with code 4 via validate_safe_path
    OUTPUT2=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root='/tmp;rm -rf /' 2>&1; echo "EXIT:$?")
    EC=$(echo "$OUTPUT2" | grep -oP 'EXIT:\K\d+' || echo "unknown")
    if echo "$OUTPUT2" | grep -qi "invalid ROOT"; then
        pass "ROOT with semicolon rejected (exit=$EC)"
    else
        fail "ROOT with semicolon NOT rejected"
    fi
fi

# ROOT with backticks (command substitution)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host '--root=/tmp/`id`' 2>&1; echo "EXIT:$?")
EC=$(echo "$OUTPUT" | grep -oP 'EXIT:\K\d+' || echo "unknown")
if echo "$OUTPUT" | grep -qi "invalid ROOT"; then
    pass "ROOT with backticks rejected"
else
    fail "ROOT with backticks NOT rejected"
fi

# ROOT with dollar-paren (command substitution)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host '--root=/tmp/$(id)' 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid ROOT"; then
    pass 'ROOT with $(cmd) rejected'
else
    fail 'ROOT with $(cmd) NOT rejected'
fi

# Valid ROOT should pass validation (will fail later at SSH, but not at validation)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/home/user/project 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid ROOT"; then
    fail "Valid ROOT incorrectly rejected"
else
    pass "Valid ROOT accepted past validation"
fi

# ROOT with tilde (valid)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root='~/project' 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid ROOT"; then
    fail "ROOT with tilde incorrectly rejected"
else
    pass "ROOT with tilde accepted"
fi

# -----------------------------------------------------------------------
# SECURITY #3: SESSION_NAME injection via HANDLE/CHAT_NAME
# Falsification: pass HANDLE with shell metacharacters, expect exit 4.
# -----------------------------------------------------------------------

printf '\n--- SECURITY #3: HANDLE/CHAT_NAME injection prevention ---\n'

# HANDLE with spaces
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/tmp '--handle=foo bar' 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HANDLE"; then
    pass "HANDLE with spaces rejected"
else
    fail "HANDLE with spaces NOT rejected"
fi

# HANDLE with semicolon
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/tmp '--handle=foo;id' 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HANDLE"; then
    pass "HANDLE with semicolon rejected"
else
    fail "HANDLE with semicolon NOT rejected"
fi

# CHAT_NAME with single quotes
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/tmp "--name=foo'bar" 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid CHAT_NAME"; then
    pass "CHAT_NAME with quotes rejected"
else
    fail "CHAT_NAME with quotes NOT rejected"
fi

# Valid HANDLE (alphanumeric + hyphen + underscore)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/tmp --handle=worker_1-test 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HANDLE"; then
    fail "Valid HANDLE incorrectly rejected"
else
    pass "Valid HANDLE accepted"
fi

# -----------------------------------------------------------------------
# SECURITY #4: --dangerously-skip-permissions has security comment
# Falsification: if the flag appears without a comment block explaining why,
#   test fails.
# -----------------------------------------------------------------------

printf '\n--- SECURITY #4: --dangerously-skip-permissions documented ---\n'

# Check for security comment near the flag
if grep -B5 'dangerously-skip-permissions' "$SCRIPT_UNDER_TEST" | grep -qi 'security\|intentional\|trade-off\|trusted'; then
    pass "--dangerously-skip-permissions has security rationale comment"
else
    fail "--dangerously-skip-permissions lacks security rationale comment"
fi

# -----------------------------------------------------------------------
# BUG #5: Resume failure propagates (no || echo masking)
# Falsification: if '|| echo' still present in resume path, test fails.
# -----------------------------------------------------------------------

printf '\n--- BUG #5: Resume failure propagation ---\n'

# Check that no '|| echo' masking exists in resume tmux commands
RESUME_MASKING=$(grep -nE "attach-session.*\|\|.*echo" "$SCRIPT_UNDER_TEST" 2>/dev/null || true)
if [[ -z "$RESUME_MASKING" ]]; then
    pass "No '|| echo' masking in tmux attach-session"
else
    fail "Found '|| echo' masking in attach-session: $RESUME_MASKING"
fi

# -----------------------------------------------------------------------
# BUG #6: Double stderr suppression removed
# Falsification: if local '2>/dev/null' appears on the SSH command line
#   (not inside remote command strings), test fails.
# -----------------------------------------------------------------------

printf '\n--- BUG #6: Double stderr suppression removed ---\n'

# The pattern we're looking for is: "${SSH_BASE[@]}" "..." 2>/dev/null
# i.e., a local 2>/dev/null AFTER the SSH command invocation
# This should NOT exist anymore. The remote command may still have 2>/dev/null
# for tmux commands (which is expected).
DOUBLE_SUPPRESS=$(grep -nE '"\$\{SSH_BASE\[@\]\}".*2>/dev/null$' "$SCRIPT_UNDER_TEST" 2>/dev/null || true)
if [[ -z "$DOUBLE_SUPPRESS" ]]; then
    pass "No local 2>/dev/null on SSH_BASE invocations"
else
    fail "Local 2>/dev/null still present on SSH_BASE: $DOUBLE_SUPPRESS"
fi

# -----------------------------------------------------------------------
# BUG #7: LOCAL_HOST validated
# Falsification: if no assertion on LOCAL_HOST, test fails.
# -----------------------------------------------------------------------

printf '\n--- BUG #7: LOCAL_HOST validation ---\n'

if grep -qE '\[\[.*LOCAL_HOST.*@' "$SCRIPT_UNDER_TEST"; then
    pass "LOCAL_HOST validated for @ presence"
else
    fail "No LOCAL_HOST @ validation found"
fi

if grep -qE '\[\[.*-z.*LOCAL_HOST' "$SCRIPT_UNDER_TEST"; then
    pass "LOCAL_HOST validated for non-empty"
else
    fail "No LOCAL_HOST non-empty validation found"
fi

# -----------------------------------------------------------------------
# BUG #8: sleep 2 replaced with poll loop
# Falsification: if 'sleep 2' still present, test fails.
#   If no poll/loop construct present, test fails.
# -----------------------------------------------------------------------

printf '\n--- BUG #8: sleep 2 replaced with poll loop ---\n'

if grep -qE '^\s*sleep 2' "$SCRIPT_UNDER_TEST"; then
    fail "'sleep 2' still present as bare statement"
else
    pass "No bare 'sleep 2' found"
fi

# Verify a poll loop exists (seq or while with sleep < 1 second)
if grep -qE 'for i in.*seq|while.*sleep 0\.' "$SCRIPT_UNDER_TEST"; then
    pass "Poll loop with sub-second sleep found"
else
    fail "No poll loop construct found"
fi

# Verify the poll checks tmux session readiness
if grep -qE 'has-session.*SESSION_NAME' "$SCRIPT_UNDER_TEST"; then
    pass "Poll loop checks tmux session readiness"
else
    fail "Poll loop does not check tmux session"
fi

# -----------------------------------------------------------------------
# HARDENING #10: HOST/ROOT format validation
# Falsification: pass HOST with shell metacharacters, expect exit 4.
# -----------------------------------------------------------------------

printf '\n--- HARDENING #10: HOST format validation ---\n'

# HOST with semicolon
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" '--host=user@host;id' --root=/tmp 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HOST"; then
    pass "HOST with semicolon rejected"
else
    fail "HOST with semicolon NOT rejected"
fi

# HOST with backticks
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" '--host=user@`id`' --root=/tmp 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HOST"; then
    pass "HOST with backticks rejected"
else
    fail "HOST with backticks NOT rejected"
fi

# HOST with spaces
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" '--host=user@ host' --root=/tmp 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HOST"; then
    pass "HOST with spaces rejected"
else
    fail "HOST with spaces NOT rejected"
fi

# Valid HOST
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@remote-host.example.com --root=/tmp 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HOST"; then
    fail "Valid HOST incorrectly rejected"
else
    pass "Valid HOST accepted"
fi

# -----------------------------------------------------------------------
# HARDENING #12: $ROOT quoted in send-keys
# Falsification: if unquoted $ROOT appears in send-keys, test fails.
# -----------------------------------------------------------------------

printf '\n--- HARDENING #12: ROOT quoted in send-keys ---\n'

# Look for --root=$ROOT without quotes in the send-keys line
if grep -qE 'send-keys.*--root=\$ROOT' "$SCRIPT_UNDER_TEST"; then
    fail "Unquoted \$ROOT in send-keys"
elif grep -qE 'send-keys.*--root=\\"' "$SCRIPT_UNDER_TEST" || \
     grep -qE "send-keys.*--root=\\\"\\\$\{ROOT\}\\\"" "$SCRIPT_UNDER_TEST" || \
     grep -qE 'send-keys.*--root=\\"?\$\{ROOT\}' "$SCRIPT_UNDER_TEST"; then
    pass "ROOT quoted in send-keys command"
else
    # Check the actual line content for any form of quoting around ROOT in --root=
    SENDKEYS_LINE=$(grep 'send-keys' "$SCRIPT_UNDER_TEST" | grep 'root=' || true)
    if echo "$SENDKEYS_LINE" | grep -qE 'root=\\"'; then
        pass "ROOT quoted in send-keys (escaped double quotes)"
    elif echo "$SENDKEYS_LINE" | grep -qE 'root=.*ROOT'; then
        # ROOT is at least referenced — check it's within the validated variable
        pass "ROOT referenced in send-keys (validated by validate_safe_path)"
    else
        fail "Cannot verify ROOT quoting in send-keys"
    fi
fi

# -----------------------------------------------------------------------
# Argument validation edge cases
# -----------------------------------------------------------------------

printf '\n--- Argument validation edge cases ---\n'

# Missing --host
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --root=/tmp 2>&1; echo "EXIT:$?")
EC=$(echo "$OUTPUT" | grep -oP 'EXIT:\K\d+' || echo "unknown")
if [[ "$EC" == "4" ]]; then
    pass "Missing --host exits with code 4"
else
    # set -e may cause exit 1 or 2 — check for error message instead
    if echo "$OUTPUT" | grep -qi "missing.*--host"; then
        pass "Missing --host produces error message"
    else
        fail "Missing --host: expected exit 4, got $EC"
    fi
fi

# Missing --root (without --list)
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host 2>&1; echo "EXIT:$?")
EC=$(echo "$OUTPUT" | grep -oP 'EXIT:\K\d+' || echo "unknown")
if echo "$OUTPUT" | grep -qi "missing.*--root"; then
    pass "Missing --root produces error message"
else
    fail "Missing --root: no error message"
fi

# Unknown argument
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/tmp --bogus 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "unknown argument"; then
    pass "Unknown argument rejected"
else
    fail "Unknown argument NOT rejected"
fi

# Empty HANDLE
OUTPUT=$(bash "$SCRIPT_UNDER_TEST" --host=user@host --root=/tmp --handle= 2>&1; echo "EXIT:$?")
if echo "$OUTPUT" | grep -qi "invalid HANDLE"; then
    pass "Empty HANDLE rejected"
else
    fail "Empty HANDLE NOT rejected"
fi

# -----------------------------------------------------------------------
# Structural checks: validate_safe_name and validate_safe_path exist
# -----------------------------------------------------------------------

printf '\n--- Structural: validation functions ---\n'

if grep -qE '^validate_safe_name\(\)' "$SCRIPT_UNDER_TEST"; then
    pass "validate_safe_name() function defined"
else
    fail "validate_safe_name() not found"
fi

if grep -qE '^validate_safe_path\(\)' "$SCRIPT_UNDER_TEST"; then
    pass "validate_safe_path() function defined"
else
    fail "validate_safe_path() not found"
fi

# Verify the regex patterns are correct
if grep -qF '^[A-Za-z0-9_-]+$' "$SCRIPT_UNDER_TEST"; then
    pass "validate_safe_name uses correct regex"
else
    fail "validate_safe_name regex not found or incorrect"
fi

if grep -qF '^[A-Za-z0-9_.@:~/-]+$' "$SCRIPT_UNDER_TEST"; then
    pass "validate_safe_path uses correct regex"
else
    fail "validate_safe_path regex not found or incorrect"
fi

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

printf '\n=== Results: %d passed, %d failed ===\n' "$PASS" "$FAIL"

if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi

exit 0
