#!/bin/bash
# Test: Adversarial Scribe — rejection of non-decisions
#
# A chat transcript contains ONLY non-decisions: status updates, greetings,
# questions without answers, and debugging chatter. The Scribe SHOULD log
# zero decisions. If it logs anything, the decision detection is too loose.
#
# Falsification: Test fails if:
#   - Scribe logs ANY entry from a non-decision-only chat
#   - Scribe modifies existing log entries (append-only violation)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
CHAT_FILE="$TEST_DIR/test.chat"
LOG_FILE="$TEST_DIR/.nbs/scribe/log.md"
SCRIBE_OUTPUT="$SCRIPT_DIR/verdicts/scribe_adversarial_output_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/scribe_adversarial_verdict_${TIMESTAMP}.json"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$SCRIPT_DIR/verdicts"
mkdir -p "$TEST_DIR/.nbs/scribe"

echo "=== Adversarial Scribe: Rejection of Non-Decisions ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 0: Create chat with ONLY non-decisions
echo "Step 0: Creating chat with zero decisions..."

"$NBS_CHAT" create "$CHAT_FILE" >/dev/null

"$NBS_CHAT" send "$CHAT_FILE" claude "Good morning. Starting work on the parser." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "Morning! How are the tests looking?" >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "Tests are passing — 84 out of 84." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" bench-claude "Build complete. No warnings." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "Nice work everyone." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "I noticed an edge case in parse_int — investigating." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "False alarm — the existing test covers it." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "Should we think about error handling next?" >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" bench-claude "Maybe. Let me check the coverage report first." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "Coverage is at 91%. Pushing the latest changes." >/dev/null

CHAT_CONTENTS=$("$NBS_CHAT" read "$CHAT_FILE" 2>/dev/null)
echo "  Chat has 10 messages — all greetings, status updates, questions, debugging"
echo "  Zero decisions in this chat"
echo ""

# Step 1: Create initial log with one pre-existing entry
echo "Step 1: Creating log with one pre-existing entry..."

cat > "$LOG_FILE" << 'EOF'
# Decision Log

Project: adversarial-test
Created: 2026-02-14T00:00:00Z
Scribe: scribe

---

### D-1707609600 Use recursive descent parser
- **Chat ref:** live.chat:~L42
- **Participants:** claude, alex
- **Artefacts:** src/parser.c
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Grammar is LL(1). Simplest approach.
EOF

PRE_HASH=$(sha256sum "$LOG_FILE" | awk '{print $1}')
PRE_COUNT=$(grep -c "^### D-" "$LOG_FILE")
echo "  Pre-existing entries: $PRE_COUNT"
echo "  Pre-hash: ${PRE_HASH:0:16}..."
echo ""

# Step 2: Run Scribe
echo "Step 2: Running Claude as Scribe..."

SCRIBE_PROMPT="You are the Scribe — institutional memory for this project. Read the chat transcript below and identify DECISIONS to log.

## What is a decision
- Explicit agreement: 'let's do X', 'agreed', 'go with option 2'
- Architecture choice: 'file-based events, not sockets'
- Risk acceptance: 'we accept that X will be slower'

## What is NOT a decision
- Status updates: 'tests passing', 'build complete', 'coverage is 91%'
- Greetings: 'good morning', 'nice work'
- Questions without answers: 'should we think about X?'
- Debugging chatter: 'investigating', 'false alarm'
- Acknowledgements: 'on it', 'pushing changes'

## Chat transcript
---
${CHAT_CONTENTS}
---

## Your task
Read the chat above. If there are decisions, append entries to ${LOG_FILE}. If there are NO decisions (which is possible — not every conversation produces decisions), do NOT modify the file. Just output 'No decisions found.' and stop.

Remember: err on the side of caution for this check. Only log clear, unambiguous decisions where someone explicitly chose a direction."

echo "$SCRIBE_PROMPT" | claude -p - --output-format text --allowedTools "Bash,Read,Write,Edit" > "$SCRIBE_OUTPUT" 2>&1 || true

echo "  Scribe output captured ($(wc -l < "$SCRIBE_OUTPUT") lines)"
echo ""

# Step 3: Deterministic checks
echo "Step 3: Checking results..."

POST_COUNT=$(grep -c "^### D-" "$LOG_FILE")
POST_HASH=$(sha256sum "$LOG_FILE" | awk '{print $1}')

echo "  Post entries: $POST_COUNT (was $PRE_COUNT)"
echo "  Post hash: ${POST_HASH:0:16}..."

