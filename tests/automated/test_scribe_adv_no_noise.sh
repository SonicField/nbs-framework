#!/bin/bash
# Test: Adversarial - Scribe does NOT log non-decisions
#
# Given a chat excerpt containing only status updates, greetings,
# and questions without resolution, the Scribe should NOT create
# any decision entries.
#
# Falsification: Test fails if AI produces any D-<timestamp> entries
# for the given non-decision conversation.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SCRIBE_SKILL="$PROJECT_ROOT/claude_tools/nbs-scribe.md"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/scribe_adv_no_noise_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/scribe_adv_no_noise_verdict_$TIMESTAMP.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Adversarial: Scribe Should NOT Log Non-Decisions ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Set up scenario with only non-decision chat messages
echo "Step 1: Setting up test scenario..."

SCRIBE_CONTENT=$(cat "$SCRIBE_SKILL")

# Chat excerpt with NO decisions — only status updates, greetings, questions
CHAT_EXCERPT="claude: Hello! Starting work on the parser module today.
alex: Good morning. How are things going?
claude: Tests are passing — 84/84 green. Making progress on the tokenizer.
bench-claude: Running benchmarks now, will report back shortly.
alex: Thanks, keep me posted.
claude: Found a potential issue in the lexer. Investigating.
doc-claude: I've updated the README with the new API examples.
claude: The lexer issue turned out to be a false alarm — test was using wrong input.
alex: Great. What should we work on next?
claude: I think we should look at either the optimizer or the code generator. Both need attention.
alex: Let me think about it and get back to you."

PROMPT="You are the Scribe — the institutional memory of this project. Here is your role document:

---
$SCRIBE_CONTENT
---

Here is a recent chat excerpt. Your job is to identify any DECISIONS in this conversation and produce the corresponding decision log entries.

Chat excerpt:
---
$CHAT_EXCERPT
---

If you find decisions, output them in the standard D-<timestamp> format.
If you find NO decisions, state clearly that no decisions were found and explain why.

Be precise. Only log actual decisions — explicit choices between alternatives, risk acceptances, architecture changes, scope changes, or reversals."

echo "$PROMPT" | claude -p - --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 2: Check for spurious decision entries
echo "Step 2: Checking for spurious decision entries..."

OUTPUT=$(cat "$OUTPUT_FILE")
VERDICT="PASS"

# Count D-timestamp entries in the output
DECISION_COUNT=$(echo "$OUTPUT" | grep -cE '### D-[0-9]{10}' || true)

# Also check for entries that use the log format but with different timestamp format
ALT_DECISION_COUNT=$(echo "$OUTPUT" | grep -cE '^### D-' || true)

# Check if the AI explicitly says no decisions were found
SAYS_NO_DECISIONS=false
if echo "$OUTPUT" | grep -qiE '(no decisions|no.*decision.*found|none.*decisions|did not find.*decision|nothing.*qualifies)'; then
    SAYS_NO_DECISIONS=true
fi

if [[ "$DECISION_COUNT" -gt 0 ]] || [[ "$ALT_DECISION_COUNT" -gt 0 ]]; then
    VERDICT="FAIL"
    REASONING="Scribe logged $DECISION_COUNT decision entries from a conversation containing no decisions. Chat had only status updates, greetings, and unresolved questions."
elif [[ "$SAYS_NO_DECISIONS" == true ]]; then
    REASONING="Scribe correctly identified that no decisions were present in the chat excerpt."
else
    REASONING="No decision entries produced. Scribe did not explicitly state 'no decisions found' but also did not produce spurious entries."
fi

# Build verdict JSON
JSON_VERDICT=$(python3 -c "
import json, sys
print(json.dumps({
    'verdict': '$VERDICT',
    'decision_entries_found': $DECISION_COUNT,
    'alt_entries_found': $ALT_DECISION_COUNT,
    'says_no_decisions': $SAYS_NO_DECISIONS,
    'reasoning': $(python3 -c "import json, sys; print(json.dumps(sys.argv[1]))" "$REASONING")
}, indent=2))
")

echo "$JSON_VERDICT" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

# Report
echo "Step 3: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if [[ "$VERDICT" == "PASS" ]]; then
    echo -e "${GREEN}TEST PASSED${NC}: Scribe did not produce spurious decision entries"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Scribe logged non-decisions"
    echo ""
    echo "Entries found in output:"
    echo "$OUTPUT" | grep -E '### D-' || true
    echo ""
    echo "Full output:"
    cat "$OUTPUT_FILE"
    exit 1
fi
