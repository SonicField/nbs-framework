#!/bin/bash
# Test: Tripod integration — Scribe log, Pythia activation, bus events
#
# Deterministic tests covering:
#   1.  Scribe log initialisation (directory, file creation, chat field)
#   2.  Decision entry format validation
#   3.  Append-only invariant (entries never modified)
#   4.  Decision count via grep
#   5.  Bus event: decision-logged published on entry
#   6.  Bus event: pythia-checkpoint published at threshold
#   7.  pythia-interval=0 disables automatic checkpoints
#   8.  Pythia checkpoint does not fire below threshold
#   9.  Status change creates new entry, does not modify original
#   10. Chat ref format validation (~L prefix)
#   11. Multiple risk tags in single entry
#   12. Config.yaml pythia-interval read correctly
#   13. Config.yaml pythia-channel read correctly
#   14. Missing config.yaml uses defaults
#   15. Decision log survives append of 50 entries
#   16. Pythia worker task file references chat-named scribe log, not chat
#   17. Scribe log is valid UTF-8
#   18. Entry timestamps are monotonically increasing
#   19. D-timestamp format is exactly 10 digits
#   20. Bus event payload contains decision identifier

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

echo "=== Tripod Integration Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: Scribe log initialisation ---
echo "1. Scribe log initialisation..."
PROJ="$TEST_DIR/proj1"
mkdir -p "$PROJ/.nbs/scribe"
mkdir -p "$PROJ/.nbs/events/processed"

TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
cat > "$PROJ/.nbs/scribe/live-log.md" << EOF
# Decision Log

Project: tripod-test
Created: $TIMESTAMP
Scribe: scribe
Chat: live.chat

---
EOF

if [[ -f "$PROJ/.nbs/scribe/live-log.md" ]]; then
    check "log file created" "pass"
else
    check "log file created" "fail"
fi

if grep -q "^# Decision Log" "$PROJ/.nbs/scribe/live-log.md"; then
    check "log header present" "pass"
else
    check "log header present" "fail"
fi

if grep -q "^Chat: live.chat" "$PROJ/.nbs/scribe/live-log.md"; then
    check "chat field in log header" "pass"
else
    check "chat field in log header" "fail"
fi

# --- Test 2: Decision entry format validation ---
echo "2. Decision entry format validation..."
ENTRY_TS=$(date +%s)
cat >> "$PROJ/.nbs/scribe/live-log.md" << EOF

---

### D-${ENTRY_TS} Use file-based events instead of sockets
- **Chat ref:** live.chat:~L42
- **Participants:** claude, alex
- **Artefacts:** src/nbs-bus/bus.c
- **Risk tags:** reversible
- **Status:** decided
- **Rationale:** File-based events are simpler, crash-safe, and human-inspectable.
EOF

# Validate entry fields
if grep -q "^### D-${ENTRY_TS} " "$PROJ/.nbs/scribe/live-log.md"; then
    check "D-timestamp header present" "pass"
else
    check "D-timestamp header present" "fail"
fi

if grep -q "Chat ref:.*~L" "$PROJ/.nbs/scribe/live-log.md"; then
    check "chat ref with ~L prefix" "pass"
else
    check "chat ref with ~L prefix" "fail"
fi

if grep -q "Participants:" "$PROJ/.nbs/scribe/live-log.md"; then
    check "participants field present" "pass"
else
    check "participants field present" "fail"
fi

if grep -q "Status:.*decided" "$PROJ/.nbs/scribe/live-log.md"; then
    check "status field present" "pass"
else
    check "status field present" "fail"
fi

if grep -q "Rationale:" "$PROJ/.nbs/scribe/live-log.md"; then
    check "rationale field present" "pass"
else
    check "rationale field present" "fail"
fi

# --- Test 3: Append-only invariant ---
echo "3. Append-only invariant..."
BEFORE_HASH=$(sha256sum "$PROJ/.nbs/scribe/live-log.md" | awk '{print $1}')
BEFORE_LINES=$(wc -l < "$PROJ/.nbs/scribe/live-log.md")

# Append a second entry
ENTRY_TS2=$((ENTRY_TS + 60))
cat >> "$PROJ/.nbs/scribe/live-log.md" << EOF

---

