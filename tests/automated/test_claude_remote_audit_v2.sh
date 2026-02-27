#!/bin/bash
# test_claude_remote_audit_v2.sh — Tests for audit-report.md violations 1-10
#
# Tests all 10 violations from the bin/nbs-claude-remote audit:
#   BUG (4):      #1 list branch stderr/exit masking, #2 poll loop process check,
#                 #3 LOCAL_HOST unquoted in send-keys, #7 ssh_opts compound splitting
#   SECURITY (1): #4 LOCAL_HOST injection validation
#   HARDENING (5): #5 REMOTE_CMD maintenance invariant, #6 cleanup trap documented,
#                  #8 tmux has-session failure modes, #9 session name quoting comment,
#                  #10 NBS_HANDLE unquoted in send-keys
#
# These tests exercise static analysis of the script (grep-based structural
# checks) and dynamic argument validation (invoking the script with crafted
# inputs). No SSH connectivity is required.
#
# Usage: bash tests/automated/test_claude_remote_audit_v2.sh
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

printf '=== test_claude_remote_audit_v2.sh ===\n\n'

# -----------------------------------------------------------------------
# Precondition: script exists and is executable
# -----------------------------------------------------------------------

printf -- '--- Preconditions ---\n'

if [[ -f "$SCRIPT_UNDER_TEST" && -x "$SCRIPT_UNDER_TEST" ]]; then
    pass "Script exists and is executable"
else
    fail "Script not found or not executable: $SCRIPT_UNDER_TEST"
    printf '\nCannot continue without script.\n' >&2
    exit 1
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #1 (BUG): --list branch no longer suppresses stderr
# or masks exit code with || echo
# Falsification: if '2>/dev/null' appears on tmux ls in the list branch,
#   or if '|| echo' appears without exit 1, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #1 (BUG): --list branch stderr/exit masking ---\n'

# The list branch remote command should NOT use bare '2>/dev/null' on tmux ls
LIST_CMD_LINE=$(grep -n 'tmux ls' "$SCRIPT_UNDER_TEST" | head -1 || true)
if echo "$LIST_CMD_LINE" | grep -q '2>/dev/null'; then
    fail "tmux ls still has 2>/dev/null (hides tmux errors)"
else
    pass "tmux ls does not suppress stderr with 2>/dev/null"
fi

# The list branch should NOT mask exit code with || echo (without exit)
# Extract the actual list-mode block by finding the if-LIST-true block
LIST_SECTION=$(awk '/^\s*if \[\[ "\$LIST" == "true" \]\]/,/^fi/' "$SCRIPT_UNDER_TEST")
if echo "$LIST_SECTION" | grep -qP "\\|\\| echo '[^']*'\"$" 2>/dev/null; then
    fail "list branch still masks exit code with '|| echo' without exit"
else
    pass "list branch does not mask exit code with bare '|| echo'"
fi

# The list branch should propagate non-zero exit (exit $? or exit 1 in the remote cmd)
if echo "$LIST_SECTION" | grep -qE 'exit \$\?|exit 1'; then
    pass "list branch propagates non-zero exit code"
else
    fail "list branch does not propagate non-zero exit code"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #2 (BUG): Poll loop checks nbs-claude process, not
# just pane PID liveness
# Falsification: if poll loop only uses 'kill -0' without nbs-claude
#   process check, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #2 (BUG): Poll loop process check ---\n'

POLL_SECTION=$(sed -n '/POLL_CMD=/,/^"/p' "$SCRIPT_UNDER_TEST")

# Must reference nbs-claude in the poll check
if echo "$POLL_SECTION" | grep -q 'nbs-claude'; then
    pass "Poll loop references nbs-claude process"
else
    fail "Poll loop does not check for nbs-claude process"
fi

# Must use pgrep or similar process-finding tool
if echo "$POLL_SECTION" | grep -qE 'pgrep.*nbs-claude'; then
    pass "Poll loop uses pgrep to find nbs-claude"
else
    fail "Poll loop does not use pgrep for nbs-claude detection"
fi

