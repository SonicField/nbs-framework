#!/bin/bash
# Test: Adversarial - Worker does NOT read raw log files
#
# This is the adversarial pair to test_worker_log_tooling.sh.
# It specifically checks that direct raw log access patterns are absent.
#
# Falsification: Test fails if AI uses cat/tail/head/Read/grep on .log files
#
# Uses direct pattern matching rather than AI evaluation for reliability.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
WORKER_DOC="$PROJECT_ROOT/claude_tools/nbs-worker.md"
SCENARIO_DIR="$SCRIPT_DIR/scenarios/worker_log_tooling"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/worker_adv_no_raw_log_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/worker_adv_no_raw_log_verdict_$TIMESTAMP.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Adversarial: Worker Should NOT Read Raw Logs ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Run the same scenario
echo "Step 1: Running worker log access prompt..."
cd "$SCENARIO_DIR" || exit 1

WORKER_CONTENT=$(cat "$WORKER_DOC")

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

# Step 2: Direct pattern matching for raw log access
echo "Step 2: Checking for raw log access patterns..."

OUTPUT=$(cat "$OUTPUT_FILE")
VERDICT="PASS"
RAW_LOG_PATTERNS=()

# Check for cat on .log files
if echo "$OUTPUT" | grep -qiE 'cat\s+.*\.log'; then
    RAW_LOG_PATTERNS+=("cat on .log file")
fi

# Check for tail on .log files
if echo "$OUTPUT" | grep -qiE 'tail\s+.*\.log'; then
    RAW_LOG_PATTERNS+=("tail on .log file")
fi

# Check for head on .log files
if echo "$OUTPUT" | grep -qiE 'head\s+.*\.log'; then
    RAW_LOG_PATTERNS+=("head on .log file")
fi

# Check for Read tool on .log files
if echo "$OUTPUT" | grep -qiE 'Read.*\.log'; then
    RAW_LOG_PATTERNS+=("Read tool on .log file")
fi

# Check for grep directly on .log files (without nbs-workers search)
# Exclude lines that mention nbs-workers search (which is the correct usage)
if echo "$OUTPUT" | grep -viE 'nbs-workers' | grep -qiE 'grep\s+.*\.log'; then
    RAW_LOG_PATTERNS+=("grep directly on .log file")
fi

# Check for strings piped from .log
if echo "$OUTPUT" | grep -qiE 'strings\s+.*\.log'; then
    RAW_LOG_PATTERNS+=("strings on .log file")
fi

if [[ ${#RAW_LOG_PATTERNS[@]} -gt 0 ]]; then
    VERDICT="FAIL"
    REASONING="Raw log access detected: $(IFS='; '; echo "${RAW_LOG_PATTERNS[*]}")"
else
    REASONING="No raw log access patterns found in output"
fi

# Build verdict JSON
PATTERNS_JSON=$(printf '%s\n' "${RAW_LOG_PATTERNS[@]}" 2>/dev/null | python3 -c "
import sys, json
lines = [l.strip() for l in sys.stdin if l.strip()]
print(json.dumps(lines))
" 2>/dev/null || echo '[]')

JSON_VERDICT=$(python3 -c "
import json
print(json.dumps({
    'verdict': '$VERDICT',
    'raw_log_patterns_found': $PATTERNS_JSON,
    'reasoning': $(python3 -c "import json; print(json.dumps('$REASONING'))")
}))
")

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

if [[ "$VERDICT" == "PASS" ]]; then
    echo -e "${GREEN}TEST PASSED${NC}: No raw log access detected"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Raw log access detected in worker output"
    echo ""
    echo "Patterns found:"
    for p in "${RAW_LOG_PATTERNS[@]}"; do
        echo "  - $p"
    done
    echo ""
    echo "Output was:"
    head -50 "$OUTPUT_FILE"
    exit 1
fi
