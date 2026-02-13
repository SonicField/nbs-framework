#!/bin/bash
# Test: nbs-bus event queue with falsification tests
#
# Deterministic tests covering:
#   1.  Publish creates correctly named event file
#   2.  Publish creates correct YAML content
#   3.  Check lists events sorted by priority
#   4.  Check lists events sorted by timestamp within same priority
#   5.  Read displays event content
#   6.  Ack moves event to processed/
#   7.  Ack-all moves all events
#   8.  Prune respects size limit
#   9.  Status reports correct counts
#   10. Error codes: missing dir (2), missing event (3), bad args (4)
#   11. Priority validation: rejects invalid priority strings
#   12. Concurrent publishes: no data corruption
#   13. Binary integrity: ASSERT_MSG strings present
#   14. Help exits cleanly (exit code 0)
#   15. Prune with no processed dir (clean exit)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"

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

echo "=== nbs-bus Event Queue Test ==="
echo "Test dir: $TEST_DIR"
echo "Binary: $NBS_BUS"
echo ""

# --- Test 1: Publish creates correctly named event file ---
echo "1. Publish creates correctly named event file..."
EVENTS="$TEST_DIR/events1"
mkdir -p "$EVENTS/processed"
EVENT=$($NBS_BUS publish "$EVENTS" worker1 task-complete high "Test payload")

# Filename should match pattern: <digits>-worker1-task-complete-<digits>.event
if [[ "$EVENT" =~ ^[0-9]+-worker1-task-complete-[0-9]+\.event$ ]]; then
    check "Filename matches pattern" "pass"
else
    check "Filename matches pattern (got: $EVENT)" "fail"
fi

# File should exist
if [ -f "$EVENTS/$EVENT" ]; then
    check "Event file exists" "pass"
else
    check "Event file exists" "fail"
fi

echo ""

# --- Test 2: Publish creates correct YAML content ---
echo "2. Publish creates correct YAML content..."
CONTENT=$(cat "$EVENTS/$EVENT")

if echo "$CONTENT" | grep -q "^source: worker1$"; then
    check "Source field" "pass"
else
    check "Source field" "fail"
fi

if echo "$CONTENT" | grep -q "^type: task-complete$"; then
    check "Type field" "pass"
else
    check "Type field" "fail"
fi

if echo "$CONTENT" | grep -q "^priority: high$"; then
    check "Priority field" "pass"
else
    check "Priority field" "fail"
fi

if echo "$CONTENT" | grep -q "^timestamp: "; then
    check "Timestamp field" "pass"
else
    check "Timestamp field" "fail"
fi

if echo "$CONTENT" | grep -q "^dedup-key: worker1:task-complete$"; then
    check "Dedup-key field" "pass"
else
    check "Dedup-key field" "fail"
fi

if echo "$CONTENT" | grep -q "Test payload"; then
    check "Payload content" "pass"
else
    check "Payload content" "fail"
fi

echo ""

# --- Test 3: Check lists events sorted by priority ---
echo "3. Check lists events sorted by priority..."
EVENTS="$TEST_DIR/events3"
mkdir -p "$EVENTS/processed"

# Publish events with different priorities (sleep 1ms between to ensure ordering)
$NBS_BUS publish "$EVENTS" src1 low-event low "low" > /dev/null
sleep 0.01
$NBS_BUS publish "$EVENTS" src2 critical-event critical "critical" > /dev/null
sleep 0.01
$NBS_BUS publish "$EVENTS" src3 normal-event normal "normal" > /dev/null
sleep 0.01
$NBS_BUS publish "$EVENTS" src4 high-event high "high" > /dev/null

OUTPUT=$($NBS_BUS check "$EVENTS")

# First line should be critical, last should be low
FIRST_PRIORITY=$(echo "$OUTPUT" | head -1 | grep -o '\[.*\]' | tr -d '[]')
LAST_PRIORITY=$(echo "$OUTPUT" | tail -1 | grep -o '\[.*\]' | tr -d '[]')

if [[ "$FIRST_PRIORITY" == "critical" ]]; then
    check "Critical event first" "pass"
else
    check "Critical event first (got: $FIRST_PRIORITY)" "fail"
fi

if [[ "$LAST_PRIORITY" == "low" ]]; then
    check "Low event last" "pass"
else
    check "Low event last (got: $LAST_PRIORITY)" "fail"
fi

# Count total events
EVENT_COUNT=$(echo "$OUTPUT" | wc -l)
if [[ "$EVENT_COUNT" -eq 4 ]]; then
    check "All 4 events listed" "pass"
