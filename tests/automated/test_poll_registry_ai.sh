#!/bin/bash
# Test: AI-driven nbs-poll with dynamic registration
#
# An AI is given the /nbs-poll skill and a project with a .nbs/control-registry
# containing registered resources. The test verifies the AI checks the
# registered resources rather than only using glob patterns.
#
# The registry includes a chat file in a non-standard location (not .nbs/chat/)
# to ensure the AI is reading the registry, not just scanning .nbs/chat/*.chat.
#
# Falsification: Test fails if:
#   - AI does not read the control registry
#   - AI does not check the registered chat file in the non-standard location
#   - AI only checks .nbs/chat/*.chat and misses the registered resource
#   - Evaluator determines the AI did not follow registry-aware polling

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
AI_OUTPUT="$SCRIPT_DIR/verdicts/poll_registry_ai_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/poll_registry_ai_${TIMESTAMP}.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$SCRIPT_DIR/verdicts"

echo "=== nbs-poll Registry-Aware Test ==="
echo "Timestamp: $TIMESTAMP"
echo "Test dir:  $TEST_DIR"
echo ""

# Step 0: Set up project with registry containing a non-standard chat path
echo "Step 0: Setting up test project..."

mkdir -p "$TEST_DIR/.nbs/chat"
mkdir -p "$TEST_DIR/external-chats"

# Create a standard chat (in .nbs/chat/)
"$NBS_CHAT" create "$TEST_DIR/.nbs/chat/live.chat" >/dev/null

# Create a non-standard chat (outside .nbs/chat/) — only discoverable via registry
"$NBS_CHAT" create "$TEST_DIR/external-chats/remote-team.chat" >/dev/null

# Add a message from another agent to the non-standard chat
"$NBS_CHAT" send "$TEST_DIR/external-chats/remote-team.chat" remote-agent \
    "Priority: the staging deployment is failing on the auth module. Need help debugging. Can you check the logs?" >/dev/null

# Create registry with both chats registered
cat > "$TEST_DIR/.nbs/control-registry" << EOF
chat:$TEST_DIR/.nbs/chat/live.chat
chat:$TEST_DIR/external-chats/remote-team.chat
EOF

echo "  Created standard chat: .nbs/chat/live.chat (no new messages)"
echo "  Created non-standard chat: external-chats/remote-team.chat (1 unread message)"
echo "  Registry lists both chats"
echo ""

# Step 1: AI runs a poll-like check
echo "Step 1: AI checking for messages..."

# Read the nbs-poll skill doc
POLL_DOC=$(cat "$PROJECT_ROOT/claude_tools/nbs-poll.md")

PROMPT_AI="You are an NBS agent (handle: test-agent) working in: $TEST_DIR

Here is your polling skill documentation:
---
${POLL_DOC}
---

Run a poll check now. Important details:
- The nbs-chat binary is at: $NBS_CHAT
- There is a control registry at: $TEST_DIR/.nbs/control-registry
- Read the registry FIRST to find which resources to check
- For each registered chat, check for messages using: $NBS_CHAT read <path>
- Report what you find

Note: You are doing a poll, not starting new work. Just check and report."

echo "$PROMPT_AI" | claude -p - --output-format text --allowedTools "Bash,Read" > "$AI_OUTPUT" 2>&1 || true

echo "  AI output captured ($(wc -l < "$AI_OUTPUT") lines)"
echo ""

# Step 2: Deterministic checks
echo "Step 2: Checking AI behaviour..."

AI_OUTPUT_CONTENTS=$(cat "$AI_OUTPUT")

# Check if AI read the registry
READ_REGISTRY=0
if echo "$AI_OUTPUT_CONTENTS" | grep -qF "control-registry"; then
    READ_REGISTRY=1
    echo "  ✓ AI referenced the control registry"
else
    echo "  ✗ AI did not reference the control registry"
fi

# Check if AI found the non-standard chat message
FOUND_REMOTE=0
if echo "$AI_OUTPUT_CONTENTS" | grep -qiE "remote|staging|auth|deploy"; then
    FOUND_REMOTE=1
    echo "  ✓ AI found the message in the non-standard chat"
else
    echo "  ✗ AI did not find the message in the non-standard chat"
fi

echo ""

# Step 3: Evaluator
echo "Step 3: Evaluating poll behaviour..."

EVAL_PROMPT="You are a test evaluator. An AI agent was asked to poll for messages using the NBS polling protocol. The key test: the project has a .nbs/control-registry that lists chat files including one in a non-standard location (external-chats/remote-team.chat). The AI should have read the registry and checked ALL listed chats, not just .nbs/chat/*.chat.

## AI Output
---
${AI_OUTPUT_CONTENTS}
---

## Ground Truth
- .nbs/control-registry contains two entries:
  chat:$TEST_DIR/.nbs/chat/live.chat
  chat:$TEST_DIR/external-chats/remote-team.chat
- live.chat has no unread messages
- remote-team.chat has one unread message from remote-agent about a staging deployment failure

## Evaluation Criteria

1. **read_registry**: Did the AI read or reference .nbs/control-registry?
2. **checked_nonstandard**: Did the AI check the non-standard chat (external-chats/remote-team.chat)?
3. **found_message**: Did the AI find and report the staging deployment message?
4. **brief_report**: Was the AI's report brief (not starting new work, just reporting)?

Respond with ONLY valid JSON:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"read_registry\": true/false,
  \"checked_nonstandard\": true/false,
  \"found_message\": true/false,
  \"brief_report\": true/false,
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

# Step 4: Report
echo "Step 4: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}: AI correctly used registry for polling"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: AI did not follow registry-aware polling"
    echo ""
    echo "AI output:"
    head -50 "$AI_OUTPUT"
    exit 1
fi