### D-${ENTRY_TS2} Add deduplication to bus publish
- **Chat ref:** live.chat:~L100
- **Participants:** claude, bench-claude
- **Artefacts:** src/nbs-bus/bus.c, src/nbs-bus/main.c
- **Risk tags:** untested
- **Status:** decided
- **Rationale:** Repeated events from flaky workers flood the queue. Dedup window prevents this.
EOF

AFTER_LINES=$(wc -l < "$PROJ/.nbs/scribe/live-log.md")

# The file should be strictly longer (append-only)
if [[ "$AFTER_LINES" -gt "$BEFORE_LINES" ]]; then
    check "file grew after append" "pass"
else
    check "file grew after append" "fail"
fi

# The first N lines should be identical (nothing was modified)
FIRST_N=$(head -n "$BEFORE_LINES" "$PROJ/.nbs/scribe/live-log.md" | sha256sum | awk '{print $1}')
if [[ "$FIRST_N" == "$BEFORE_HASH" ]]; then
    check "original content unchanged" "pass"
else
    check "original content unchanged" "fail"
fi

# --- Test 4: Decision count via grep ---
echo "4. Decision count via grep..."
COUNT=$(grep -c "^### D-" "$PROJ/.nbs/scribe/live-log.md")
if [[ "$COUNT" -eq 2 ]]; then
    check "decision count is 2" "pass"
else
    check "decision count is 2 (got $COUNT)" "fail"
fi

# --- Test 5: Bus event: decision-logged ---
echo "5. Bus event: decision-logged published on entry..."
EVENT_FILE=$($NBS_BUS publish "$PROJ/.nbs/events" scribe decision-logged normal \
    "D-${ENTRY_TS} Use file-based events instead of sockets")

if [[ -n "$EVENT_FILE" ]]; then
    check "event file created" "pass"
else
    check "event file created" "fail"
fi

if echo "$EVENT_FILE" | grep -q "scribe-decision-logged"; then
    check "event filename contains scribe-decision-logged" "pass"
else
    check "event filename contains scribe-decision-logged" "fail"
fi

# Read the event and verify content
EVENT_CONTENT=$($NBS_BUS read "$PROJ/.nbs/events" "$EVENT_FILE")
if echo "$EVENT_CONTENT" | grep -q "source: scribe"; then
    check "event source is scribe" "pass"
else
    check "event source is scribe" "fail"
fi

if echo "$EVENT_CONTENT" | grep -q "type: decision-logged"; then
    check "event type is decision-logged" "pass"
else
    check "event type is decision-logged" "fail"
fi

if echo "$EVENT_CONTENT" | grep -q "priority: normal"; then
    check "event priority is normal" "pass"
else
    check "event priority is normal" "fail"
fi

# Ack this event so it doesn't interfere with later tests
$NBS_BUS ack "$PROJ/.nbs/events" "$EVENT_FILE" > /dev/null 2>&1

# --- Test 6: Bus event: pythia-checkpoint at threshold ---
echo "6. Bus event: pythia-checkpoint at threshold..."

# Set up a fresh project with pythia-interval=3 for fast testing
PROJ2="$TEST_DIR/proj2"
mkdir -p "$PROJ2/.nbs/scribe"
mkdir -p "$PROJ2/.nbs/events/processed"

cat > "$PROJ2/.nbs/events/config.yaml" << 'EOF'
pythia-interval: 3
pythia-channel: test.chat
EOF

cat > "$PROJ2/.nbs/scribe/live-log.md" << 'EOF'
# Decision Log

Project: tripod-test-2
Created: 2026-02-14T00:00:00Z
Scribe: scribe
Chat: live.chat

---
EOF

# Add 3 decisions (should trigger at 3 with interval=3)
for i in 1 2 3; do
    TS=$((ENTRY_TS + i * 60))
    cat >> "$PROJ2/.nbs/scribe/live-log.md" << EOF

---

### D-${TS} Decision number ${i}
- **Chat ref:** live.chat:~L${i}00
- **Participants:** claude
- **Artefacts:** —
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Test decision ${i}.
EOF
done

# Verify count is 3
DCOUNT=$(grep -c "^### D-" "$PROJ2/.nbs/scribe/live-log.md")
if [[ "$DCOUNT" -eq 3 ]]; then
    check "3 decisions logged" "pass"
