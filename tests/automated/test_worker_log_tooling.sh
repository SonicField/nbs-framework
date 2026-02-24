#!/bin/bash
# Test: Verify worker uses nbs-workers search for log access
#
# 1. Gives AI a worker task file with Tooling section + a task needing log access
# 2. Evaluator AI checks for nbs-workers search usage vs raw log access
# 3. Produces deterministic verdict file
# 4. Exit code based on verdict
#
# Falsification: Test fails if AI reads raw .log files instead of using
#                nbs-workers search

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
WORKER_DOC="$PROJECT_ROOT/claude_tools/nbs-worker.md"
SCENARIO_DIR="$SCRIPT_DIR/scenarios/worker_log_tooling"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/worker_log_tooling_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/worker_log_tooling_verdict_$TIMESTAMP.json"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Worker Log Tooling Test ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Build prompt with worker role + task file containing Tooling section
echo "Step 1: Running worker log access prompt..."
cd "$SCENARIO_DIR" || exit 1

WORKER_CONTENT=$(cat "$WORKER_DOC")

# Simulate a task file as nbs-workers spawn would create it
TASK_FILE_CONTENT='# Worker: log-checker

## Task

Check whether worker parser-a3f1 has finished building the parser module.
Look in its logs for evidence of test results. Report whether all tests passed.

## Tooling

Your supervisor monitors you via `nbs-workers`. These tips avoid common mistakes:

- **Do not read raw .log files** — they contain ANSI escape codes. Use `nbs-workers search <name> <regex>` for clean, searchable output.
- **Update Status and Log sections** in this file when done — your supervisor reads them via `nbs-workers results`.
- **Escalate blockers** by setting State to `escalated` — do not work around problems silently.

## Status

State: running
Started: 2026-02-10 14:30:00
Completed:

## Log

[Worker appends findings here]'

PROMPT="You are acting as an NBS Teams worker. Here is your role document:

---
$WORKER_CONTENT
---

Here is your task file (created by nbs-workers spawn):

---
$TASK_FILE_CONTENT
---

The project is at /home/user/my-project. Worker parser-a3f1 exists and has been running.
Its log is at .nbs/workers/parser-a3f1.log and its task file is at .nbs/workers/parser-a3f1.md.

Respond with the exact commands you would use to check parser-a3f1's progress and find its test results. Be specific about the commands. Show the actual command invocations you would run."

echo "$PROMPT" | claude -p - --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 2: Evaluate
echo "Step 2: Evaluating output..."

CRITERIA=$(cat "$SCENARIO_DIR/TEST_CRITERIA.md")
OUTPUT=$(cat "$OUTPUT_FILE")

EVAL_PROMPT="You are a test evaluator. Your job is to determine whether a worker AI correctly uses nbs-workers tooling for log access instead of reading raw log files.

## Test Criteria
$CRITERIA

## AI Output To Evaluate
---
$OUTPUT
---

## Your Task

Check the output for these two conditions:

### Condition 1: Uses nbs-workers commands (REQUIRED for PASS)
Does the output use nbs-workers for checking logs? Look for:
- \"nbs-workers search\" (the search command for logs)
- \"nbs-workers results\" (reading completed task results)
- \"nbs-workers status\" (checking worker status)

At least one of these must be present.

### Condition 2: No raw log access (REQUIRED for PASS)
Does the output directly access raw .log files? Look for:
- \"cat\" on any .log file
- \"tail\" on any .log file
- \"head\" on any .log file
- \"Read\" tool on any .log file (e.g., Read .nbs/workers/parser-a3f1.log)
- \"grep\" directly on a .log file without ANSI stripping
- \"strings\" piped from a .log file
- Any direct file reading of a path ending in .log

If raw log access is present, verdict is FAIL.

Respond with ONLY valid JSON in this exact format:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"uses_nbs_worker_commands\": true/false,
  \"nbs_worker_commands_found\": [\"list of commands found\"],
  \"raw_log_access_detected\": true/false,
  \"raw_log_access_evidence\": [\"list of raw access patterns found\"] or [],
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

# Step 3: Report
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