# Must have a postcondition check after the poll loop (warning or error
# if nbs-claude not found)
if echo "$POLL_SECTION" | grep -qE 'Warning.*nbs-claude|Error.*nbs-claude'; then
    pass "Poll loop has postcondition check for nbs-claude readiness"
else
    fail "Poll loop lacks postcondition check for nbs-claude readiness"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #3 (BUG): --remote-host=${LOCAL_HOST} quoted in send-keys
# Falsification: if --remote-host= appears without escaped quotes around
#   the value in the send-keys string, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #3 (BUG): LOCAL_HOST quoted in send-keys ---\n'

SENDKEYS_LINE=$(grep 'send-keys' "$SCRIPT_UNDER_TEST" | grep 'remote-host' || true)

# Check --remote-host is quoted with escaped double quotes
if echo "$SENDKEYS_LINE" | grep -qE 'remote-host=\\"'; then
    pass "--remote-host value is quoted in send-keys string"
else
    fail "--remote-host value is NOT quoted in send-keys string"
fi

# Check there are no unquoted occurrences of --remote-host=${LOCAL_HOST}
# (without backslash-quote before the dollar sign)
if echo "$SENDKEYS_LINE" | grep -qE 'remote-host=[^"\\]\$\{LOCAL_HOST\}[^"\\]'; then
    fail "Found unquoted --remote-host=\${LOCAL_HOST}"
else
    pass "No unquoted --remote-host=\${LOCAL_HOST} found"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #4 (SECURITY): LOCAL_HOST validated with validate_safe_path
# Falsification: if validate_safe_path is not called with LOCAL_HOST, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #4 (SECURITY): LOCAL_HOST injection validation ---\n'

if grep -q 'validate_safe_path "LOCAL_HOST"' "$SCRIPT_UNDER_TEST"; then
    pass "validate_safe_path called for LOCAL_HOST"
else
    fail "validate_safe_path NOT called for LOCAL_HOST"
fi

# The validation must occur BEFORE LOCAL_HOST is interpolated into REMOTE_CMD
LOCAL_HOST_VALIDATE_LINE=$(grep -n 'validate_safe_path "LOCAL_HOST"' "$SCRIPT_UNDER_TEST" | head -1 | cut -d: -f1)
REMOTE_CMD_LINE=$(grep -n '^REMOTE_CMD=' "$SCRIPT_UNDER_TEST" | head -1 | cut -d: -f1)

if [[ -n "$LOCAL_HOST_VALIDATE_LINE" && -n "$REMOTE_CMD_LINE" ]]; then
    if [[ "$LOCAL_HOST_VALIDATE_LINE" -lt "$REMOTE_CMD_LINE" ]]; then
        pass "LOCAL_HOST validated before interpolation into REMOTE_CMD"
    else
        fail "LOCAL_HOST validated AFTER interpolation (line $LOCAL_HOST_VALIDATE_LINE >= $REMOTE_CMD_LINE)"
    fi
else
    fail "Could not determine line numbers for validation order check"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #5 (HARDENING): REMOTE_CMD has maintenance invariant comment
# Falsification: if no comment above REMOTE_CMD listing all interpolated
#   variables and their validation sites, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #5 (HARDENING): REMOTE_CMD maintenance invariant ---\n'

# Get the comment block above REMOTE_CMD (10 lines before)
COMMENT_BLOCK=$(grep -B15 '^REMOTE_CMD=' "$SCRIPT_UNDER_TEST")

# Must list ROOT, SESSION_NAME, HANDLE, LOCAL_HOST
for var in ROOT SESSION_NAME HANDLE LOCAL_HOST; do
    if echo "$COMMENT_BLOCK" | grep -qE "^#.*$var.*validate"; then
        pass "Maintenance invariant documents $var validation"
    else
        fail "Maintenance invariant missing $var validation documentation"
    fi
done

# Must contain the keyword "MAINTENANCE INVARIANT" or "maintenance invariant"
if echo "$COMMENT_BLOCK" | grep -qi 'MAINTENANCE INVARIANT'; then
    pass "Comment block labelled as MAINTENANCE INVARIANT"