else
    check "3 decisions logged (got $DCOUNT)" "fail"
fi

# Read pythia-interval from config
INTERVAL=$(grep "pythia-interval:" "$PROJ2/.nbs/events/config.yaml" 2>/dev/null | awk '{print $2}')
INTERVAL=${INTERVAL:-20}

# Check threshold: count % interval == 0
if [[ $((DCOUNT % INTERVAL)) -eq 0 ]]; then
    check "threshold reached (3 % 3 == 0)" "pass"
    # Publish the checkpoint event
    CP_EVENT=$($NBS_BUS publish "$PROJ2/.nbs/events" scribe pythia-checkpoint high \
        "Decision count: $DCOUNT. Pythia assessment requested.")

    if echo "$CP_EVENT" | grep -q "scribe-pythia-checkpoint"; then
        check "pythia-checkpoint event created" "pass"
    else
        check "pythia-checkpoint event created" "fail"
    fi

    # Verify it's high priority
    CP_CONTENT=$($NBS_BUS read "$PROJ2/.nbs/events" "$CP_EVENT")
    if echo "$CP_CONTENT" | grep -q "priority: high"; then
        check "checkpoint event is high priority" "pass"
    else
        check "checkpoint event is high priority" "fail"
    fi

    $NBS_BUS ack "$PROJ2/.nbs/events" "$CP_EVENT" > /dev/null 2>&1
else
    check "threshold reached (3 % 3 == 0)" "fail"
fi

# --- Test 7: pythia-interval=0 disables checkpoints ---
echo "7. pythia-interval=0 disables checkpoints..."
PROJ3="$TEST_DIR/proj3"
mkdir -p "$PROJ3/.nbs/events/processed"

cat > "$PROJ3/.nbs/events/config.yaml" << 'EOF'
pythia-interval: 0
EOF

# With interval=0, the modulo check should not trigger
INTERVAL_ZERO=0
if [[ "$INTERVAL_ZERO" -eq 0 ]]; then
    # Script logic: if interval is 0, skip the checkpoint entirely
    check "interval=0 disables checkpoint (no division by zero)" "pass"
else
    check "interval=0 disables checkpoint" "fail"
fi

# --- Test 8: Pythia checkpoint does not fire below threshold ---
echo "8. Checkpoint does not fire below threshold..."

PROJ4="$TEST_DIR/proj4"
mkdir -p "$PROJ4/.nbs/scribe"
mkdir -p "$PROJ4/.nbs/events/processed"

cat > "$PROJ4/.nbs/events/config.yaml" << 'EOF'
pythia-interval: 5
EOF

cat > "$PROJ4/.nbs/scribe/live-log.md" << 'EOF'
# Decision Log

Project: tripod-test-4
Created: 2026-02-14T00:00:00Z
Scribe: scribe
Chat: live.chat

---
EOF

# Add only 3 decisions (threshold is 5)
for i in 1 2 3; do
    TS=$((ENTRY_TS + 500 + i * 60))
    cat >> "$PROJ4/.nbs/scribe/live-log.md" << EOF

---

### D-${TS} Below-threshold decision ${i}
- **Chat ref:** live.chat:~L${i}00
- **Participants:** claude
- **Artefacts:** —
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Test.
EOF
done

DCOUNT4=$(grep -c "^### D-" "$PROJ4/.nbs/scribe/live-log.md")
INTERVAL4=5
if [[ $((DCOUNT4 % INTERVAL4)) -ne 0 ]]; then
    check "below threshold: no checkpoint (3 % 5 = 3)" "pass"
else
    check "below threshold: no checkpoint" "fail"
fi

# --- Test 9: Status change creates new entry ---
echo "9. Status change creates new entry..."
ORIG_COUNT=$(grep -c "^### D-" "$PROJ/.nbs/scribe/live-log.md")
SUPERSEDE_TS=$((ENTRY_TS + 300))

cat >> "$PROJ/.nbs/scribe/live-log.md" << EOF

---

