#!/bin/bash
# Test: AI-driven control inbox integration
#
# An AI is told about a new chat resource and asked to register it via
# the control inbox. The sidecar's check_control_inbox() function is then
# invoked to process the inbox. An evaluator Claude verifies the round-trip
# worked correctly.
#
# This tests the full AI-to-sidecar loop:
#   1. AI writes to .nbs/control-inbox
#   2. Sidecar processes new lines → updates .nbs/control-registry
#   3. AI reads registry to confirm registration
#
# Falsification: Test fails if:
#   - AI does not write to .nbs/control-inbox
#   - Sidecar does not add the resource to the registry
#   - AI cannot confirm the resource is registered
#   - Evaluator determines the AI did not follow the protocol correctly

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
AI_OUTPUT="$SCRIPT_DIR/verdicts/control_inbox_ai_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/control_inbox_ai_${TIMESTAMP}.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$SCRIPT_DIR/verdicts"

echo "=== Control Inbox AI Integration Test ==="
echo "Timestamp: $TIMESTAMP"
echo "Test dir:  $TEST_DIR"
echo ""

# Step 0: Set up a realistic .nbs/ project directory
echo "Step 0: Setting up test project..."
mkdir -p "$TEST_DIR/.nbs/chat" "$TEST_DIR/.nbs/events"
"$NBS_CHAT" create "$TEST_DIR/.nbs/chat/live.chat" >/dev/null
touch "$TEST_DIR/.nbs/control-registry"
echo "chat:$TEST_DIR/.nbs/chat/live.chat" > "$TEST_DIR/.nbs/control-registry"
echo "  Created project with live.chat and empty registry"
echo ""

# Step 1: AI registers a new resource via the control inbox
echo "Step 1: AI registering a new chat resource..."

PROMPT_AI="You are an NBS agent working in the project directory: $TEST_DIR

You have just learned (from another agent via chat) that a new debug chat
channel has been created at: $TEST_DIR/.nbs/chat/debug.chat

Your task:
1. Create the chat file using: $NBS_CHAT create $TEST_DIR/.nbs/chat/debug.chat
2. Register it with the sidecar by appending to the control inbox:
   echo \"register-chat $TEST_DIR/.nbs/chat/debug.chat\" >> $TEST_DIR/.nbs/control-inbox
3. Read the control inbox to confirm your registration was written:
   cat $TEST_DIR/.nbs/control-inbox

Do these three steps in order. Output only the commands and their results."

echo "$PROMPT_AI" | claude -p - --output-format text --allowedTools "Bash" > "$AI_OUTPUT" 2>&1 || true

echo "  AI output captured ($(wc -l < "$AI_OUTPUT") lines)"
echo ""

# Step 2: Verify AI wrote to the control inbox
echo "Step 2: Checking control inbox..."
if [[ ! -f "$TEST_DIR/.nbs/control-inbox" ]]; then
    echo -e "${RED}ABORT${NC}: AI did not create control inbox file"
    echo "AI output:"
    cat "$AI_OUTPUT"
    exit 1
fi

INBOX_CONTENTS=$(cat "$TEST_DIR/.nbs/control-inbox")
echo "  Inbox contents: $INBOX_CONTENTS"

if ! echo "$INBOX_CONTENTS" | grep -qF "register-chat"; then
    echo -e "${RED}ABORT${NC}: AI did not write register-chat command to inbox"
    echo "AI output:"
    cat "$AI_OUTPUT"
    exit 1
fi
echo "  ✓ Inbox contains register-chat command"
echo ""

# Step 3: Simulate sidecar processing
echo "Step 3: Running sidecar check_control_inbox()..."

# Source the control inbox functions from nbs-claude
_EXTRACT_TMP=$(mktemp)
sed -n '/^# --- Dynamic resource registration ---/,/^# --- Idle detection sidecar/p' "$NBS_CLAUDE" | head -n -2 > "$_EXTRACT_TMP"

