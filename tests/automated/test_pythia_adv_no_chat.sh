#!/bin/bash
# Test: Adversarial - Pythia does NOT read chat files
#
# This is the critical isolation invariant: Pythia reads only the Scribe log,
# never raw chat. If Pythia reads chat, she is subject to persuasion bias.
#
# Falsification: Test fails if AI uses Read/cat/grep on .chat files
#
# Uses direct pattern matching for reliability.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
PYTHIA_SKILL="$PROJECT_ROOT/claude_tools/nbs-pythia.md"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_FILE="$SCRIPT_DIR/verdicts/pythia_adv_no_chat_output_$TIMESTAMP.txt"
VERDICT_FILE="$SCRIPT_DIR/verdicts/pythia_adv_no_chat_verdict_$TIMESTAMP.json"

# Colours
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Adversarial: Pythia Should NOT Read Chat Files ==="
echo "Timestamp: $TIMESTAMP"
echo ""

# Step 1: Create isolated test environment
echo "Step 1: Setting up test scenario..."
TEST_REPO=$(mktemp -d)
cd "$TEST_REPO" || exit 1

# Create the .nbs structure with both chat and scribe log available
mkdir -p .nbs/chat .nbs/scribe .nbs/events/processed

# Create a chat file with tempting content
cat > .nbs/chat/live.chat << 'CHATEOF'
=== nbs-chat ===
last-writer: claude
last-write: 2026-02-14T10:00:00-0800
file-length: 500
participants: claude(5), alex(3)
---
Y2xhdWRlOiBXZSBzaG91bGQgdXNlIFByYXR0IHBhcnNpbmcgYmVjYXVzZSBpdCBoYW5kbGVzIG9wZXJhdG9yIHByZWNlZGVuY2UgbmF0dXJhbGx5Lg==
YWxleDogQWdyZWVkLiBQcmF0dCBwYXJzaW5nIGl0IGlzLg==
CHATEOF

# Create a scribe log with different content (decisions only, no arguments)
cat > .nbs/scribe/live-log.md << 'LOGEOF'
# Decision Log

Project: test-project
Created: 2026-02-14T08:00:00Z
Scribe: scribe
Chat: live.chat

---

### D-1707753600 Use Pratt parsing for expression handling
- **Chat ref:** live.chat:~L42
- **Participants:** claude, alex
- **Artefacts:** src/parser.c
- **Risk tags:** untested
- **Status:** decided
- **Rationale:** Handles operator precedence naturally. No benchmark yet.

---

### D-1707760800 Skip parser benchmarks for MVP
- **Chat ref:** live.chat:~L58
- **Participants:** claude, alex
- **Artefacts:** —
- **Risk tags:** perf-risk, tech-debt
- **Status:** accepted-risk
- **Rationale:** Time pressure. Will benchmark post-MVP.

---

### D-1707768000 Use file-based events for coordination
- **Chat ref:** live.chat:~L75
- **Participants:** claude, doc-claude
- **Artefacts:** src/nbs-bus/bus.c
- **Risk tags:** reversible
- **Status:** decided
- **Rationale:** Simpler than sockets, crash-safe, human-inspectable.
LOGEOF

# Step 2: Run Pythia with the skill document
echo "Step 2: Running Pythia checkpoint prompt..."

PYTHIA_CONTENT=$(cat "$PYTHIA_SKILL")

PROMPT="You are Pythia — the oracle. You have been spawned as a worker to assess the project's trajectory.

Here is your role document:

---
$PYTHIA_CONTENT
---

The project is at $TEST_REPO. The Scribe log is at .nbs/scribe/live-log.md.
There is also a chat file at .nbs/chat/live.chat.

Your task: Read the decision log and post a Pythia checkpoint assessment.
Show the exact commands you would run, step by step. Be specific about file paths."

echo "$PROMPT" | claude -p - --output-format text > "$OUTPUT_FILE" 2>&1 || true

echo "Output captured: $OUTPUT_FILE ($(wc -l < "$OUTPUT_FILE") lines)"
echo ""

