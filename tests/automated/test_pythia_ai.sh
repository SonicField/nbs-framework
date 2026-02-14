#!/bin/bash
# Test: Pythia produces valid checkpoint assessment
#
# Confirmational test: given a decision log, verify Pythia's output
# contains all 5 required assessment sections and cites D-timestamps.
#
# Uses AI evaluation for semantic judgement.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
PYTHIA_SKILL="$PROJECT_ROOT/claude_tools/nbs-pythia.md"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/pythia_ai_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/pythia_ai_verdict_$TIMESTAMP.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Pythia Checkpoint Assessment Test ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Set up scenario
echo "Step 1: Setting up test scenario..."
TEST_REPO=$(mktemp -d)
cd "$TEST_REPO" || exit 1

mkdir -p .nbs/scribe

# Create a decision log with enough content for meaningful assessment
cat > .nbs/scribe/log.md << 'LOGEOF'
# Decision Log

Project: web-api-refactor
Created: 2026-02-10T08:00:00Z
Scribe: scribe

---

### D-1707523200 Use REST instead of GraphQL for MVP
- **Chat ref:** live.chat:~L100
- **Participants:** claude, alex
- **Artefacts:** src/api/routes.py
- **Risk tags:** irreversible
- **Status:** decided
- **Rationale:** Team has REST experience. GraphQL migration possible later but endpoint contracts would change.

---

### D-1707526800 Skip authentication for internal endpoints
- **Chat ref:** live.chat:~L150
- **Participants:** claude, alex, doc-claude
- **Artefacts:** src/api/middleware.py
- **Risk tags:** untested, perf-risk
- **Status:** accepted-risk
- **Rationale:** Internal network only. Will add auth before any external exposure.

---

### D-1707530400 Use SQLite for development, PostgreSQL for production
- **Chat ref:** live.chat:~L200
- **Participants:** claude, bench-claude
- **Artefacts:** src/db/connection.py, config/database.yaml
- **Risk tags:** untested
- **Status:** decided
- **Rationale:** SQLite is simpler for dev. ORM abstraction should handle differences.

---

### D-1707534000 Defer input validation to post-MVP
- **Chat ref:** live.chat:~L250
- **Participants:** claude, alex
- **Artefacts:** —
- **Risk tags:** scope-creep, tech-debt
- **Status:** accepted-risk
- **Rationale:** Time pressure. Internal users only for now.

---

### D-1707537600 Use in-memory caching, no Redis
- **Chat ref:** live.chat:~L300
- **Participants:** claude
- **Artefacts:** src/cache/memory.py
- **Risk tags:** perf-risk, reversible
- **Status:** decided
- **Rationale:** Single-process deployment. Redis adds operational complexity for no benefit at current scale.
LOGEOF

# Step 2: Run Pythia
echo "Step 2: Running Pythia assessment prompt..."

PYTHIA_CONTENT=$(cat "$PYTHIA_SKILL")

PROMPT="You are Pythia — the oracle. You have been spawned to assess this project's trajectory.

Here is your role document:

---
$PYTHIA_CONTENT
---

The project is at $TEST_REPO. The decision log is below:

---
$(cat .nbs/scribe/log.md)
---

Produce your Pythia Checkpoint assessment now. Follow the exact format specified in your role document. This is assessment #1."

echo "$PROMPT" | claude -p - --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 3: Evaluate with pattern matching first
echo "Step 3: Checking structural requirements..."

OUTPUT=$(cat "$OUTPUT_FILE")
ISSUES=()

# Check for all 5 required sections
if ! echo "$OUTPUT" | grep -qi "hidden assumption"; then
    ISSUES+=("Missing 'Hidden Assumption' section")
fi

if ! echo "$OUTPUT" | grep -qi "second.order risk"; then
    ISSUES+=("Missing 'Second-Order Risk' section")
fi

if ! echo "$OUTPUT" | grep -qi "missing validation"; then
    ISSUES+=("Missing 'Missing Validation' section")
fi

if ! echo "$OUTPUT" | grep -qi "six.month regret"; then
    ISSUES+=("Missing 'Six-Month Regret' section")
fi

if ! echo "$OUTPUT" | grep -qi "confidence"; then
    ISSUES+=("Missing 'Confidence' section")
fi

# Check for D-timestamp citations
if ! echo "$OUTPUT" | grep -qE 'D-[0-9]{10}'; then
    ISSUES+=("No D-timestamp citations found")
fi

# Check confidence level is specified
if ! echo "$OUTPUT" | grep -qiE '(high|moderate|medium|low)'; then
    ISSUES+=("No confidence level (high/moderate/low) specified")