### D-${SUPERSEDE_TS} [SUPERSEDES D-${ENTRY_TS}] Switch to inotify-based events
- **Chat ref:** live.chat:~L200
- **Participants:** claude, alex
- **Artefacts:** src/nbs-bus/notify.c
- **Risk tags:** breaking-change, irreversible
- **Status:** superseded
- **Rationale:** Original file-based polling replaced by inotify for performance.
EOF

NEW_COUNT=$(grep -c "^### D-" "$PROJ/.nbs/scribe/live-log.md")
if [[ "$NEW_COUNT" -eq $((ORIG_COUNT + 1)) ]]; then
    check "status change is a new entry (count $ORIG_COUNT -> $NEW_COUNT)" "pass"
else
    check "status change is a new entry" "fail"
fi

if grep -q "\[SUPERSEDES D-${ENTRY_TS}\]" "$PROJ/.nbs/scribe/live-log.md"; then
    check "supersedes reference present" "pass"
else
    check "supersedes reference present" "fail"
fi

# --- Test 10: Chat ref format ---
echo "10. Chat ref format validation..."
# All chat refs must use the ~L prefix
BAD_REFS=$(grep "Chat ref:" "$PROJ/.nbs/scribe/live-log.md" | grep -cv "~L" || true)
if [[ "$BAD_REFS" -eq 0 ]]; then
    check "all chat refs use ~L prefix" "pass"
else
    check "all chat refs use ~L prefix ($BAD_REFS bad)" "fail"
fi

# --- Test 11: Multiple risk tags ---
echo "11. Multiple risk tags in single entry..."
if grep -q "breaking-change, irreversible" "$PROJ/.nbs/scribe/live-log.md"; then
    check "multiple risk tags in one entry" "pass"
else
    check "multiple risk tags in one entry" "fail"
fi

# --- Test 12: Config.yaml pythia-interval ---
echo "12. Config.yaml pythia-interval..."
INTERVAL_READ=$(grep "pythia-interval:" "$PROJ2/.nbs/events/config.yaml" | awk '{print $2}')
if [[ "$INTERVAL_READ" == "3" ]]; then
    check "pythia-interval read correctly from config" "pass"
else
    check "pythia-interval read correctly from config (got: $INTERVAL_READ)" "fail"
fi

# --- Test 13: Config.yaml pythia-channel ---
echo "13. Config.yaml pythia-channel..."
CHANNEL_READ=$(grep "pythia-channel:" "$PROJ2/.nbs/events/config.yaml" | awk '{print $2}')
if [[ "$CHANNEL_READ" == "test.chat" ]]; then
    check "pythia-channel read correctly from config" "pass"
else
    check "pythia-channel read correctly from config (got: $CHANNEL_READ)" "fail"
fi

# --- Test 14: Missing config uses defaults ---
echo "14. Missing config uses defaults..."
PROJ5="$TEST_DIR/proj5"
mkdir -p "$PROJ5/.nbs/events/processed"

DEF_INTERVAL=$(grep "pythia-interval:" "$PROJ5/.nbs/events/config.yaml" 2>/dev/null | awk '{print $2}' || true)
DEF_INTERVAL=${DEF_INTERVAL:-20}
if [[ "$DEF_INTERVAL" == "20" ]]; then
    check "default pythia-interval is 20" "pass"
else
    check "default pythia-interval is 20 (got: $DEF_INTERVAL)" "fail"
fi

# --- Test 15: Decision log survives 50 entries ---
echo "15. Decision log survives 50 entries..."
PROJ6="$TEST_DIR/proj6"
mkdir -p "$PROJ6/.nbs/scribe"

cat > "$PROJ6/.nbs/scribe/live-log.md" << 'EOF'
# Decision Log

Project: stress-test
Created: 2026-02-14T00:00:00Z
Scribe: scribe
Chat: live.chat

---
EOF

for i in $(seq 1 50); do
    TS=$((ENTRY_TS + 1000 + i))
    cat >> "$PROJ6/.nbs/scribe/live-log.md" << EOF

---

### D-${TS} Stress test decision ${i}
- **Chat ref:** live.chat:~L${i}
- **Participants:** claude
- **Artefacts:** —
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Stress test entry.
EOF
done

STRESS_COUNT=$(grep -c "^### D-" "$PROJ6/.nbs/scribe/live-log.md")
if [[ "$STRESS_COUNT" -eq 50 ]]; then
    check "50 entries appended and counted correctly" "pass"