# Run in a subshell with the test dir as working directory
REGISTRY_AFTER=$(cd "$TEST_DIR" && {
    # Set variables needed by sourced functions
    NBS_ROOT="$PWD"
    NBS_REMOTE_HOST=""
    NBS_LOG_FILE="/dev/null"
    SIDECAR_HANDLE="test"

    source "$_EXTRACT_TMP"

    # Override paths after sourcing (source sets them from NBS_ROOT/SIDECAR_HANDLE)
    CONTROL_INBOX=".nbs/control-inbox"
    CONTROL_REGISTRY=".nbs/control-registry"
    CONTROL_INBOX_LINE=0

    # Process the inbox
    check_control_inbox

    # Output the registry
    cat .nbs/control-registry
} 2>&1)

rm -f "$_EXTRACT_TMP"

echo "  Registry after sidecar processing:"
echo "$REGISTRY_AFTER" | sed 's/^/    /'
echo ""

# Deterministic check: registry must contain the new chat
if echo "$REGISTRY_AFTER" | grep -qF "chat:$TEST_DIR/.nbs/chat/debug.chat"; then
    echo "  ✓ Registry contains the new chat resource"
else
    echo -e "${RED}FAIL${NC}: Registry does not contain the registered chat"
    echo "Registry contents:"
    echo "$REGISTRY_AFTER"
    exit 1
fi
echo ""

# Step 4: Evaluator checks the AI followed the protocol
echo "Step 4: Evaluating AI behaviour..."

AI_OUTPUT_CONTENTS=$(cat "$AI_OUTPUT")

EVAL_PROMPT="You are a test evaluator. An AI agent was asked to register a new chat resource using the NBS control inbox mechanism. Evaluate whether it followed the protocol correctly.

## AI Output
---
${AI_OUTPUT_CONTENTS}
---

## Expected Protocol
1. Create the chat file using nbs-chat create
2. Append 'register-chat <path>' to .nbs/control-inbox
3. Read back the inbox to confirm

## Actual Results
- Control inbox was written: YES
- Inbox contains 'register-chat': YES
- Sidecar processed inbox and registry updated: YES
- Registry now contains: $(cat "$TEST_DIR/.nbs/control-registry" 2>/dev/null)

## Evaluation Criteria

1. **created_chat**: Did the AI create the chat file (ran nbs-chat create)?
2. **wrote_inbox**: Did the AI write to .nbs/control-inbox using echo/append?
3. **correct_format**: Was the inbox line in the correct format (register-chat <path>)?
4. **verified**: Did the AI read back the inbox to confirm?

Respond with ONLY valid JSON:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"created_chat\": true/false,
  \"wrote_inbox\": true/false,
  \"correct_format\": true/false,
  \"verified\": true/false,
  \"reasoning\": \"<one sentence>\"
}"

EVAL_TEMP=$(mktemp)
EVAL_RESULT=$(echo "$EVAL_PROMPT" | claude -p - --output-format text 2>&1)
echo "$EVAL_RESULT" > "$EVAL_TEMP"

JSON_VERDICT=$("$EXTRACT_JSON" "$EVAL_TEMP")
EXTRACT_STATUS=$?
rm -f "$EVAL_TEMP"

if [[ $EXTRACT_STATUS -ne 0 ]] || [[ -z "$JSON_VERDICT" ]]; then
    echo -e "${RED}ERROR${NC}: Could not extract JSON from evaluator response"
    echo "Raw response:"
    echo "$EVAL_RESULT"
    exit 2
fi

# Write verdict
echo "$JSON_VERDICT" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

# Step 5: Report
echo "Step 5: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}: AI correctly used control inbox for dynamic registration"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: AI did not follow the control inbox protocol"
    echo ""
    echo "AI output:"
    head -40 "$AI_OUTPUT"
    exit 1
fi