fi

# Step 4: AI evaluation for quality
echo "Step 4: AI evaluation of assessment quality..."

EVAL_PROMPT="You are evaluating a Pythia checkpoint assessment. The assessment was generated from a decision log containing 5 decisions about a web API project.

Here is the assessment:

---
$OUTPUT
---

Evaluate against these criteria:
1. Does it address all 5 sections: hidden assumptions, second-order risks, missing validation, six-month regret, confidence?
2. Does it cite specific decision entries by their D-timestamp?
3. Are the assessments specific and falsifiable (not vague)?
4. Does the confidence section give a level (high/moderate/low) with justification?
5. Does it identify real risks from the decision log (e.g., deferred auth, deferred validation, SQLite vs PostgreSQL differences, single-process caching)?

Respond with ONLY a JSON object:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"sections_present\": [list of sections found],
  \"citations_count\": number of D-timestamp citations,
  \"specificity\": \"high\" or \"medium\" or \"low\",
  \"reasoning\": \"brief explanation\"
}"

EVAL_OUTPUT=$(echo "$EVAL_PROMPT" | claude -p - --output-format text 2>/dev/null || echo '{"verdict":"FAIL","reasoning":"Evaluator failed to run"}')

# Try to extract JSON from evaluator output
EVAL_JSON=$(echo "$EVAL_OUTPUT" | python3 -c "
import sys, json, re
text = sys.stdin.read()
# Find JSON in the output
match = re.search(r'\{[^{}]*\}', text, re.DOTALL)
if match:
    try:
        obj = json.loads(match.group())
        print(json.dumps(obj))
    except:
        print(json.dumps({'verdict': 'FAIL', 'reasoning': 'Could not parse evaluator JSON'}))
else:
    print(json.dumps({'verdict': 'FAIL', 'reasoning': 'No JSON found in evaluator output'}))
" 2>/dev/null || echo '{"verdict":"FAIL","reasoning":"Python extraction failed"}')

# Combine structural and AI evaluation
STRUCTURAL_PASS=true
if [[ ${#ISSUES[@]} -gt 0 ]]; then
    STRUCTURAL_PASS=false
fi

AI_VERDICT=$(echo "$EVAL_JSON" | python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('verdict','FAIL'))" 2>/dev/null || echo "FAIL")

if [[ "$STRUCTURAL_PASS" == true ]] && [[ "$AI_VERDICT" == "PASS" ]]; then
    FINAL_VERDICT="PASS"
    FINAL_REASONING="Structural checks pass. AI evaluation: PASS."
elif [[ "$STRUCTURAL_PASS" == false ]]; then
    FINAL_VERDICT="FAIL"
    FINAL_REASONING="Structural issues: $(IFS='; '; echo "${ISSUES[*]}")"
else
    FINAL_VERDICT="FAIL"
    AI_REASONING=$(echo "$EVAL_JSON" | python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('reasoning','unknown'))" 2>/dev/null || echo "unknown")
    FINAL_REASONING="AI evaluation failed: $AI_REASONING"
fi

# Build final verdict JSON
ISSUES_JSON=$(printf '%s\n' "${ISSUES[@]}" 2>/dev/null | python3 -c "
import sys, json
lines = [l.strip() for l in sys.stdin if l.strip()]
print(json.dumps(lines))
" 2>/dev/null || echo '[]')

FINAL_JSON=$(python3 -c "
import json, sys
print(json.dumps({
    'verdict': '$FINAL_VERDICT',
    'structural_issues': $ISSUES_JSON,
    'ai_evaluation': $EVAL_JSON,
    'reasoning': $(python3 -c "import json, sys; print(json.dumps(sys.argv[1]))" "$FINAL_REASONING")
}, indent=2))
")

echo "$FINAL_JSON" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

# Clean up
rm -rf "$TEST_REPO"

# Report
echo "Step 5: Verdict"
echo "---"
echo "$FINAL_JSON" | python3 -m json.tool 2>/dev/null || echo "$FINAL_JSON"
echo "---"
echo ""

if [[ "$FINAL_VERDICT" == "PASS" ]]; then
    echo -e "${GREEN}TEST PASSED${NC}: Pythia produced valid checkpoint assessment"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Assessment quality check failed"
    if [[ ${#ISSUES[@]} -gt 0 ]]; then
        echo "Structural issues:"
        for issue in "${ISSUES[@]}"; do
            echo "  - $issue"
        done
    fi
    echo ""
    echo "First 30 lines of assessment:"
    head -30 "$OUTPUT_FILE"
    exit 1
fi
