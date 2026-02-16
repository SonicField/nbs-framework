#!/bin/bash
# Test: Verify /nbs command via AI evaluation
#
# 1. Runs /nbs on test scenario
# 2. Evaluator AI judges output against explicit criteria
# 3. Produces deterministic verdict file (state of truth)
# 4. Exit code based on verdict
#
# Falsification: Test fails if evaluator determines output missed known issues

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SCENARIO_DIR="$SCRIPT_DIR/scenarios/no_plan_project"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/nbs_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/nbs_verdict_$TIMESTAMP.json"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== NBS Command Test ==="
echo "Scenario: no_plan_project"
echo "Timestamp: $TIMESTAMP"
echo ""

# Prerequisite: ensure /nbs skill is registered (repair broken symlinks)
NBS_SKILL="$HOME/.claude/commands/nbs.md"
if [[ ! -e "$NBS_SKILL" ]]; then
    echo "Skill symlink broken or missing — running install.sh to repair..."
    echo 'n' | "$PROJECT_ROOT/bin/install.sh" >/dev/null 2>&1
    if [[ ! -e "$NBS_SKILL" ]]; then
        echo "SKIP: Could not install /nbs skill"
        exit 0
    fi
    echo "  Repaired."
    echo ""
fi

# Step 1: Run /nbs in scenario directory
echo "Step 1: Running /nbs command..."
cd "$SCENARIO_DIR" || exit 1

claude -p "/nbs" --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 2: Read criteria and evaluate
echo "Step 2: Evaluating output against criteria..."

CRITERIA=$(cat "$SCENARIO_DIR/TEST_CRITERIA.md")
OUTPUT=$(cat "$OUTPUT_FILE")

EVAL_PROMPT="You are a test evaluator. Your job is to determine whether an nbs review tool produced correct output for a known test scenario.

## Test Criteria
$CRITERIA

## Tool Output To Evaluate
---
$OUTPUT
---

## Your Task

Evaluate the output and produce a JSON verdict. Be strict but fair.

Respond with ONLY valid JSON in this exact format:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"identified_issues\": {
    \"no_plan\": true/false,
    \"no_progress_log\": true/false,
    \"unclear_goals\": true/false,
    \"has_recommendations\": true/false
  },
  \"issues_found\": <number 0-4>,
  \"is_concise\": true/false,
  \"has_structure\": true/false,
  \"hallucinations\": true/false,
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
