#!/bin/bash
# Test: Verify supervisor role uses nbs-workers for spawning workers
#
# 1. Loads updated supervisor role document and asks AI to spawn a worker
# 2. Evaluator AI checks for nbs-workers commands and absence of old pattern
# 3. Produces deterministic verdict file
# 4. Exit code based on verdict
#
# Falsification: Test fails if AI uses old pty-session spawn pattern or
#                hedges about nbs-workers availability

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SCENARIO_DIR="$SCRIPT_DIR/scenarios/supervisor_nbs_worker"
SUPERVISOR_DOC="$PROJECT_ROOT/claude_tools/nbs-supervisor.md"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/supervisor_nbs_worker_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/supervisor_nbs_worker_verdict_$TIMESTAMP.json"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Supervisor nbs-workers Adoption Test ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Build prompt with supervisor role
echo "Step 1: Running supervisor spawn prompt..."
cd "$SCENARIO_DIR" || exit 1

SUPERVISOR_CONTENT=$(cat "$SUPERVISOR_DOC")

PROMPT="You are acting as an NBS Teams supervisor. Here is your role document:

---
$SUPERVISOR_CONTENT
---

You are in a project directory with a .nbs/ structure already set up.
The project is at /home/user/my-project.

Your task: Spawn a worker to implement a parser module that passes all tests in test_parser.py.

Respond with the exact commands and steps you would take to spawn this worker. Be specific about the commands you would use. Show the actual command invocations."

echo "$PROMPT" | claude -p - --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 2: Read criteria and evaluate
echo "Step 2: Evaluating output..."

CRITERIA=$(cat "$SCENARIO_DIR/TEST_CRITERIA.md")
OUTPUT=$(cat "$OUTPUT_FILE")

EVAL_PROMPT="You are a test evaluator. Your job is to determine whether an AI supervisor correctly uses nbs-workers for worker management.

## Test Criteria
$CRITERIA

## AI Output To Evaluate
---
$OUTPUT
---

## Your Task

Check the output for these three conditions:

### Condition 1: Uses nbs-workers (REQUIRED for PASS)
Does the output contain nbs-workers commands? Look for:
- \"nbs-workers spawn\" (the spawn command)
- \"nbs-workers status\" or \"nbs-workers search\" or \"nbs-workers results\" (monitoring commands)
- \"nbs-workers dismiss\" (cleanup command)

### Condition 2: No old pattern (REQUIRED for PASS)
Does the output contain the OLD pty-session spawn pattern? Look for:
- \"temp.sh\" (the old workaround script)
- \"pty-session create\" used for spawning a worker (not for raw terminal use)
- Manual task file creation BEFORE spawning (nbs-workers creates task files automatically)
- \"pty-session send\" with a prompt to read a task file

If the old pattern is present, verdict is FAIL.

### Condition 3: No hedging (REQUIRED for PASS)
Does the output contain hedging phrases about nbs-workers availability? (case insensitive)
- \"if nbs-workers is installed\"
- \"check if nbs-workers\"
- \"ensure nbs-workers\"
- \"may not be available\"
- \"might not be installed\"

If hedging is present, verdict is FAIL.

Respond with ONLY valid JSON in this exact format:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"has_nbs_worker_commands\": true/false,
  \"old_pattern_detected\": true/false,
  \"old_pattern_evidence\": [\"list\", \"of\", \"phrases\"] or [],
  \"hedging_detected\": true/false,
  \"hedging_phrases_found\": [\"list\", \"of\", \"phrases\"] or [],
  \"reasoning\": \"<one sentence explaining verdict>\"
}"

EVAL_RESULT=$(echo "$EVAL_PROMPT" | claude -p - --output-format text 2>&1)

# Save eval result to temp file for extraction
EVAL_TEMP=$(mktemp)
echo "$EVAL_RESULT" > "$EVAL_TEMP"

# Extract JSON from response
JSON_VERDICT=$("$EXTRACT_JSON" "$EVAL_TEMP")
EXTRACT_STATUS=$?

rm -f "$EVAL_TEMP"

if [[ $EXTRACT_STATUS -ne 0 ]] || [[ -z "$JSON_VERDICT" ]]; then
    echo -e "${RED}ERROR${NC}: Could not extract JSON from evaluator response"
    echo "Raw response:"
    echo "$EVAL_RESULT"
    exit 2
fi

# Write verdict file (state of truth)
echo "$JSON_VERDICT" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

# Step 3: Parse verdict and report
echo "Step 3: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

# Extract pass/fail
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
