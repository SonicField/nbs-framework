#!/bin/bash
# Test: Enhanced standup check-in message
#
# Tests that the standup check-in posted by the nbs-claude sidecar
# includes structured directives: @scribe summary request, @supervisor
# next-steps review, and all-agents status check.
#
# This tests the standup message content by extracting and validating
# the hardcoded string in nbs-claude's check_standup_trigger function.
# The test is integration-level: it sources the actual nbs-claude code
# indirectly by grepping the standup message from the script.
#
# Deterministic tests:
#   1.  Standup message contains @scribe mention
#   2.  Standup message contains @supervisor mention
#   3.  Standup message requests decision summary
#   4.  Standup message requests next steps
#   5.  Standup message asks agents for status
#   6.  Standup message asks about blockers
#   7.  Standup message includes open items request
#   8.  Standup message is posted by handle 'sidecar'
#   9.  Message is a single line (no newlines that break nbs-chat send)
#   10. Message length under 500 chars (nbs-chat send limit safety)
#
# Adversarial tests:
#   11. Message does not contain AskUserQuestion (banned in multi-agent)
#   12. Message does not contain /nbs- skill invocations (sidecar cannot invoke skills)
#   13. No unescaped single quotes that would break the shell string
#
# Integration test (requires nbs-chat binary):
#   14. Standup message can be sent and read back via nbs-chat

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

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

# --- Precondition: nbs-claude script exists ---
if [[ ! -f "$NBS_CLAUDE" ]]; then
    echo "FATAL: nbs-claude not found at $NBS_CLAUDE"
    exit 1
fi

echo "Test: Enhanced standup check-in message"
echo "======================================="
echo ""

# Extract the standup message from nbs-claude.
# The message is defined as a variable: local standup_msg='...'
# We extract the content between the single quotes.
STANDUP_MSG=$(grep "standup_msg='" "$NBS_CLAUDE" | head -1 | sed "s/.*standup_msg='//;s/'$//")

if [[ -z "$STANDUP_MSG" ]]; then
    echo "FATAL: Could not extract standup message from nbs-claude"
    echo "  Looked for: standup_msg='...'"
    echo "  In: $NBS_CLAUDE"
    exit 1
fi

echo "Extracted standup message:"
echo "  '$STANDUP_MSG'"
echo ""

# --- Content tests ---

echo "Content tests:"

# 1. Contains @scribe
if echo "$STANDUP_MSG" | grep -qF '@scribe'; then
    check "1. Contains @scribe mention" "pass"
else
    check "1. Contains @scribe mention" "fail"
fi

# 2. Contains @supervisor
if echo "$STANDUP_MSG" | grep -qF '@supervisor'; then
    check "2. Contains @supervisor mention" "pass"
else
    check "2. Contains @supervisor mention" "fail"
fi

# 3. Requests decision summary
if echo "$STANDUP_MSG" | grep -qi 'summary\|decisions'; then
    check "3. Requests decision summary" "pass"
else
    check "3. Requests decision summary" "fail"
fi

# 4. Requests next steps
if echo "$STANDUP_MSG" | grep -qi 'next steps\|next tasks\|assign next'; then
    check "4. Requests next steps" "pass"
else
    check "4. Requests next steps" "fail"
fi

# 5. Asks agents for status
if echo "$STANDUP_MSG" | grep -qi 'working on'; then
    check "5. Asks agents for status" "pass"
else
    check "5. Asks agents for status" "fail"
fi

# 6. Asks about blockers
if echo "$STANDUP_MSG" | grep -qi 'blocked'; then
    check "6. Asks about blockers" "pass"
else
    check "6. Asks about blockers" "fail"
fi

# 7. Includes open items request
if echo "$STANDUP_MSG" | grep -qi 'open items\|in-progress'; then
    check "7. Includes open items request" "pass"
else
    check "7. Includes open items request" "fail"
fi

# 8. Posted by handle 'sidecar' (verified by extraction pattern)
if grep -q "nbs-chat send.*sidecar" "$NBS_CLAUDE"; then
    check "8. Posted by handle 'sidecar'" "pass"
else
    check "8. Posted by handle 'sidecar'" "fail"
fi

# 9. Single line (no newlines)
LINE_COUNT=$(echo "$STANDUP_MSG" | wc -l)
if [[ "$LINE_COUNT" -eq 1 ]]; then
    check "9. Message is single line" "pass"
else
    check "9. Message is single line (got $LINE_COUNT lines)" "fail"
fi

# 10. Length under 500 chars
MSG_LEN=${#STANDUP_MSG}
if [[ "$MSG_LEN" -lt 500 ]]; then
    check "10. Length under 500 chars ($MSG_LEN)" "pass"
else
    check "10. Length under 500 chars ($MSG_LEN)" "fail"
fi

echo ""
echo "Adversarial tests:"

# 11. No AskUserQuestion
if echo "$STANDUP_MSG" | grep -qi 'AskUserQuestion'; then
    check "11. No AskUserQuestion reference" "fail"
else
    check "11. No AskUserQuestion reference" "pass"
fi

# 12. No /nbs- skill invocations
if echo "$STANDUP_MSG" | grep -qE '/nbs-'; then
    check "12. No /nbs- skill invocations" "fail"
else
    check "12. No /nbs- skill invocations" "pass"
fi

# 13. No unescaped single quotes (would break shell string)
# The message is wrapped in single quotes in nbs-claude, so internal
# single quotes would break the shell syntax.
if echo "$STANDUP_MSG" | grep -qF "'"; then
    check "13. No unescaped single quotes" "fail"
else
    check "13. No unescaped single quotes" "pass"
fi

echo ""
echo "Integration test:"

# 14. Round-trip via nbs-chat
if [[ -x "$NBS_CHAT" ]]; then
    CHAT_FILE="$TEST_DIR/standup-test.chat"
    "$NBS_CHAT" create "$CHAT_FILE" 2>/dev/null

    # Send the standup message
    "$NBS_CHAT" send "$CHAT_FILE" sidecar "$STANDUP_MSG" 2>/dev/null
    SEND_RC=$?

    # Read it back
    READ_OUTPUT=$("$NBS_CHAT" read "$CHAT_FILE" 2>/dev/null)
    READ_RC=$?

    if [[ $SEND_RC -eq 0 && $READ_RC -eq 0 ]] && echo "$READ_OUTPUT" | grep -qF '@scribe'; then
        check "14. Round-trip via nbs-chat" "pass"
    else
        check "14. Round-trip via nbs-chat (send=$SEND_RC read=$READ_RC)" "fail"
    fi
else
    echo "   SKIP: 14. nbs-chat binary not found"
fi

# --- Consistency test: local and remote messages match ---
echo ""
echo "Consistency test:"

# The standup message is defined once as standup_msg='...' and used in both
# local (nbs-chat send ... "$standup_msg") and remote (remote_cmd ... "$standup_msg").
# Since both use the same variable, we just verify the variable is used in both paths.
LOCAL_USES=$(grep -c '"$standup_msg"' "$NBS_CLAUDE" 2>/dev/null || echo 0)

if [[ "$LOCAL_USES" -ge 2 ]]; then
    check "15. Local and remote messages use same variable" "pass"
else
    check "15. Local and remote messages use same variable" "fail"
    echo "       Expected standup_msg used >= 2 times, found $LOCAL_USES"
fi

echo ""
echo "======================================="
if [[ $ERRORS -eq 0 ]]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "FAILURES: $ERRORS"
    exit 1
fi