else
    check "All 4 events listed (got: $EVENT_COUNT)" "fail"
fi

echo ""

# --- Test 4: Check lists events sorted by timestamp within same priority ---
echo "4. Timestamp ordering within same priority..."
EVENTS="$TEST_DIR/events4"
mkdir -p "$EVENTS/processed"

E1=$($NBS_BUS publish "$EVENTS" first normal-a normal "first")
sleep 0.01
E2=$($NBS_BUS publish "$EVENTS" second normal-b normal "second")

OUTPUT=$($NBS_BUS check "$EVENTS")
FIRST_FILE=$(echo "$OUTPUT" | head -1 | awk '{print $2}')
SECOND_FILE=$(echo "$OUTPUT" | tail -1 | awk '{print $2}')

if [[ "$FIRST_FILE" == "$E1" ]]; then
    check "Oldest event first within same priority" "pass"
else
    check "Oldest event first within same priority" "fail"
fi

if [[ "$SECOND_FILE" == "$E2" ]]; then
    check "Newest event last within same priority" "pass"
else
    check "Newest event last within same priority" "fail"
fi

echo ""

# --- Test 5: Read displays event content ---
echo "5. Read displays event content..."
EVENTS="$TEST_DIR/events5"
mkdir -p "$EVENTS/processed"
EVENT=$($NBS_BUS publish "$EVENTS" reader test-read normal "Read test payload")

READ_OUTPUT=$($NBS_BUS read "$EVENTS" "$EVENT")

if echo "$READ_OUTPUT" | grep -q "^source: reader$"; then
    check "Read shows source" "pass"
else
    check "Read shows source" "fail"
fi

if echo "$READ_OUTPUT" | grep -q "Read test payload"; then
    check "Read shows payload" "pass"
else
    check "Read shows payload" "fail"
fi

echo ""

# --- Test 6: Ack moves event to processed/ ---
echo "6. Ack moves event to processed/..."
EVENTS="$TEST_DIR/events6"
mkdir -p "$EVENTS/processed"
EVENT=$($NBS_BUS publish "$EVENTS" acker ack-test normal "ack me")

# Verify file exists in events dir
if [ -f "$EVENTS/$EVENT" ]; then
    check "Event exists before ack" "pass"
else
    check "Event exists before ack" "fail"
fi

$NBS_BUS ack "$EVENTS" "$EVENT"

# Verify file moved to processed
if [ ! -f "$EVENTS/$EVENT" ] && [ -f "$EVENTS/processed/$EVENT" ]; then
    check "Event moved to processed after ack" "pass"
else
    check "Event moved to processed after ack" "fail"
fi

echo ""

# --- Test 7: Ack-all moves all events ---
echo "7. Ack-all moves all events..."
EVENTS="$TEST_DIR/events7"
mkdir -p "$EVENTS/processed"

$NBS_BUS publish "$EVENTS" w1 evt1 normal "one" > /dev/null
$NBS_BUS publish "$EVENTS" w2 evt2 high "two" > /dev/null
$NBS_BUS publish "$EVENTS" w3 evt3 low "three" > /dev/null