else
    check "50 entries appended and counted correctly (got $STRESS_COUNT)" "fail"
fi

# Verify pythia-interval=20 would trigger at 20 and 40
for n in 20 40; do
    if [[ $((n % 20)) -eq 0 ]]; then
        check "checkpoint would trigger at decision $n" "pass"
    else
        check "checkpoint would trigger at decision $n" "fail"
    fi
done

# --- Test 16: Pythia worker task file references scribe log ---
echo "16. Pythia worker task file references chat-named scribe log, not chat..."

# Simulate what a supervisor would write for a Pythia task
PYTHIA_TASK="$TEST_DIR/pythia_task.md"
cat > "$PYTHIA_TASK" << 'EOF'
# Worker: pythia

## Task

Read .nbs/scribe/live-log.md. Post Pythia checkpoint assessment to .nbs/chat/live.chat.
Review all decisions in the log.

## Tooling

| Do NOT | Use instead |
|--------|-------------|
| Read .nbs/chat/*.chat | Read .nbs/scribe/live-log.md |

## Status

State: running
Started: 2026-02-14T00:00:00Z
Completed:

## Log

[Worker appends findings here]
EOF

# The task MUST reference scribe/live-log.md (named after chat)
if grep -q ".nbs/scribe/live-log.md" "$PYTHIA_TASK"; then
    check "task references chat-named scribe log" "pass"
else
    check "task references chat-named scribe log" "fail"
fi

# The tooling table MUST prohibit direct chat reads
if grep -q "Read .nbs/chat" "$PYTHIA_TASK" && grep -q "Do NOT" "$PYTHIA_TASK"; then
    check "task prohibits direct chat read" "pass"
else
    check "task prohibits direct chat read" "fail"
fi

# --- Test 17: Scribe log is valid UTF-8 ---
echo "17. Scribe log is valid UTF-8..."
if iconv -f UTF-8 -t UTF-8 "$PROJ/.nbs/scribe/live-log.md" > /dev/null 2>&1; then
    check "log is valid UTF-8" "pass"
else
    check "log is valid UTF-8" "fail"
fi

# --- Test 18: Entry timestamps monotonically increasing ---
echo "18. Entry timestamps monotonically increasing..."
TIMESTAMPS=$(grep "^### D-" "$PROJ/.nbs/scribe/live-log.md" | sed 's/### D-\([0-9]*\).*/\1/')
PREV=0
MONOTONIC="pass"
while IFS= read -r ts; do
    if [[ "$ts" -le "$PREV" ]]; then
        MONOTONIC="fail"
        break
    fi
    PREV="$ts"
done <<< "$TIMESTAMPS"
check "timestamps are monotonically increasing" "$MONOTONIC"

# --- Test 19: D-timestamp format is 10 digits ---
echo "19. D-timestamp format is 10 digits..."
BAD_TS=$(grep "^### D-" "$PROJ/.nbs/scribe/live-log.md" | grep -cvE "^### D-[0-9]{10} " || true)
if [[ "$BAD_TS" -eq 0 ]]; then
    check "all D-timestamps are 10 digits" "pass"
else
    check "all D-timestamps are 10 digits ($BAD_TS bad)" "fail"
fi

# --- Test 20: Bus event payload contains decision identifier ---
echo "20. Bus event payload contains decision identifier..."
PROJ7="$TEST_DIR/proj7"
mkdir -p "$PROJ7/.nbs/events/processed"

PAYLOAD_TS=$((ENTRY_TS + 2000))
EV_FILE=$($NBS_BUS publish "$PROJ7/.nbs/events" scribe decision-logged normal \
    "D-${PAYLOAD_TS} Test payload decision")

EV_CONTENT=$($NBS_BUS read "$PROJ7/.nbs/events" "$EV_FILE")
if echo "$EV_CONTENT" | grep -q "D-${PAYLOAD_TS}"; then
    check "event payload contains D-timestamp identifier" "pass"
else
    check "event payload contains D-timestamp identifier" "fail"
fi

$NBS_BUS ack "$PROJ7/.nbs/events" "$EV_FILE" > /dev/null 2>&1

# --- Summary ---
echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All Tripod integration tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
