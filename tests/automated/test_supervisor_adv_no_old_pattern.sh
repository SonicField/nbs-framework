#!/bin/bash
# Test: Adversarial - Supervisor does NOT use old pty-session spawn pattern
#
# This is the adversarial pair to test_supervisor_nbs_worker.sh.
# It specifically checks that the old workaround pattern is absent.
#
# Falsification: Test fails if AI produces temp.sh, raw pty-session create/send
#                sequence, or manually creates task files before spawning.
#
# This uses direct pattern matching rather than AI evaluation for reliability.
# (See testing-strategy.md: "Direct Pattern Matching vs AI Evaluation")

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SUPERVISOR_DOC="$PROJECT_ROOT/claude_tools/nbs-supervisor.md"
SCENARIO_DIR="$SCRIPT_DIR/scenarios/supervisor_nbs_worker"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/supervisor_adv_no_old_pattern_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/supervisor_adv_no_old_pattern_verdict_$TIMESTAMP.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Adversarial: Supervisor Should NOT Use Old Pattern ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Run the same prompt
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

# Step 2: Direct pattern matching for old pattern markers
echo "Step 2: Checking for old pattern markers..."

OUTPUT=$(cat "$OUTPUT_FILE")
VERDICT="PASS"
REASONING=""
OLD_PATTERNS_FOUND=()

# Check for temp.sh workaround
if echo "$OUTPUT" | grep -qi "temp\.sh"; then
    OLD_PATTERNS_FOUND+=("temp.sh workaround detected")
fi

# Check for raw pty-session create used for worker spawning
# (pty-session create is fine for REPLs, but not for spawning Claude workers)
if echo "$OUTPUT" | grep -qi "pty-session create.*claude"; then
    OLD_PATTERNS_FOUND+=("pty-session create with claude detected")
fi

# Check for the old send-task-prompt pattern
if echo "$OUTPUT" | grep -qi "pty-session send.*Read.*\.nbs/workers"; then
    OLD_PATTERNS_FOUND+=("pty-session send with task file prompt detected")
fi

# Check for manual worker file creation before spawn
# (nbs-workers creates this automatically)
if echo "$OUTPUT" | grep -qi "cat >.*\.nbs/workers.*\.md" || \
   echo "$OUTPUT" | grep -qi "cat >.*worker-[0-9]"; then
    OLD_PATTERNS_FOUND+=("manual task file creation before spawn detected")
fi

# Check for old worker-NNN naming convention
if echo "$OUTPUT" | grep -qi "worker-[0-9][0-9][0-9]"; then
    OLD_PATTERNS_FOUND+=("old worker-NNN naming convention detected")
fi

if [[ ${#OLD_PATTERNS_FOUND[@]} -gt 0 ]]; then
    VERDICT="FAIL"
    REASONING="Old pty-session spawn pattern detected: $(IFS='; '; echo "${OLD_PATTERNS_FOUND[*]}")"
else
    REASONING="No old pattern markers found in output"
fi

# Build verdict JSON
PATTERNS_JSON=$(printf '%s\n' "${OLD_PATTERNS_FOUND[@]}" 2>/dev/null | python3 -c "
import sys, json
lines = [l.strip() for l in sys.stdin if l.strip()]
print(json.dumps(lines))
" 2>/dev/null || echo '[]')

JSON_VERDICT=$(python3 -c "
import json
print(json.dumps({
    'verdict': '$VERDICT',
    'old_patterns_found': $PATTERNS_JSON,
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
    echo -e "${GREEN}TEST PASSED${NC}: Old pattern correctly absent"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Old pattern detected in supervisor output"
    echo ""
    echo "Patterns found:"
    for p in "${OLD_PATTERNS_FOUND[@]}"; do
        echo "  - $p"
    done
    echo ""
    echo "Output was:"
    head -50 "$OUTPUT_FILE"
    exit 1
fi
