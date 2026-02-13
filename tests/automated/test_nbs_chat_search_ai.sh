#!/bin/bash
# Test: AI-driven nbs-chat search integration
#
# An AI is given a chat file containing a conversation between several
# workers, and asked to find specific information using nbs-chat search.
# The test verifies the AI can use the search command effectively to
# answer questions about the chat history.
#
# This tests the AI's ability to:
#   1. Use nbs-chat search with patterns
#   2. Use --handle filter to narrow results
#   3. Synthesise information from search results
#
# Falsification: Test fails if:
#   - AI does not use nbs-chat search
#   - AI gives incorrect answers to questions about the chat history
#   - Evaluator determines the AI did not use the search feature correctly

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
AI_OUTPUT="$SCRIPT_DIR/verdicts/chat_search_ai_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/chat_search_ai_${TIMESTAMP}.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$(dirname "$AI_OUTPUT")"

echo "=== AI-Driven Chat Search Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# Step 0: Create a realistic chat with searchable content
echo "Step 0: Populating test chat..."
CHAT="$TEST_DIR/debug.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Simulate a debugging conversation between three workers
"$NBS_CHAT" send "$CHAT" "parser-worker" "Found 3 failing tests in the tokeniser module" >/dev/null
"$NBS_CHAT" send "$CHAT" "test-runner" "Confirmed: test_parse_int, test_parse_float, test_parse_string all fail" >/dev/null
"$NBS_CHAT" send "$CHAT" "parser-worker" "Root cause: the isdigit check does not handle negative numbers" >/dev/null
"$NBS_CHAT" send "$CHAT" "test-runner" "I see the same pattern. The minus sign is rejected by isdigit()" >/dev/null
"$NBS_CHAT" send "$CHAT" "supervisor" "Good find. parser-worker, please fix the isdigit issue" >/dev/null
"$NBS_CHAT" send "$CHAT" "parser-worker" "Fixed: added check for leading minus in parse_int()" >/dev/null
"$NBS_CHAT" send "$CHAT" "test-runner" "Retesting... test_parse_int now passes. Other 2 still failing" >/dev/null
"$NBS_CHAT" send "$CHAT" "parser-worker" "The float and string parsers have separate bugs. Investigating." >/dev/null
"$NBS_CHAT" send "$CHAT" "supervisor" "Priority: fix parse_float next, string can wait" >/dev/null
"$NBS_CHAT" send "$CHAT" "parser-worker" "parse_float fixed: was not handling decimal point correctly" >/dev/null
"$NBS_CHAT" send "$CHAT" "test-runner" "All tests passing now. parse_string was already correct after the float fix" >/dev/null
"$NBS_CHAT" send "$CHAT" "supervisor" "Excellent. Moving on to the validator module next" >/dev/null

MSG_COUNT=$("$NBS_CHAT" read "$CHAT" | wc -l)
echo "  Created chat with $MSG_COUNT messages from 3 participants"
echo ""

# Step 1: AI uses search to answer questions
echo "Step 1: AI answering questions using search..."

PROMPT_AI="Your ONLY task is to run bash commands. Do NOT write explanations or answers in text.

For each of the 4 questions below, you MUST:
1. Run the exact nbs-chat search command shown
2. Copy the search output

Do NOT answer the questions yourself. Just run the commands and output the results.

Commands to run (run each one separately using the Bash tool):

Question 1:
$NBS_CHAT search $CHAT 'root cause'

Question 2:
$NBS_CHAT search $CHAT 'isdigit'

Question 3:
$NBS_CHAT search $CHAT 'priority' --handle=supervisor

Question 4:
$NBS_CHAT search $CHAT 'passes'

After running all 4 commands, output a brief summary of what each search found."

echo "$PROMPT_AI" | claude -p - --output-format text --allowedTools "Bash" > "$AI_OUTPUT" 2>&1 || true

echo "  AI output captured ($(wc -l < "$AI_OUTPUT") lines)"
echo ""

# Step 2: Evaluator checks the AI's answers
echo "Step 2: Evaluating AI answers..."

AI_OUTPUT_CONTENTS=$(cat "$AI_OUTPUT")

EVAL_PROMPT="You are a test evaluator. An AI was given a chat history and asked to answer 4 questions using the nbs-chat search command.

## Ground Truth

The chat contains 12 messages from parser-worker, test-runner, and supervisor about debugging tokeniser test failures.

Correct answers:
1. Root cause: the isdigit check does not handle negative numbers (the minus sign is rejected)
2. Three participants mentioned 'isdigit': parser-worker, test-runner, and supervisor
3. The supervisor prioritised fixing parse_float next (string can wait)
4. test_parse_int was the first test to pass

## AI Output
---
${AI_OUTPUT_CONTENTS}
---

## Evaluation Criteria

For each question, evaluate:
- **used_search**: Did the AI run nbs-chat search (not just nbs-chat read)?
- **correct_answer**: Is the AI's answer factually correct?

Respond with ONLY valid JSON:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"q1_used_search\": true/false,
  \"q1_correct\": true/false,
  \"q2_used_search\": true/false,
  \"q2_correct\": true/false,
  \"q3_used_search\": true/false,
  \"q3_correct\": true/false,
  \"q4_used_search\": true/false,
  \"q4_correct\": true/false,
  \"reasoning\": \"<one sentence summary>\"
}

The verdict is PASS if the AI used search for at least 3 of 4 questions AND got at least 3 of 4 answers correct."

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

# Step 3: Report
echo "Step 3: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}: AI effectively used nbs-chat search to answer questions"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: AI did not use search effectively"
    echo ""
    echo "AI output:"
    head -50 "$AI_OUTPUT"
    exit 1
fi
