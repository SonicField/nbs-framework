#!/bin/bash
# Test: AI-based Scribe decision logging
#
# A Claude instance acts as Scribe, reading a chat transcript that contains
# both real decisions and non-decisions. An evaluator Claude then checks
# whether the Scribe logged the correct entries with proper schema.
#
# Falsification: Test fails if:
#   - Scribe logs non-decisions (status updates, greetings)
#   - Scribe misses real decisions
#   - Logged entries have incorrect or missing fields
#   - Entries lack the D-<timestamp> format

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
CHAT_FILE="$TEST_DIR/test.chat"
LOG_FILE="$TEST_DIR/.nbs/scribe/log.md"
SCRIBE_OUTPUT="$SCRIPT_DIR/verdicts/scribe_ai_output_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/scribe_ai_verdict_${TIMESTAMP}.json"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$SCRIPT_DIR/verdicts"
mkdir -p "$TEST_DIR/.nbs/scribe"
mkdir -p "$TEST_DIR/.nbs/events/processed"

echo "=== AI-Based Scribe Decision Logging Test ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 0: Create chat file with embedded decisions AND non-decisions
echo "Step 0: Setting up test chat with decisions and non-decisions..."

"$NBS_CHAT" create "$CHAT_FILE" >/dev/null

# Post a mix of messages: 3 real decisions, 3 non-decisions
"$NBS_CHAT" send "$CHAT_FILE" alex "Hello team, let's get started on the parser." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "Hi Alex. Ready to work." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "We need to decide: recursive descent or Pratt parsing for the expression parser?" >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "I recommend recursive descent. The grammar is LL(1) and Pratt adds complexity we do not need." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "Agreed, let's go with recursive descent." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "Tests are passing — 42 out of 42." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "Good. Now, should we use file-based events or a socket-based approach for the coordination bus?" >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" bench-claude "File-based is simpler and crash-recoverable. Sockets need a daemon." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "File-based it is. We accept that it will be slower than sockets for high-frequency events." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" claude "Build complete. Pushed to main." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" alex "@bench-claude can you handle the benchmarks? @claude you take the parser. We defer the optimiser to next sprint." >/dev/null
"$NBS_CHAT" send "$CHAT_FILE" bench-claude "On it." >/dev/null

CHAT_CONTENTS=$("$NBS_CHAT" read "$CHAT_FILE" 2>/dev/null)
MSG_COUNT=$(echo "$CHAT_CONTENTS" | grep -c '.' || true)
echo "  Chat has $MSG_COUNT messages (3 decisions + status/social)"
echo ""

# Step 1: Create initial log file
echo "Step 1: Creating initial Scribe log..."
cat > "$LOG_FILE" << 'EOF'
# Decision Log

Project: scribe-test
Created: 2026-02-14T00:00:00Z
Scribe: scribe

---
EOF

# Step 2: Have a Claude instance act as Scribe
echo "Step 2: Running Claude as Scribe..."

SCRIBE_PROMPT="You are the Scribe — institutional memory for this project. Your job is to read the chat transcript below and identify DECISIONS. Not every message is a decision. Log only moments where the team chose a direction.

## What is a decision
- Explicit agreement: 'let's do X', 'agreed', 'go with option 2'
- Architecture choice: 'file-based events, not sockets'
- Task assignment with scope change: 'you handle X, defer Y'
- Risk acceptance: 'we accept that X will be slower'

## What is NOT a decision
- Status updates: 'tests passing', 'build complete'
- Greetings: 'hello', 'ready to work'
- Acknowledgements without substance: 'on it', 'thanks'

## Chat transcript
---
${CHAT_CONTENTS}
---

## Your task
Read the chat above. Identify the decisions. For each decision, append an entry to the log file at ${LOG_FILE} using this EXACT format:

---

### D-<unix-timestamp> <one-line summary>
- **Chat ref:** test.chat:~L<approx-line>
- **Participants:** <handles>
- **Artefacts:** —
- **Risk tags:** <none or tags>
- **Status:** decided
- **Rationale:** <1-3 sentences>

Get the current unix timestamp with: date +%s
Append each entry to ${LOG_FILE} using cat >> or echo >>. Do NOT overwrite the file.

After logging all decisions, output the number of decisions you logged."

echo "$SCRIBE_PROMPT" | claude -p - --output-format text --allowedTools "Bash,Read,Write,Edit" > "$SCRIBE_OUTPUT" 2>&1 || true

echo "  Scribe output captured ($(wc -l < "$SCRIBE_OUTPUT") lines)"
echo ""

# Step 3: Deterministic checks
echo "Step 3: Deterministic checks on Scribe log..."

