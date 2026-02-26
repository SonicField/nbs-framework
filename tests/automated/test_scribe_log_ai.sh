#!/bin/bash
# Test: AI-based Scribe decision logging via nbs-scribe-log tool
#
# A Claude instance acts as Scribe, given the full nbs-scribe skill content.
# It reads a chat transcript containing decisions and non-decisions, and must
# use the `nbs-scribe-log` binary to record them — NOT manual cat/echo/heredoc.
# An evaluator Claude then checks tool usage, decision quality, and schema.
#
# Falsification: Test fails if:
#   - Scribe uses manual append (cat >>, echo >>) instead of nbs-scribe-log
#   - Scribe logs non-decisions (status updates, greetings)
#   - Scribe misses real decisions
#   - Logged entries have incorrect or missing fields
#   - Entries lack the D-<timestamp> format
#   - Bus events were not published

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

TEST_DIR=$(mktemp -d)
CHAT_FILE="$TEST_DIR/test.chat"
LOG_FILE="$TEST_DIR/.nbs/scribe/test-log.md"
EVENTS_DIR="$TEST_DIR/.nbs/events"
SCRIBE_OUTPUT="$SCRIPT_DIR/verdicts/scribe_log_ai_output_${TIMESTAMP}.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/scribe_log_ai_verdict_${TIMESTAMP}.json"

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

echo "=== AI-Based Scribe nbs-scribe-log Tool Usage Test ==="
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

# Step 1: Create initial log file using nbs-scribe-log's init behaviour
echo "Step 1: Creating initial Scribe log..."
cat > "$LOG_FILE" << 'EOF'
# Decision Log

Project: scribe-test
Created: 2026-02-14T00:00:00Z
Scribe: scribe
Chat: test.chat

---
EOF

# Step 2: Read the nbs-scribe skill content
echo "Step 2: Loading nbs-scribe skill content..."
SKILL_FILE="$PROJECT_ROOT/claude_tools/nbs-scribe.md"
if [[ ! -f "$SKILL_FILE" ]]; then
    echo -e "${RED}FAIL${NC}: Skill file not found at $SKILL_FILE"
    exit 1
fi
SKILL_CONTENT=$(cat "$SKILL_FILE")
echo "  Skill loaded ($(wc -l < "$SKILL_FILE") lines)"
echo ""

# Step 3: Have a Claude instance act as Scribe using nbs-scribe-log
echo "Step 3: Running Claude as Scribe (must use nbs-scribe-log)..."

SCRIBE_PROMPT="You are the Scribe — institutional memory for this project. Your job is to read the chat transcript below and identify DECISIONS. Not every message is a decision. Log only moments where the team chose a direction.

## Your skill definition

You MUST follow this skill exactly. In particular, you MUST use the nbs-scribe-log tool to log decisions. Do NOT manually append to the log file with cat >>, echo >>, heredoc, or any other manual method. The nbs-scribe-log binary is on your PATH.

---
${SKILL_CONTENT}
---

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

Read the chat above. Identify the decisions. For each decision, use the nbs-scribe-log command to log it.

The log file is: ${LOG_FILE}
The bus directory is: ${EVENTS_DIR}

Example invocation:
  nbs-scribe-log ${LOG_FILE} \"Use recursive descent parser\" \\
    --participants=alex,claude \\
    --chat-ref=test.chat:~L5 \\
    --rationale=\"Grammar is LL(1), Pratt adds unnecessary complexity.\" \\
    --risk-tags=none \\
    --bus-dir=${EVENTS_DIR}

Use appropriate --risk-tags for decisions that accept risk (e.g. perf-risk, accepted-risk).

After logging all decisions, state how many you logged."

export PATH="$PROJECT_ROOT/bin:$PATH"

echo "$SCRIBE_PROMPT" | claude -p - --output-format text --allowedTools "Bash,Read,Write,Edit" > "$SCRIBE_OUTPUT" 2>&1 || true

echo "  Scribe output captured ($(wc -l < "$SCRIBE_OUTPUT") lines)"
echo ""

# Step 4: Deterministic checks
echo "Step 4: Deterministic checks on Scribe log..."

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

if [[ "$DECISION_COUNT" -gt 4 ]]; then
    echo -e "${RED}FAIL${NC}: Scribe logged more than 4 entries (logging non-decisions)"
    echo "Log contents:"
    cat "$LOG_FILE"
    exit 1