# Count pending before
BEFORE=$(ls "$EVENTS"/*.event 2>/dev/null | wc -l)

ACK_OUTPUT=$($NBS_BUS ack-all "$EVENTS")
AFTER=$(find "$EVENTS" -maxdepth 1 -name '*.event' | wc -l)
PROCESSED=$(find "$EVENTS/processed" -maxdepth 1 -name '*.event' | wc -l)

if [[ "$BEFORE" -eq 3 ]]; then
    check "3 events before ack-all" "pass"
else
    check "3 events before ack-all (got: $BEFORE)" "fail"
fi

if [[ "$AFTER" -eq 0 ]]; then
    check "0 events after ack-all" "pass"
else
    check "0 events after ack-all (got: $AFTER)" "fail"
fi

if [[ "$PROCESSED" -eq 3 ]]; then
    check "3 events in processed" "pass"
else
    check "3 events in processed (got: $PROCESSED)" "fail"
fi

if echo "$ACK_OUTPUT" | grep -q "Acknowledged 3 events"; then
    check "Ack-all reports count" "pass"
else
    check "Ack-all reports count" "fail"
fi

echo ""

# --- Test 8: Prune respects size limit ---
echo "8. Prune respects size limit..."
EVENTS="$TEST_DIR/events8"
mkdir -p "$EVENTS/processed"

# Create several processed events (each ~100 bytes)
for i in $(seq 1 20); do
    E=$($NBS_BUS publish "$EVENTS" pruner "prune-test-$i" normal "Payload data for prune test iteration $i")
    $NBS_BUS ack "$EVENTS" "$E"
done

BEFORE_COUNT=$(ls "$EVENTS/processed/"*.event | wc -l)
BEFORE_SIZE=$(du -sb "$EVENTS/processed/" | awk '{print $1}')

# Prune to 500 bytes — should delete most files
$NBS_BUS prune "$EVENTS" --max-bytes=500

AFTER_COUNT=$(ls "$EVENTS/processed/"*.event 2>/dev/null | wc -l || echo "0")
AFTER_SIZE=$(du -sb "$EVENTS/processed/" | awk '{print $1}')

if [[ "$BEFORE_COUNT" -eq 20 ]]; then
    check "20 processed events before prune" "pass"
else
    check "20 processed events before prune (got: $BEFORE_COUNT)" "fail"
fi

if [[ "$AFTER_COUNT" -lt "$BEFORE_COUNT" ]]; then
    check "Events pruned" "pass"
else
    check "Events pruned (before=$BEFORE_COUNT, after=$AFTER_COUNT)" "fail"
fi

# After-size should be under the limit (500 bytes) OR equal to a single file
# (if a single file exceeds 500 bytes, prune stops once it's deleted enough)
if [[ "$AFTER_SIZE" -le 700 ]]; then
    check "Size reduced near limit" "pass"
else
    check "Size reduced near limit (got: $AFTER_SIZE)" "fail"
fi

echo ""

# --- Test 9: Status reports correct counts ---
echo "9. Status reports correct counts..."
EVENTS="$TEST_DIR/events9"
mkdir -p "$EVENTS/processed"

$NBS_BUS publish "$EVENTS" s1 critical-evt critical "c" > /dev/null
$NBS_BUS publish "$EVENTS" s2 high-evt high "h" > /dev/null
$NBS_BUS publish "$EVENTS" s3 normal-evt normal "n" > /dev/null

STATUS=$($NBS_BUS status "$EVENTS")

if echo "$STATUS" | grep -q "Pending: 3 total"; then
    check "Pending count" "pass"
else
    check "Pending count" "fail"
fi

if echo "$STATUS" | grep -q "critical=1"; then
    check "Critical count" "pass"
else
    check "Critical count" "fail"
fi

if echo "$STATUS" | grep -q "high=1"; then
    check "High count" "pass"
else
    check "High count" "fail"
fi

if echo "$STATUS" | grep -q "normal=1"; then
    check "Normal count" "pass"
else
    check "Normal count" "fail"
fi

if echo "$STATUS" | grep -q "Oldest pending:"; then
    check "Oldest pending reported" "pass"
else
    check "Oldest pending reported" "fail"
fi

echo ""

# --- Test 10: Error codes ---
echo "10. Error codes..."

# Missing directory
set +e
$NBS_BUS check "$TEST_DIR/nonexistent" > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 2 ]]; then
    check "Missing dir returns exit 2" "pass"
else
    check "Missing dir returns exit 2 (got: $EXIT_CODE)" "fail"
fi

# Missing event file
EVENTS="$TEST_DIR/events10"
mkdir -p "$EVENTS/processed"
set +e
$NBS_BUS read "$EVENTS" "nonexistent.event" > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 3 ]]; then
    check "Missing event returns exit 3" "pass"
else
    check "Missing event returns exit 3 (got: $EXIT_CODE)" "fail"
fi

# Bad arguments (no command)
set +e
$NBS_BUS > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 4 ]]; then
    check "No args returns exit 4" "pass"
else
    check "No args returns exit 4 (got: $EXIT_CODE)" "fail"
fi

# Bad arguments (publish missing args)
set +e
$NBS_BUS publish > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 4 ]]; then
    check "Publish missing args returns exit 4" "pass"
else
    check "Publish missing args returns exit 4 (got: $EXIT_CODE)" "fail"
fi

# Ack on missing event
set +e
$NBS_BUS ack "$EVENTS" "nonexistent.event" > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 3 ]]; then
    check "Ack missing event returns exit 3" "pass"
else
    check "Ack missing event returns exit 3 (got: $EXIT_CODE)" "fail"
fi

echo ""

# --- Test 11: Priority validation ---
echo "11. Priority validation..."
EVENTS="$TEST_DIR/events11"
mkdir -p "$EVENTS/processed"

set +e
$NBS_BUS publish "$EVENTS" src1 evt invalid_priority > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 4 ]]; then
    check "Invalid priority rejected" "pass"
else
    check "Invalid priority rejected (got: $EXIT_CODE)" "fail"
fi

# Valid priorities should all work
for prio in critical high normal low; do
    set +e
    $NBS_BUS publish "$EVENTS" "src-$prio" "test-$prio" "$prio" "payload" > /dev/null 2>&1
    EXIT_CODE=$?
    set -e
    if [[ "$EXIT_CODE" -eq 0 ]]; then
        check "Priority '$prio' accepted" "pass"
    else
        check "Priority '$prio' accepted (exit: $EXIT_CODE)" "fail"
    fi
done

echo ""

# --- Test 12: Concurrent publishes ---
echo "12. Concurrent publishes (no corruption)..."
EVENTS="$TEST_DIR/events12"
mkdir -p "$EVENTS/processed"

# Publish 50 events concurrently
for i in $(seq 1 50); do
    $NBS_BUS publish "$EVENTS" "worker-$i" "concurrent-test" normal "payload $i" > /dev/null &
done
wait

# Count event files
EVENT_FILES=$(ls "$EVENTS"/*.event 2>/dev/null | wc -l)
if [[ "$EVENT_FILES" -eq 50 ]]; then
    check "All 50 concurrent events created" "pass"
else
    check "All 50 concurrent events created (got: $EVENT_FILES)" "fail"
fi

# Verify each file is valid YAML with required fields
VALID=0
for f in "$EVENTS"/*.event; do
    if grep -q "^source: " "$f" && grep -q "^type: " "$f" && grep -q "^priority: " "$f"; then
        VALID=$((VALID + 1))
    fi
done
if [[ "$VALID" -eq 50 ]]; then
    check "All 50 events are valid YAML" "pass"
else
    check "All 50 events are valid YAML (got: $VALID/50)" "fail"
fi

echo ""

# --- Test 13: Binary integrity (ASSERT_MSG strings present) ---
echo "13. Binary integrity..."
if strings "$NBS_BUS" | grep -q "ASSERT FAILED"; then
    check "ASSERT_MSG strings in binary" "pass"
else
    check "ASSERT_MSG strings in binary" "fail"
fi

echo ""

# --- Test 14: Help exits cleanly ---
echo "14. Help command..."
set +e
$NBS_BUS help > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 0 ]]; then
    check "Help exits with code 0" "pass"
else
    check "Help exits with code 0 (got: $EXIT_CODE)" "fail"
fi

echo ""

# --- Test 15: Prune with no processed directory ---
echo "15. Prune with no processed directory..."
EVENTS="$TEST_DIR/events15"
mkdir -p "$EVENTS"
# No processed/ subdirectory

set +e
PRUNE_OUTPUT=$($NBS_BUS prune "$EVENTS" 2>&1)
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 0 ]]; then
    check "Prune exits cleanly with no processed dir" "pass"
else
    check "Prune exits cleanly with no processed dir (got: $EXIT_CODE)" "fail"
fi

if echo "$PRUNE_OUTPUT" | grep -q "Pruned 0 events"; then
    check "Prune reports 0 events" "pass"
else
    check "Prune reports 0 events" "fail"
fi

echo ""

# --- Test 16: Publish without payload ---
echo "16. Publish without payload..."
EVENTS="$TEST_DIR/events16"
mkdir -p "$EVENTS/processed"

EVENT=$($NBS_BUS publish "$EVENTS" bare bare-event low)
CONTENT=$(cat "$EVENTS/$EVENT")

if echo "$CONTENT" | grep -q "^source: bare$"; then
    check "Source present" "pass"
else
    check "Source present" "fail"
fi

# Should NOT contain payload line
if echo "$CONTENT" | grep -q "^payload:"; then
    check "No payload line" "fail"
else
    check "No payload line" "pass"
fi

echo ""

# --- Test 17: Publish atomic (temp file cleaned up) ---
echo "17. No temp files left after publish..."
EVENTS="$TEST_DIR/events17"
mkdir -p "$EVENTS/processed"

$NBS_BUS publish "$EVENTS" atomic atomic-test normal "test" > /dev/null

TMP_FILES=$(find "$EVENTS" -maxdepth 1 -name '.tmp-*' | wc -l)
if [[ "$TMP_FILES" -eq 0 ]]; then
    check "No temp files after publish" "pass"
else
    check "No temp files after publish (found: $TMP_FILES)" "fail"
fi

echo ""

# --- Summary ---
echo "=== Results ==="
if [[ "$ERRORS" -eq 0 ]]; then
    echo "All tests passed."
    exit 0
else
    echo "$ERRORS test(s) FAILED."
    exit 1
fi