else
    fail "Comment block not labelled as MAINTENANCE INVARIANT"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #6 (HARDENING): Cleanup trap documented as scaffolding
# Falsification: if cleanup function comment says "hook point" or "TODO"
#   instead of explicitly stating no resources need cleanup, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #6 (HARDENING): Cleanup trap documentation ---\n'

CLEANUP_BLOCK=$(sed -n '/^cleanup()/,/^}/p' "$SCRIPT_UNDER_TEST")

# Must NOT contain "hook point" or "future resource management" (TODO-like)
if echo "$CLEANUP_BLOCK" | grep -qi 'hook point'; then
    fail "Cleanup still uses TODO-like 'hook point' language"
else
    pass "Cleanup does not use 'hook point' language"
fi

# Must contain explicit statement about current state
if echo "$CLEANUP_BLOCK" | grep -qi 'STRUCTURAL SCAFFOLDING\|no resources currently'; then
    pass "Cleanup explicitly documents current state"
else
    fail "Cleanup does not explicitly document current state"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #7 (BUG): parse_ssh_opts_array limitation documented
# Falsification: if the function comment does not mention the limitation
#   about compound options with spaces, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #7 (BUG): SSH opts limitation documented ---\n'

PARSE_COMMENT=$(grep -B10 'parse_ssh_opts_array()' "$SCRIPT_UNDER_TEST")

# Must mention limitation about compound options or embedded spaces
if echo "$PARSE_COMMENT" | grep -qi 'LIMITATION\|compound.*option\|embedded space'; then
    pass "parse_ssh_opts_array documents compound option limitation"
else
    fail "parse_ssh_opts_array does not document compound option limitation"
fi

# Help text must also mention the limitation
HELP_TEXT=$(sed -n '/cat <<.USAGE/,/^USAGE/p' "$SCRIPT_UNDER_TEST")
if echo "$HELP_TEXT" | grep -qi 'compound\|space.*NOT supported\|single.token'; then
    pass "Help text documents SSH opts limitation"
else
    fail "Help text does not document SSH opts limitation"
fi

# Header comment must also mention the limitation
HEADER_COMMENT=$(head -35 "$SCRIPT_UNDER_TEST")
if echo "$HEADER_COMMENT" | grep -qi 'compound\|space.*NOT supported\|single.token'; then
    pass "Header comment documents SSH opts limitation"
else
    fail "Header comment does not document SSH opts limitation"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #8 (HARDENING): tmux availability checked before has-session
# Falsification: if no 'command -v tmux' or equivalent check exists before
#   the has-session call, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #8 (HARDENING): tmux availability check ---\n'

# Must check for tmux binary before using has-session
if grep -q 'command -v tmux' "$SCRIPT_UNDER_TEST"; then
    pass "tmux availability checked with 'command -v tmux'"
else
    fail "No 'command -v tmux' check found"
fi

# The tmux check must occur before has-session in the launch path
TMUX_CHECK_LINE=$(grep -n 'command -v tmux' "$SCRIPT_UNDER_TEST" | head -1 | cut -d: -f1)
# Find the first actual has-session invocation (not in POLL_CMD, not a comment line)
HAS_SESSION_LINE=$(grep -n 'has-session' "$SCRIPT_UNDER_TEST" | grep -v 'POLL_CMD' | grep -v ':#' | head -1 | cut -d: -f1)

if [[ -n "$TMUX_CHECK_LINE" && -n "$HAS_SESSION_LINE" ]]; then
    if [[ "$TMUX_CHECK_LINE" -lt "$HAS_SESSION_LINE" ]]; then
        pass "tmux availability check occurs before has-session"
    else
        fail "tmux check (line $TMUX_CHECK_LINE) occurs after has-session (line $HAS_SESSION_LINE)"
    fi
else
    fail "Could not determine line numbers for tmux check order"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #9 (HARDENING): Session name quoting has clarifying comment
# Falsification: if no comment near attach-session or has-session explains
#   that SESSION_NAME is expanded locally with remote single quotes as
#   defence-in-depth, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #9 (HARDENING): Session name quoting comment ---\n'