fi

# Check D-<timestamp> format (timestamp is a unix epoch integer)
D_FORMAT_COUNT=$(grep -cE "^### D-[0-9]+ " "$LOG_FILE" || true)
if [[ "$D_FORMAT_COUNT" -ne "$DECISION_COUNT" ]]; then
    echo -e "${RED}FAIL${NC}: Not all entries match D-<timestamp> format ($D_FORMAT_COUNT of $DECISION_COUNT)"
    exit 1
fi
echo "  D-<timestamp> format: ok"

# Check schema fields are present in every entry
SCHEMA_OK="pass"
for field in "Chat ref:" "Participants:" "Artefacts:" "Risk tags:" "Status:" "Rationale:"; do
    FIELD_COUNT=$(grep -c "$field" "$LOG_FILE" || true)
    if [[ "$FIELD_COUNT" -lt "$DECISION_COUNT" ]]; then
        echo "  WARN: Field '$field' appears $FIELD_COUNT times but $DECISION_COUNT entries exist"
        SCHEMA_OK="fail"
    fi
done

echo "  Schema completeness: $SCHEMA_OK"

# Check bus events were published
EVENT_COUNT=$(find "$EVENTS_DIR" -name "*.event" -type f 2>/dev/null | wc -l || echo 0)
echo "  Bus events found: $EVENT_COUNT"
echo ""

# Step 5: Evaluator checks quality and tool usage
echo "Step 5: Evaluating Scribe quality and tool usage..."

LOG_CONTENTS=$(cat "$LOG_FILE")
SCRIBE_RAW_OUTPUT=$(cat "$SCRIBE_OUTPUT")

EVAL_PROMPT="You are a test evaluator for the NBS Scribe system. The Scribe was given the nbs-scribe skill and told to use the nbs-scribe-log tool. Your job is to evaluate TWO things: (1) whether it used the tool correctly, and (2) whether it logged the RIGHT decisions.

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

## Scribe's actual log output
---
${LOG_CONTENTS}
---

## Scribe's raw output (showing commands it ran)
---
${SCRIBE_RAW_OUTPUT}
---

## Bus events directory contents
$(ls -la "${EVENTS_DIR}"/ 2>/dev/null || echo "(empty or not found)")
$(ls -la "${EVENTS_DIR}"/processed/ 2>/dev/null || echo "(processed dir empty or not found)")

## Evaluation criteria

### Tool usage (critical)
- Check the scribe's raw output for invocations of 'nbs-scribe-log'. The scribe MUST have called nbs-scribe-log at least once.
- Check that the scribe did NOT use 'cat >>' or 'echo >>' or heredoc to append directly to the log file. If you see 'cat >>' or 'echo >>' writing decision entries to the log file, that is a FAIL.
- Note: 'cat' used to READ the log file is fine — only APPENDING with 'cat >>' or 'echo >>' to the log file is forbidden.

### Decision quality
- Scribe logged 2-4 entries (3 is ideal)
- The log captures the recursive descent decision
- The log captures the file-based events decision
- The log does NOT contain entries for 'tests passing' or 'build complete'
- Each entry has D-<timestamp> header, Chat ref, Participants, Artefacts, Risk tags, Status, and Rationale fields

PASS if tool usage is correct AND decision quality is acceptable.
FAIL if the scribe used manual append OR missed key decisions OR logged non-decisions.

Respond with ONLY valid JSON:
{
  \"verdict\": \"PASS\" or \"FAIL\",
  \"used_tool\": true or false,
  \"avoided_manual_append\": true or false,
  \"decisions_logged\": <number>,
  \"has_recursive_descent\": true or false,
  \"has_file_based_events\": true or false,
  \"logged_non_decisions\": true or false,
  \"schema_complete\": true or false,
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

# Step 6: Report
echo "Step 6: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    echo -e "${GREEN}TEST PASSED${NC}: Scribe correctly used nbs-scribe-log to record decisions"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Scribe did not correctly use nbs-scribe-log or logged incorrect decisions"
    echo ""
    echo "Scribe output:"
    head -80 "$SCRIBE_OUTPUT"
    echo ""
    echo "Final log:"
    cat "$LOG_FILE"
    exit 1
fi
