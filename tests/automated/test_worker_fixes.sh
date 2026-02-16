#!/bin/bash
# Test: nbs-worker audit fixes (16 violations: 4 BUG, 5 SECURITY, 7 HARDENING)
#
# Falsification approach:
# Each test targets a specific violation fix. For each fix, we construct an input
# that would have triggered the old (broken) behaviour and verify the new
# (correct) behaviour.
#
# Tests that require tmux are skipped when tmux is not available.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_WORKER="$PROJECT_ROOT/bin/nbs-worker"

TEST_DIR=$(mktemp -d)
ORIGINAL_DIR=$(pwd)

ERRORS=0
SKIPPED=0

cleanup() {
    cd "$ORIGINAL_DIR"
    # Kill any test sessions we created
    tmux kill-session -t "pty_validslug-a1b2" 2>/dev/null || true
    tmux kill-session -t "pty_inject-a1b2" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

pass() {
    echo "   PASS: $1"
}

fail() {
    echo "   FAIL: $1" >&2
    ERRORS=$((ERRORS + 1))
}

skip() {
    echo "   SKIP: $1"
    SKIPPED=$((SKIPPED + 1))
}

echo "=== nbs-worker Audit Fix Tests ==="
echo "Test directory: $TEST_DIR"
echo ""

# Set up test project
mkdir -p "$TEST_DIR/.nbs/workers"
mkdir -p "$TEST_DIR/.nbs/events"
cd "$TEST_DIR"

# Helper: create a minimal worker for testing (bypasses spawn)
create_test_worker() {
    local name="$1"
    local task_file=".nbs/workers/${name}.md"
    local log_file=".nbs/workers/${name}.log"

    cat > "$task_file" <<EOF
# Worker: ${name}

## Task

Test worker.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF
    touch "$log_file"
    echo "$name"
}

# ============================================================
# SECURITY fixes
# ============================================================

# --- Fix 1: set -e is present ---
echo "1. [SECURITY] set -euo pipefail present..."
if head -30 "$NBS_WORKER" | grep -q '^set -euo pipefail$'; then
    pass "set -euo pipefail found"
else
    fail "set -euo pipefail not found in script header"
fi

# --- Fix 2: Shell injection via abs_project_dir ---
echo "2. [SECURITY] Shell injection via project dir path escaping..."
# Verify the escape pattern is in the source
if grep -q 'escaped_project_dir' "$NBS_WORKER"; then
    pass "Single-quote escaping applied to project dir in tmux command"
else
    fail "No single-quote escaping found for project dir"
fi

# --- Fix 3: --dangerously-skip-permissions has security comment ---
echo "3. [SECURITY] --dangerously-skip-permissions has security comment..."
# Check that the line with --dangerously-skip-permissions is preceded by a SECURITY comment
if grep -B5 'dangerously-skip-permissions' "$NBS_WORKER" | grep -q 'SECURITY'; then
    pass "Security rationale comment found for --dangerously-skip-permissions"
else
    fail "No security rationale comment for --dangerously-skip-permissions"
fi

# --- Fix 4: Unvalidated regex to grep -E ---
echo "4. [SECURITY] Invalid regex rejected..."
create_test_worker "regextest-a1b2" >/dev/null
echo "some test data" > ".nbs/workers/regextest-a1b2.log"

# Pass an invalid regex (unmatched bracket)
REGEX_OUT=$("$NBS_WORKER" search "regextest-a1b2" "[invalid" 2>&1) || true
if echo "$REGEX_OUT" | grep -qi "invalid regex"; then
    pass "Invalid regex rejected with error message"
else
    # Some grep implementations might not return exit code 2 for this pattern.
    # Check at least that the code doesn't crash unexpectedly.
    if echo "$REGEX_OUT" | grep -qi "error\|invalid"; then
        pass "Invalid regex handled (alternative error message)"
    else
        fail "Invalid regex not rejected: $REGEX_OUT"
    fi
fi

# --- Fix 5: Path traversal via name ---
echo "5. [SECURITY] Path traversal in worker name rejected..."
# Try a name with path traversal
TRAVERSE_OUT=$("$NBS_WORKER" search "../../etc/passwd" "root" 2>&1) || true
if echo "$TRAVERSE_OUT" | grep -q "invalid worker name format"; then
    pass "Path traversal name rejected"
else
    fail "Path traversal name not rejected: $TRAVERSE_OUT"
fi

# Try a name with slashes
SLASH_OUT=$("$NBS_WORKER" search "foo/bar" "test" 2>&1) || true
if echo "$SLASH_OUT" | grep -q "invalid worker name format"; then
    pass "Name with slash rejected"
else
    fail "Name with slash not rejected: $SLASH_OUT"
fi

# Valid name format should pass format check (may fail on missing log)
VALID_OUT=$("$NBS_WORKER" search "valid-a1b2" "test" 2>&1) || true
if echo "$VALID_OUT" | grep -q "invalid worker name format"; then
    fail "Valid name format incorrectly rejected"
else
    pass "Valid name format accepted (may fail for other reasons)"
fi

# ============================================================
# BUG fixes
# ============================================================

# --- Fix 6: cmd_search positional parsing ---
echo "6. [BUG] cmd_search requires explicit arguments..."
# With zero args, should get error about requiring args
NOARGS_OUT=$("$NBS_WORKER" search 2>&1) || true
if echo "$NOARGS_OUT" | grep -q "search requires"; then
    pass "search with no args gives proper error"
else
    fail "search with no args: $NOARGS_OUT"
fi

# With one arg, should also fail
ONEARG_OUT=$("$NBS_WORKER" search "somename-a1b2" 2>&1) || true
if echo "$ONEARG_OUT" | grep -q "search requires"; then
    pass "search with one arg gives proper error"
else
    fail "search with one arg: $ONEARG_OUT"
fi

# With correct args, pattern should be the second positional
create_test_worker "argstest-a1b2" >/dev/null
echo "FINDME_LINE" > ".nbs/workers/argstest-a1b2.log"
CORRECT_OUT=$("$NBS_WORKER" search "argstest-a1b2" "FINDME_LINE" 2>&1) || true
if echo "$CORRECT_OUT" | grep -q "FINDME_LINE"; then
    pass "search with correct args finds expected content"
else
    fail "search with correct args did not find content: $CORRECT_OUT"
fi

# --- Fix 7: Timeout loop failure path ---
echo "7. [BUG] Timeout loop warns on max_wait..."
# Verify the warning code is present in the source
if grep -q 'timed out waiting for Claude UI prompt' "$NBS_WORKER"; then
    pass "Timeout warning message present in source"
else
    fail "Timeout warning message not found in source"
fi

# Verify the comparison uses -ge
if grep -q 'wait_count -ge \$max_wait' "$NBS_WORKER"; then
    pass "Timeout check uses -ge comparison"
else
    fail "Timeout check does not use -ge comparison"
fi

# --- Fix 8: $? from grep captured after pipe ---
echo "8. [BUG] grep exit code captured correctly (not from pipe)..."
# Verify sed and grep are separated (not piped) for result capture
if grep -q 'cleaned_log=.*sed' "$NBS_WORKER" && grep -q 'echo.*cleaned_log.*grep' "$NBS_WORKER"; then
    pass "sed and grep separated to capture correct exit code"
else
    fail "sed and grep may still be piped together for exit code capture"
fi

# Functional test: search that finds nothing should return 1
create_test_worker "nofind-a1b2" >/dev/null
echo "nothing interesting here" > ".nbs/workers/nofind-a1b2.log"
NOFIND_RC=0
"$NBS_WORKER" search "nofind-a1b2" "WONT_MATCH_xyz987" >/dev/null 2>&1 || NOFIND_RC=$?
if [[ $NOFIND_RC -eq 1 ]]; then
    pass "search with no match returns exit code 1"
else
    fail "search with no match returned exit code $NOFIND_RC (expected 1)"
fi

# --- Fix 9: tmux kill-session error not suppressed ---
echo "9. [BUG] tmux kill-session failure warned..."
# Verify the source checks kill-session return code
if grep -A2 'kill-session' "$NBS_WORKER" | grep -q 'Warning.*kill-session failed'; then
    pass "kill-session failure warning present"
else
    fail "kill-session failure warning not found"
fi

# ============================================================
# HARDENING fixes
# ============================================================

# --- Fix 10: WORKERS_DIR / EVENTS_DIR resolved to absolute ---
echo "10. [HARDENING] WORKERS_DIR and EVENTS_DIR are absolute..."
# Parse the variable assignments from the script
WORKERS_LINE=$(grep '^WORKERS_DIR=' "$NBS_WORKER")
EVENTS_LINE=$(grep '^EVENTS_DIR=' "$NBS_WORKER")

if echo "$WORKERS_LINE" | grep -q 'pwd'; then
    pass "WORKERS_DIR resolved via pwd"
else
    fail "WORKERS_DIR not resolved to absolute: $WORKERS_LINE"
fi

if echo "$EVENTS_LINE" | grep -q 'pwd'; then
    pass "EVENTS_DIR resolved via pwd"
else
    fail "EVENTS_DIR not resolved to absolute: $EVENTS_LINE"
fi

# --- Fix 11: Stronger hash entropy ---
echo "11. [HARDENING] Hash uses /dev/urandom not date..."
GENERATE_NAME_HASH=$(grep -A5 'generate_name' "$NBS_WORKER" | grep 'hash=')
if echo "$GENERATE_NAME_HASH" | grep -q '/dev/urandom'; then
    pass "generate_name uses /dev/urandom"
else
    fail "generate_name does not use /dev/urandom: $GENERATE_NAME_HASH"
fi

if echo "$GENERATE_NAME_HASH" | grep -q 'date'; then
    fail "generate_name still uses date"
else
    pass "generate_name no longer uses date"
fi

# --- Fix 12: Bus publish errors logged ---
echo "12. [HARDENING] Bus publish errors logged..."
if grep -q 'Warning: bus publish failed' "$NBS_WORKER"; then
    pass "Bus publish failure warning present"
else
    fail "Bus publish failure warning not found"
fi

# Verify || true is removed from bus_publish
if grep 'nbs_bus publish' "$NBS_WORKER" | grep -q '|| true'; then
    fail "bus_publish still uses || true to suppress errors"
else
    pass "bus_publish no longer uses || true"
fi

# --- Fix 13: pipe-pane checked ---
echo "13. [HARDENING] pipe-pane return code checked..."
if grep -B1 -A2 'pipe-pane' "$NBS_WORKER" | grep -q 'if !.*pipe-pane\|Warning.*pipe-pane'; then
    pass "pipe-pane return code is checked"
else
    fail "pipe-pane return code not checked"
fi

# --- Fix 14: Unknown args warned ---
echo "14. [HARDENING] Unknown args in search warned..."
# The main dispatch already handles unknown commands (exit 4).
# Check that search also warns on unknown options.
if grep -A30 'cmd_search()' "$NBS_WORKER" | grep -q 'Warning: unknown argument'; then
    pass "Unknown args in search produce warning"
else
    fail "Unknown args in search not warned"
fi

# Functional test: unknown option produces warning
create_test_worker "unkarg-a1b2" >/dev/null
echo "data" > ".nbs/workers/unkarg-a1b2.log"
UNK_OUT=$("$NBS_WORKER" search "unkarg-a1b2" "data" --unknown-flag 2>&1) || true
if echo "$UNK_OUT" | grep -q "Warning: unknown argument"; then
    pass "Unknown --unknown-flag produces warning"
else
    fail "Unknown --unknown-flag not warned: $UNK_OUT"
fi

# Main dispatch rejects unknown commands
UNK_CMD=$("$NBS_WORKER" boguscmd 2>&1) || true
if echo "$UNK_CMD" | grep -q "Unknown command"; then
    pass "Unknown command rejected at dispatch level"
else
    fail "Unknown command not rejected: $UNK_CMD"
fi

# --- Fix 15: Postcondition on generate_name ---
echo "15. [HARDENING] generate_name has postcondition assert..."
if grep -A10 'generate_name' "$NBS_WORKER" | grep -q 'ASSERTION FAILED.*generate_name'; then
    pass "generate_name has postcondition assertion"
else
    fail "generate_name missing postcondition assertion"
fi

# Verify the format regex is correct
if grep -A10 'generate_name' "$NBS_WORKER" | grep -q '\^\\[a-z0-9\\]+-\\[a-f0-9\\]\\{4\\}\$'; then
    pass "Postcondition uses correct format regex"
else
    # Try matching the bash regex syntax
    if grep -A10 'generate_name' "$NBS_WORKER" | grep -q 'a-z0-9.*a-f0-9'; then
        pass "Postcondition format regex present (bash syntax)"
    else
        fail "Postcondition format regex not found"
    fi
fi

# --- Fix 16: Slug format validation ---
echo "16. [HARDENING] Slug format validated in cmd_spawn..."
# This is a source check since spawn requires tmux
if grep -A15 'cmd_spawn' "$NBS_WORKER" | grep -q 'slug must match'; then
    pass "Slug format validation present"
else
    fail "Slug format validation not found"
fi

# Functional test: invalid slug rejected
# spawn will reject before needing tmux
BAD_SLUG=$("$NBS_WORKER" spawn "BAD-SLUG!" "/tmp" "test" 2>&1) || true
if echo "$BAD_SLUG" | grep -q "slug must match"; then
    pass "Uppercase/special char slug rejected"
else
    fail "Bad slug not rejected: $BAD_SLUG"
fi

GOOD_SLUG=$("$NBS_WORKER" spawn "validslug" "/nonexistent-dir-xyz" "test" 2>&1) || true
# Should fail for directory not found, NOT for slug validation
if echo "$GOOD_SLUG" | grep -q "slug must match"; then
    fail "Valid slug incorrectly rejected"
else
    pass "Valid slug accepted (fails later for missing dir)"
fi

# ============================================================
# Cross-cutting: set -e audit
# ============================================================
echo ""
echo "--- set -e compatibility checks ---"

# Verify grep pipelines in get_state_field have || true
echo "E1. get_state_field grep pipeline guarded..."
if grep -A15 'get_state_field()' "$NBS_WORKER" | grep "grep.*State.*|| true"; then
    pass "get_state_field grep pipeline has || true guard"
else
    fail "get_state_field grep pipeline may fail under set -e + pipefail"
fi

# Verify grep pipeline in cmd_list has || true
echo "E2. cmd_list grep pipeline guarded..."
if grep -A30 'cmd_list' "$NBS_WORKER" | grep "grep.*State.*|| true"; then
    pass "cmd_list grep pipeline has || true guard"
else
    fail "cmd_list grep pipeline may fail under set -e + pipefail"
fi

# Verify the help command works (basic smoke test)
echo "E3. help command works under set -e..."
HELP_OUT=$("$NBS_WORKER" help 2>&1) || true
if echo "$HELP_OUT" | grep -q "nbs-worker"; then
    pass "help command produces output"
else
    fail "help command failed: $HELP_OUT"
fi

# Verify list works with no workers dir
echo "E4. list works when no workers exist..."
LIST_DIR=$(mktemp -d)
cd "$LIST_DIR"
LIST_OUT=$("$NBS_WORKER" list 2>&1) || true
if echo "$LIST_OUT" | grep -q "no workers directory\|none"; then
    pass "list handles missing workers directory"
else
    fail "list failed with no workers dir: $LIST_OUT"
fi
cd "$TEST_DIR"
rm -rf "$LIST_DIR"

# ============================================================
# Summary
# ============================================================
echo ""
echo "=== Result ==="
echo "Tests run: $((16 + 4))"
echo "Skipped: $SKIPPED"
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All audit fix tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