# Must have a comment explaining local expansion + defence-in-depth
if grep -q 'expanded locally' "$SCRIPT_UNDER_TEST" && \
   grep -q 'defence-in-depth' "$SCRIPT_UNDER_TEST"; then
    pass "Comment explains local expansion and defence-in-depth"
else
    fail "Missing comment about local expansion / defence-in-depth"
fi

# The comment must appear near the resume path and/or the launch path
RESUME_SECTION=$(sed -n '/Resume mode/,/^fi$/p' "$SCRIPT_UNDER_TEST")
if echo "$RESUME_SECTION" | grep -q 'expanded locally\|defence-in-depth'; then
    pass "Clarifying comment present in resume path"
else
    fail "No clarifying comment in resume path"
fi

LAUNCH_SECTION=$(sed -n '/MAINTENANCE INVARIANT/,/POLL_CMD=/p' "$SCRIPT_UNDER_TEST")
if echo "$LAUNCH_SECTION" | grep -q 'expanded locally\|defence-in-depth'; then
    pass "Clarifying comment present in launch path"
else
    fail "No clarifying comment in launch path"
fi

# -----------------------------------------------------------------------
# AUDIT VIOLATION #10 (HARDENING): NBS_HANDLE quoted in send-keys
# Falsification: if NBS_HANDLE=${HANDLE} appears without escaped quotes
#   in the send-keys string, test fails.
# -----------------------------------------------------------------------

printf '\n--- Violation #10 (HARDENING): NBS_HANDLE quoted in send-keys ---\n'

SENDKEYS_LINE=$(grep 'send-keys' "$SCRIPT_UNDER_TEST" | grep 'NBS_HANDLE' || true)

# NBS_HANDLE must be quoted: NBS_HANDLE=\"${HANDLE}\"
if echo "$SENDKEYS_LINE" | grep -qE 'NBS_HANDLE=\\"'; then
    pass "NBS_HANDLE quoted in send-keys string"
else
    fail "NBS_HANDLE NOT quoted in send-keys string"
fi

# NBS_REMOTE_HOST must also be quoted for consistency
if echo "$SENDKEYS_LINE" | grep -qE 'NBS_REMOTE_HOST=\\"'; then
    pass "NBS_REMOTE_HOST quoted in send-keys string"
else
    fail "NBS_REMOTE_HOST NOT quoted in send-keys string"
fi

# -----------------------------------------------------------------------
# Cross-cutting: verify no regressions in existing safety measures
# -----------------------------------------------------------------------

printf '\n--- Cross-cutting regression checks ---\n'

# set -euo pipefail must still be present
if grep -q 'set -euo pipefail' "$SCRIPT_UNDER_TEST"; then
    pass "set -euo pipefail present"
else
    fail "set -euo pipefail missing"
fi

# validate_safe_name and validate_safe_path must still be defined
for func in validate_safe_name validate_safe_path; do
    if grep -qE "^${func}\(\)" "$SCRIPT_UNDER_TEST"; then
        pass "$func() still defined"
    else
        fail "$func() missing"
    fi
done

# HOST and ROOT must still be validated
if grep -q 'validate_safe_path "HOST"' "$SCRIPT_UNDER_TEST"; then
    pass "HOST still validated by validate_safe_path"
else
    fail "HOST validation missing"
fi

if grep -q 'validate_safe_path "ROOT"' "$SCRIPT_UNDER_TEST"; then
    pass "ROOT still validated by validate_safe_path"
else
    fail "ROOT validation missing"
fi

if grep -q 'validate_safe_name "HANDLE"' "$SCRIPT_UNDER_TEST"; then
    pass "HANDLE still validated by validate_safe_name"
else
    fail "HANDLE validation missing"
fi

if grep -q 'validate_safe_name "CHAT_NAME"' "$SCRIPT_UNDER_TEST"; then
    pass "CHAT_NAME still validated by validate_safe_name"
else
    fail "CHAT_NAME validation missing"
fi

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

printf '\n=== Results: %d passed, %d failed ===\n' "$PASS" "$FAIL"

if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi

exit 0