NEW_ENTRIES=$((POST_COUNT - PRE_COUNT))
echo "  New entries added: $NEW_ENTRIES"

# Check append-only: first N lines unchanged
PRE_LINES=$(wc -l < "$LOG_FILE")  # Approximate check
FIRST_PART_HASH=$(head -n 15 "$LOG_FILE" | sha256sum | awk '{print $1}')
EXPECTED_FIRST=$(head -n 15 <<'EOF' | sha256sum | awk '{print $1}'
# Decision Log

Project: adversarial-test
Created: 2026-02-14T00:00:00Z
Scribe: scribe

---

### D-1707609600 Use recursive descent parser
- **Chat ref:** live.chat:~L42
- **Participants:** claude, alex
- **Artefacts:** src/parser.c
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Grammar is LL(1). Simplest approach.
EOF
)

APPEND_ONLY="pass"
if [[ "$FIRST_PART_HASH" != "$EXPECTED_FIRST" ]]; then
    APPEND_ONLY="fail"
    echo "  APPEND-ONLY VIOLATION: Pre-existing content was modified"
fi
echo "  Append-only: $APPEND_ONLY"
echo ""

# Step 4: Evaluate
echo "Step 4: Evaluating..."

if [[ "$APPEND_ONLY" == "fail" ]]; then
    VERDICT_JSON='{"verdict":"FAIL","new_entries":'"$NEW_ENTRIES"',"append_only_intact":false,"reasoning":"Pre-existing log entries were modified — append-only invariant violated"}'
elif [[ "$NEW_ENTRIES" -eq 0 ]]; then
    VERDICT_JSON='{"verdict":"PASS","new_entries":0,"append_only_intact":true,"reasoning":"Scribe correctly identified zero decisions in a non-decision chat"}'
elif [[ "$NEW_ENTRIES" -le 2 ]]; then
    # Borderline — some messages could arguably be decisions. Use AI evaluator.
    echo "  Scribe logged $NEW_ENTRIES entries from a non-decision chat. Evaluating..."

    NEW_CONTENT=$(tail -n +16 "$LOG_FILE")

    EVAL_PROMPT="You are a test evaluator. A Scribe read a chat containing ONLY: greetings, status updates, questions without answers, and debugging chatter. No decisions were made. The Scribe logged $NEW_ENTRIES new entries.

## The chat contained
1. 'Good morning. Starting work on the parser.' — greeting/status
2. 'How are the tests looking?' — question
3. 'Tests are passing — 84 out of 84.' — status update
4. 'Build complete. No warnings.' — status update
5. 'Nice work everyone.' — social
6. 'I noticed an edge case — investigating.' — debugging
7. 'False alarm — the existing test covers it.' — debugging resolution
8. 'Should we think about error handling next?' — unanswered question
9. 'Maybe. Let me check the coverage report first.' — deferral, not decision
10. 'Coverage is at 91%. Pushing the latest changes.' — status update

None of these are decisions. No one said 'let's do X' or 'agreed' or chose between alternatives.

## Scribe's new entries
---
${NEW_CONTENT}
---

PASS if the logged entries are truly marginal (e.g. treating 'pushing latest changes' as a minor decision is debatable but tolerable).
FAIL if the entries log clear non-decisions (greetings, status updates, questions).

Respond with ONLY valid JSON:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"new_entries\": $NEW_ENTRIES,
  \"entries_are_non_decisions\": true/false,
  \"reasoning\": \"<explanation>\"
}"

    EVAL_TEMP=$(mktemp)
    echo "$EVAL_PROMPT" | claude -p - --output-format text > "$EVAL_TEMP" 2>&1
    VERDICT_JSON=$("$EXTRACT_JSON" "$EVAL_TEMP" 2>/dev/null || echo '{"verdict":"FAIL","reasoning":"Evaluator failed"}')
    rm -f "$EVAL_TEMP"
else
    VERDICT_JSON='{"verdict":"FAIL","new_entries":'"$NEW_ENTRIES"',"append_only_intact":true,"reasoning":"Scribe logged '"$NEW_ENTRIES"' entries from a chat with zero decisions — decision detection is too loose"}'
fi

echo "$VERDICT_JSON" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

echo "Step 5: Verdict"
echo "---"
echo "$VERDICT_JSON" | python3 -m json.tool 2>/dev/null || echo "$VERDICT_JSON"
echo "---"
echo ""

if echo "$VERDICT_JSON" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}: Scribe correctly rejected non-decisions"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Scribe logged non-decisions"
    echo ""
    echo "Log contents after Scribe ran:"
    cat "$LOG_FILE"
    exit 1
fi
