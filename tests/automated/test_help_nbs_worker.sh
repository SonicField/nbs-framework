#!/bin/bash
# Test: Verify help skill recommends nbs-workers for spawning workers
#
# 1. Loads help skill document and asks "How do I spawn workers?"
# 2. Evaluator AI checks for nbs-workers commands in the response
# 3. Produces deterministic verdict file
# 4. Exit code based on verdict
#
# Falsification: Test fails if AI recommends raw pty-session for spawning
#                or does not mention nbs-workers at all

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
HELP_DOC="$PROJECT_ROOT/claude_tools/nbs-teams-help.md"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/help_nbs_worker_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/help_nbs_worker_verdict_$TIMESTAMP.json"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Help Skill nbs-workers Recommendation Test ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Simulate asking help about spawning workers
echo "Step 1: Asking help about spawning..."

HELP_CONTENT=$(cat "$HELP_DOC")

PROMPT="You are running the NBS Teams Help skill. Here is your skill document:

---
$HELP_CONTENT
---

The user has selected option 2: 'Spawning workers - How do I create and run worker Claudes?'

They have an active project at /home/user/my-project with a .nbs/ directory.
Their terminal goal is: 'Implement a REST API server with full test coverage.'

Respond as the help skill would, explaining how to spawn workers."

echo "$PROMPT" | claude -p - --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 2: Evaluate
echo "Step 2: Evaluating for nbs-workers recommendation..."

OUTPUT=$(cat "$OUTPUT_FILE")

EVAL_PROMPT="You are a test evaluator. Your job is to determine whether an NBS Teams help response correctly recommends nbs-workers for spawning workers.

## AI Output To Evaluate
---
$OUTPUT
---

## Your Task

Check the output for:

1. Does it mention or use nbs-workers? (Look for 'nbs-workers spawn' or 'nbs-workers')
   This is REQUIRED for PASS.

2. Does it recommend the OLD pattern of raw pty-session create/send for spawning?
   If it recommends pty-session for spawning workers (not for REPLs), this is FAIL.
   Note: mentioning pty-session as a lower-level tool or for REPLs is fine.

3. Does it show a single-command spawn workflow?
   (nbs-workers spawn <slug> <dir> <desc> — one command instead of multi-step)

Respond with ONLY valid JSON in this exact format:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"mentions_nbs_worker\": true/false,
  \"recommends_old_pty_pattern\": true/false,
  \"shows_single_command_spawn\": true/false,
  \"reasoning\": \"<one sentence explaining verdict>\"
}"

EVAL_RESULT=$(echo "$EVAL_PROMPT" | claude -p - --output-format text 2>&1)

EVAL_TEMP=$(mktemp)
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

echo "$JSON_VERDICT" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

echo "Step 3: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}"
    echo ""
    echo "Output was:"
    head -50 "$OUTPUT_FILE"
    exit 1
fi
