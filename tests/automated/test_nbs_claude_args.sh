#!/bin/bash
# test_nbs_claude_args.sh — Unit tests for nbs-claude argument building.
#
# Tests the CLAUDE_ARGS construction logic in isolation by sourcing a
# testable extract of nbs-claude's argument builder.
#
# Falsification approach: each test specifies the exact expected CLAUDE_ARGS
# array for given inputs. If the builder produces different args, the test fails.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

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

# We test the build_claude_args function by sourcing it and checking output.
# The function is extracted from nbs-claude into a testable helper.
# Output is one argument per line.
BUILDER="$PROJECT_ROOT/bin/nbs-claude-build-args"
if [[ ! -f "$BUILDER" ]]; then
    echo "FATAL: $BUILDER not found. Run the refactor first."
    exit 1
fi

# Helper: check if arg sequence appears in output (handles multi-word args
# split across lines, e.g. "--model\nopus[1m]")
has_arg() {
    local output="$1"
    local arg="$2"
    echo "$output" | grep -qF -- "$arg"
}

has_arg_pair() {
    local output="$1"
    local flag="$2"
    local value="$3"
    # Check flag appears on one line, value on the next
    echo "$output" | grep -A1 -F -- "$flag" | grep -qF -- "$value"
}

echo "=== nbs-claude Argument Builder Tests ==="
echo ""

# --- Test 1: Default args (no caller input) ---
echo "1. Default args include --dangerously-skip-permissions and --model..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" 2>/dev/null)
if has_arg "$RESULT" "--dangerously-skip-permissions"; then
    pass "T1a: --dangerously-skip-permissions present"
else
    fail "T1a: --dangerously-skip-permissions missing"
fi
if has_arg_pair "$RESULT" "--model" "opus[1m]"; then
    pass "T1b: --model opus[1m] present"
else
    fail "T1b: --model opus[1m] missing"
fi

# --- Test 2: Caller passes --dangerously-skip-permissions — no duplicate ---
echo ""
echo "2. No duplicate --dangerously-skip-permissions..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" --dangerously-skip-permissions 2>/dev/null)
COUNT=$(echo "$RESULT" | grep -cF -- "--dangerously-skip-permissions")
if [[ "$COUNT" -eq 1 ]]; then
    pass "T2: exactly one --dangerously-skip-permissions"
else
    fail "T2: expected 1, got $COUNT occurrences"
fi

# --- Test 3: --model= from caller overrides default ---
echo ""
echo "3. --model= from caller overrides default..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" --model=sonnet 2>/dev/null)
if has_arg_pair "$RESULT" "--model" "sonnet"; then
    pass "T3a: --model sonnet present"
else
    fail "T3a: --model sonnet missing"
fi
if has_arg "$RESULT" "opus"; then
    fail "T3b: default opus[1m] still present"
else
    pass "T3b: default opus[1m] correctly overridden"
fi

# --- Test 4: --continue= adds --resume ---
echo ""
echo "4. --continue= adds --resume..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="abc-123" \
    bash "$BUILDER" 2>/dev/null)
if has_arg_pair "$RESULT" "--resume" "abc-123"; then
    pass "T4: --resume abc-123 present"
else
    fail "T4: --resume abc-123 missing"
fi

# --- Test 5: --continue= from arg overrides env ---
echo ""
echo "5. --continue= from arg overrides env..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" --continue=xyz-789 2>/dev/null)
if has_arg_pair "$RESULT" "--resume" "xyz-789"; then
    pass "T5: --resume xyz-789 present"
else
    fail "T5: --resume xyz-789 missing"
fi

# --- Test 6: nbs-specific args stripped from passthrough ---
echo ""
echo "6. nbs-specific args stripped from passthrough..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" --root=/tmp --force --remote-host=foo 2>/dev/null)
if has_arg "$RESULT" "--root"; then
    fail "T6a: --root leaked into claude args"
else
    pass "T6a: --root correctly stripped"
fi
if has_arg "$RESULT" "--force"; then
    fail "T6b: --force leaked into claude args"
else
    pass "T6b: --force correctly stripped"
fi
if has_arg "$RESULT" "--remote-host"; then
    fail "T6c: --remote-host leaked into claude args"
else
    pass "T6c: --remote-host correctly stripped"
fi

# --- Test 7: Unknown args passed through ---
echo ""
echo "7. Unknown args passed through to claude..."
RESULT=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" --some-claude-flag --verbose 2>/dev/null)
if has_arg "$RESULT" "--some-claude-flag"; then
    pass "T7a: --some-claude-flag passed through"
else
    fail "T7a: --some-claude-flag missing"
fi
if has_arg "$RESULT" "--verbose"; then
    pass "T7b: --verbose passed through"
else
    fail "T7b: --verbose missing"
fi

# --- Test 8: Argument ordering ---
echo ""
echo "8. claude is first argument..."
FIRST=$(NBS_MODEL="opus[1m]" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" 2>/dev/null | head -1)
if [[ "$FIRST" == "claude" ]]; then
    pass "T8: claude is first arg"
else
    fail "T8: first arg is '$FIRST', expected 'claude'"
fi

# --- Test 9: Empty model ---
echo ""
echo "9. Empty model omits --model..."
RESULT=$(NBS_MODEL="" NBS_CONTINUE_SESSION="" \
    bash "$BUILDER" 2>/dev/null)
if has_arg "$RESULT" "--model"; then
    fail "T9: --model present with empty NBS_MODEL"
else
    pass "T9: --model correctly omitted"
fi

# --- Summary ---
echo ""
echo "=== Results: $PASS passed, $FAIL failed (of $TESTS tests) ==="

if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
exit 0