if [[ ! -f "$LOG_FILE" ]]; then
    echo -e "${RED}FAIL${NC}: Log file does not exist"
    exit 1
fi

DECISION_COUNT=$(grep -c "^### D-" "$LOG_FILE" || true)
echo "  Decisions logged: $DECISION_COUNT"

if [[ "$DECISION_COUNT" -lt 2 ]]; then
    echo -e "${RED}FAIL${NC}: Scribe logged fewer than 2 decisions (expected 3)"
    echo "Log contents:"
    cat "$LOG_FILE"
    exit 1
fi

if [[ "$DECISION_COUNT" -gt 6 ]]; then
    echo -e "${RED}FAIL${NC}: Scribe logged more than 6 entries (logging non-decisions)"
    echo "Log contents:"
    cat "$LOG_FILE"
    exit 1
fi

# Check schema fields are present in every entry
SCHEMA_OK="pass"
for field in "Chat ref:" "Participants:" "Risk tags:" "Status:" "Rationale:"; do
    FIELD_COUNT=$(grep -c "$field" "$LOG_FILE" || true)
    if [[ "$FIELD_COUNT" -lt "$DECISION_COUNT" ]]; then
        echo "  WARN: Field '$field' appears $FIELD_COUNT times but $DECISION_COUNT entries exist"
        SCHEMA_OK="fail"
    fi
done

echo "  Schema completeness: $SCHEMA_OK"
echo ""

# Step 4: Evaluator checks quality
echo "Step 4: Evaluating Scribe quality..."

LOG_CONTENTS=$(cat "$LOG_FILE")

EVAL_PROMPT="You are a test evaluator for the NBS Scribe system. The Scribe read a chat transcript and logged decisions. Your job is to evaluate whether it logged the RIGHT decisions with the correct format.

## The chat contained these messages (in order)
1. alex: 'Hello team, let's get started on the parser.' — GREETING, not a decision
2. claude: 'Hi Alex. Ready to work.' — GREETING, not a decision
3. alex: 'We need to decide: recursive descent or Pratt parsing?' — QUESTION
4. claude: 'I recommend recursive descent. The grammar is LL(1).' — RECOMMENDATION
5. alex: 'Agreed, let's go with recursive descent.' — DECISION #1: Use recursive descent
6. claude: 'Tests are passing — 42 out of 42.' — STATUS UPDATE, not a decision
7. alex: 'Should we use file-based events or sockets?' — QUESTION
8. bench-claude: 'File-based is simpler and crash-recoverable.' — RECOMMENDATION
9. alex: 'File-based it is. We accept slower for high-frequency.' — DECISION #2: Use file-based events (with accepted risk)
10. claude: 'Build complete. Pushed to main.' — STATUS UPDATE, not a decision
11. alex: '@bench-claude benchmarks, @claude parser. Defer optimiser.' — DECISION #3: Task assignment + scope deferral
12. bench-claude: 'On it.' — ACKNOWLEDGEMENT, not a decision

## Expected decisions (3)
- Decision about recursive descent parser
- Decision about file-based events (with risk acceptance note)
- Decision about task assignment and deferring optimiser

## Scribe's actual log
---
${LOG_CONTENTS}
---

## Evaluation criteria

PASS if:
- Scribe logged 2-4 entries (3 is ideal; 2 is acceptable if two were reasonably merged; 4 is acceptable if one decision was split)
- The log captures the recursive descent decision
- The log captures the file-based events decision
- The log does NOT contain entries for 'tests passing' or 'build complete' (those are status updates)
- Each entry has the D-<timestamp> header, Chat ref, Participants, Status, and Rationale fields
- Risk tags include something relevant for the file-based events decision (e.g. perf-risk, accepted-risk, or similar)

FAIL if:
- Scribe logged status updates or greetings as decisions
- Any required field is missing from entries
- The key decisions (recursive descent, file-based events) are not logged
- Entries lack D-<timestamp> format

Respond with ONLY valid JSON:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"decisions_logged\": <number>,
  \"has_recursive_descent\": true/false,
  \"has_file_based_events\": true/false,
  \"has_task_assignment\": true/false,
  \"logged_non_decisions\": true/false,
  \"schema_complete\": true/false,
  \"reasoning\": \"<brief explanation>\"
}"

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

echo "$JSON_VERDICT" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

# Step 5: Report
echo "Step 5: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}: Scribe correctly identified decisions from chat"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Scribe did not correctly log decisions"
    echo ""
    echo "Scribe output:"
    head -50 "$SCRIBE_OUTPUT"
    echo ""
    echo "Final log:"
    cat "$LOG_FILE"
    exit 1
fi
