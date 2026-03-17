#!/bin/bash
# Test: AI-to-AI chat integration
#
# Two Claude instances communicate through a shared nbs-chat file.
# Worker A posts a finding, Worker B reads and responds.
# An evaluator Claude verifies the exchange was coherent.
#
# Falsification: Test fails if:
#   - Either worker fails to use nbs-chat commands
#   - The chat file contains fewer than 2 messages
#   - Worker B's response is not contextually related to Worker A's message
#   - Messages are from the same handle (not a real exchange)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
CHAT_FILE="$TEST_DIR/ai-chat-test.chat"
OUTPUT_A="$SCRIPT_DIR/verdicts/ai_chat_worker_a_${TIMESTAMP}.txt"
OUTPUT_B="$SCRIPT_DIR/verdicts/ai_chat_worker_b_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/ai_chat_integration_${TIMESTAMP}.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$SCRIPT_DIR/verdicts"

echo "=== AI-to-AI Chat Integration Test ==="
echo "Timestamp: $TIMESTAMP"
echo "Chat file: $CHAT_FILE"
echo ""

# Step 0: Create the chat file
echo "Step 0: Creating chat file..."
"$NBS_CHAT" create "$CHAT_FILE" >/dev/null
echo "  Created: $CHAT_FILE"
echo ""

# Step 1: Worker A posts a finding
echo "Step 1: Worker A posting finding..."

PROMPT_A="You are worker-a in an NBS teams setup. You have found a bug: the parse_int function in parser.py fails on negative inputs like -42 because it does not handle the leading minus sign.

Post this finding to the chat file so other workers can see it.

Use this exact command format:
${NBS_CHAT} send ${CHAT_FILE} worker-a \"<your message>\"

Post exactly ONE message describing the bug you found. Output only the command and its result, nothing else."

echo "$PROMPT_A" | claude -p - --output-format text --allowedTools "Bash" > "$OUTPUT_A" 2>&1 || true

echo "  Worker A output captured ($(wc -l < "$OUTPUT_A") lines)"
echo ""

# Verify Worker A actually wrote to the chat
MSG_COUNT_A=$("$NBS_CHAT" read "$CHAT_FILE" 2>/dev/null | wc -l)
if [[ "$MSG_COUNT_A" -eq 0 ]]; then
    echo -e "${RED}ABORT${NC}: Worker A failed to post any message"
    echo "Worker A output:"
    cat "$OUTPUT_A"
    exit 1
fi
echo "  Chat has $MSG_COUNT_A message(s) after Worker A"
echo ""

# Step 2: Worker B reads and responds
echo "Step 2: Worker B reading and responding..."

PROMPT_B="You are worker-b in an NBS teams setup. Another worker has posted a finding to a shared chat file.

First, read the chat file to see what was posted:
${NBS_CHAT} read ${CHAT_FILE}

Then respond with your own message acknowledging what you read and adding useful information (e.g., you can confirm the bug, suggest a fix approach, or note related issues).

Use this exact command format to respond:
${NBS_CHAT} send ${CHAT_FILE} worker-b \"<your response>\"

Read first, then post exactly ONE response. Output only the commands and their results."

echo "$PROMPT_B" | claude -p - --output-format text --allowedTools "Bash" > "$OUTPUT_B" 2>&1 || true

echo "  Worker B output captured ($(wc -l < "$OUTPUT_B") lines)"
echo ""

# Step 3: Read the final chat state
echo "Step 3: Reading final chat state..."
CHAT_CONTENTS=$("$NBS_CHAT" read "$CHAT_FILE" 2>/dev/null)
FINAL_MSG_COUNT=$(echo "$CHAT_CONTENTS" | grep -c '.' || true)

echo "  Final message count: $FINAL_MSG_COUNT"
echo "  Chat contents:"
echo "$CHAT_CONTENTS" | sed 's/^/    /'
echo ""

# Quick deterministic checks before evaluator
if [[ "$FINAL_MSG_COUNT" -lt 2 ]]; then
    echo -e "${RED}FAIL${NC}: Fewer than 2 messages in chat file"
    echo "Worker B output:"
    cat "$OUTPUT_B"
    exit 1
fi

# Check both handles are present
HAS_A=$(echo "$CHAT_CONTENTS" | grep -c '] worker-a:' || true)
HAS_B=$(echo "$CHAT_CONTENTS" | grep -c '] worker-b:' || true)

if [[ "$HAS_A" -eq 0 ]] || [[ "$HAS_B" -eq 0 ]]; then
    echo -e "${RED}FAIL${NC}: Both handles must be present (worker-a: $HAS_A, worker-b: $HAS_B)"
    exit 1
fi

# Step 4: Evaluator checks coherence
echo "Step 4: Evaluating conversation coherence..."

EVAL_PROMPT="You are a test evaluator. Two AI workers communicated via a shared chat file. Your job is to determine whether the exchange was coherent — i.e., Worker B's response is contextually related to Worker A's message.

## Chat Contents
---
${CHAT_CONTENTS}
---

## Worker A was told
Worker A was told to report a bug: parse_int fails on negative inputs like -42 because it doesn't handle the leading minus sign.

## Worker B was told
Worker B was told to read the chat, acknowledge what Worker A posted, and add useful information.

## Evaluation Criteria

1. **Worker A posted about the bug**: Does a message from worker-a mention parse_int, negative inputs, or the minus sign issue?
2. **Worker B responded contextually**: Does worker-b's message reference or acknowledge worker-a's finding? (Not just a generic response.)
3. **Different handles**: Messages come from at least two different handles.

Respond with ONLY valid JSON in this exact format:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"worker_a_on_topic\": true/false,
  \"worker_b_contextual\": true/false,
  \"different_handles\": true/false,
  \"reasoning\": \"<one sentence explaining verdict>\"
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
    echo -e "${GREEN}TEST PASSED${NC}: Two AIs communicated coherently via nbs-chat"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: AI-to-AI conversation was not coherent"
    echo ""
    echo "Worker A output:"
    head -30 "$OUTPUT_A"
    echo ""
    echo "Worker B output:"
    head -30 "$OUTPUT_B"
    exit 1
fi