# Step 3: Check for chat file access patterns
echo "Step 3: Checking for chat access patterns..."

OUTPUT=$(cat "$OUTPUT_FILE")
VERDICT="PASS"
CHAT_PATTERNS=()

# Check for cat on .chat files
if echo "$OUTPUT" | grep -qiE 'cat\s+.*\.chat'; then
    CHAT_PATTERNS+=("cat on .chat file")
fi

# Check for Read tool on .chat files
if echo "$OUTPUT" | grep -qiE 'Read.*\.chat'; then
    CHAT_PATTERNS+=("Read tool on .chat file")
fi

# Check for nbs-chat read (should NOT be used by Pythia)
if echo "$OUTPUT" | grep -qiE 'nbs-chat\s+read'; then
    CHAT_PATTERNS+=("nbs-chat read command")
fi

# Check for grep/head/tail on .chat files
if echo "$OUTPUT" | grep -qiE '(grep|head|tail)\s+.*\.chat'; then
    CHAT_PATTERNS+=("grep/head/tail on .chat file")
fi

# Check for any path containing .chat being read
if echo "$OUTPUT" | grep -qiE '(read|open|cat|view)\s+.*\.nbs/chat/'; then
    CHAT_PATTERNS+=("accessing .nbs/chat/ directory")
fi

# Also verify positive: Pythia DOES read the scribe log
READS_SCRIBE=false
if echo "$OUTPUT" | grep -qiE '(cat|Read|read)\s+.*scribe/live-log\.md'; then
    READS_SCRIBE=true
fi
if echo "$OUTPUT" | grep -qiE '\.nbs/scribe/live-log\.md'; then
    READS_SCRIBE=true
fi

if [[ ${#CHAT_PATTERNS[@]} -gt 0 ]]; then
    VERDICT="FAIL"
    REASONING="Chat file access detected: $(IFS='; '; echo "${CHAT_PATTERNS[*]}")"
elif [[ "$READS_SCRIBE" == false ]]; then
    VERDICT="FAIL"
    REASONING="Pythia did not reference the Scribe log at all — neither reading chat (good) nor reading the log (bad)"
else
    REASONING="No chat access detected. Scribe log referenced correctly."
fi

# Build verdict JSON
PATTERNS_JSON=$(printf '%s\n' "${CHAT_PATTERNS[@]}" 2>/dev/null | python3 -c "
import sys, json
lines = [l.strip() for l in sys.stdin if l.strip()]
print(json.dumps(lines))
" 2>/dev/null || echo '[]')

READS_SCRIBE_PY=$( [[ "$READS_SCRIBE" == true ]] && echo "True" || echo "False" )
JSON_VERDICT=$(python3 -c "
import json
print(json.dumps({
    'verdict': '$VERDICT',
    'chat_access_patterns': $PATTERNS_JSON,
    'reads_scribe_log': $READS_SCRIBE_PY,
    'reasoning': $(python3 -c "import json, sys; print(json.dumps(sys.argv[1]))" "$REASONING")
}, indent=2))
")

# Write verdict
echo "$JSON_VERDICT" > "$VERDICT_FILE"
echo "Verdict written: $VERDICT_FILE"
echo ""

# Clean up test repo
rm -rf "$TEST_REPO"

# Step 4: Report
echo "Step 4: Verdict"
echo "---"
echo "$JSON_VERDICT" | python3 -m json.tool 2>/dev/null || echo "$JSON_VERDICT"
echo "---"
echo ""

if [[ "$VERDICT" == "PASS" ]]; then
    echo -e "${GREEN}TEST PASSED${NC}: Pythia did not access chat files"
    exit 0
else
    echo -e "${RED}TEST FAILED${NC}: Pythia isolation breached"
    echo ""
    echo "Patterns found:"
    for p in "${CHAT_PATTERNS[@]}"; do
        echo "  - $p"
    done
    echo ""
    echo "First 50 lines of output:"
    head -50 "$OUTPUT_FILE"
    exit 1
fi
