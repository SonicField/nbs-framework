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
#   16. Publish without payload
#   17. No temp files left after publish
#   18. Dedup drops duplicate within window (exit 5)
#   19. Dedup allows different source:type keys
#   20. Dedup: same type, different source is not a duplicate
#   21. No --dedup-window: duplicates allowed
#   22. Dedup ignores acked (processed) events
#   23. Invalid --dedup-window values rejected
#   24. Config.yaml sets default dedup-window
#   25. Config.yaml sets retention-max-bytes
#   26. CLI args override config.yaml
#   27. Missing config.yaml uses defaults
#   28. Invalid config.yaml values ignored
#   29. Config.yaml with comments and unknown keys
#   30. Config.yaml with extremely long line (>512 bytes)
#   31. Config.yaml with binary content
#   32. Config.yaml without trailing newline
#   33. Config.yaml with empty/whitespace values
#   34. Config.yaml with numeric overflow values
#   35. Config.yaml with bare colons and odd formatting

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
# grep the binary directly — avoids pipe with strings which can flake
ASSERT_COUNT=$(grep -c "ASSERT FAILED" "$NBS_BUS" || true)
if [[ "$ASSERT_COUNT" -gt 0 ]]; then
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

# --- Test 18: Dedup drops duplicate within window ---
echo "18. Dedup drops duplicate within window..."
EVENTS="$TEST_DIR/events18"
mkdir -p "$EVENTS/processed"

# First publish should succeed
E1=$($NBS_BUS publish "$EVENTS" dedup-src dedup-type normal "first" --dedup-window=300)
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "First publish succeeds" "pass"
else
    check "First publish succeeds (exit: $EXIT1)" "fail"
fi

# Second publish with same source:type within 300s window should be deduplicated
set +e
E2=$($NBS_BUS publish "$EVENTS" dedup-src dedup-type normal "second" --dedup-window=300 2>/dev/null)
EXIT2=$?
set -e
if [[ "$EXIT2" -eq 5 ]]; then
    check "Duplicate dropped (exit code 5)" "pass"
else
    check "Duplicate dropped (exit code 5, got: $EXIT2)" "fail"
fi

# Only one event file should exist
EVENT_COUNT=$(find "$EVENTS" -maxdepth 1 -name '*.event' | wc -l)
if [[ "$EVENT_COUNT" -eq 1 ]]; then
    check "Only one event file exists" "pass"
else
    check "Only one event file exists (got: $EVENT_COUNT)" "fail"
fi

echo ""

# --- Test 19: Dedup allows different keys ---
echo "19. Dedup allows different source:type keys..."
EVENTS="$TEST_DIR/events19"
mkdir -p "$EVENTS/processed"

E1=$($NBS_BUS publish "$EVENTS" src-a type-a normal "first" --dedup-window=300)
EXIT1=$?
E2=$($NBS_BUS publish "$EVENTS" src-b type-b normal "second" --dedup-window=300)
EXIT2=$?

if [[ "$EXIT1" -eq 0 && "$EXIT2" -eq 0 ]]; then
    check "Different keys both published" "pass"
else
    check "Different keys both published (exit1=$EXIT1, exit2=$EXIT2)" "fail"
fi

EVENT_COUNT=$(find "$EVENTS" -maxdepth 1 -name '*.event' | wc -l)
if [[ "$EVENT_COUNT" -eq 2 ]]; then
    check "Two event files exist" "pass"
else
    check "Two event files exist (got: $EVENT_COUNT)" "fail"
fi

echo ""

# --- Test 20: Dedup allows same key with different source ---
echo "20. Dedup: same type, different source is not a duplicate..."
EVENTS="$TEST_DIR/events20"
mkdir -p "$EVENTS/processed"

$NBS_BUS publish "$EVENTS" worker-a task-done normal "first" --dedup-window=300 > /dev/null
E2=$($NBS_BUS publish "$EVENTS" worker-b task-done normal "second" --dedup-window=300)
EXIT2=$?

if [[ "$EXIT2" -eq 0 ]]; then
    check "Different source, same type: not deduplicated" "pass"
else
    check "Different source, same type: not deduplicated (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 21: Dedup without --dedup-window allows duplicates ---
echo "21. No --dedup-window: duplicates allowed..."
EVENTS="$TEST_DIR/events21"
mkdir -p "$EVENTS/processed"

$NBS_BUS publish "$EVENTS" dup-src dup-type normal "first" > /dev/null
E2=$($NBS_BUS publish "$EVENTS" dup-src dup-type normal "second")
EXIT2=$?

if [[ "$EXIT2" -eq 0 ]]; then
    check "Without dedup window, duplicate allowed" "pass"
else
    check "Without dedup window, duplicate allowed (exit: $EXIT2)" "fail"
fi

EVENT_COUNT=$(find "$EVENTS" -maxdepth 1 -name '*.event' | wc -l)
if [[ "$EVENT_COUNT" -eq 2 ]]; then
    check "Two event files exist" "pass"
else
    check "Two event files exist (got: $EVENT_COUNT)" "fail"
fi

echo ""

# --- Test 22: Dedup ignores acked (processed) events ---
echo "22. Dedup ignores acked events..."
EVENTS="$TEST_DIR/events22"
mkdir -p "$EVENTS/processed"

# Publish and ack
E1=$($NBS_BUS publish "$EVENTS" acked-src acked-type normal "will be acked" --dedup-window=300)
$NBS_BUS ack "$EVENTS" "$E1"

# Same key should now be allowed (previous event is in processed/)
E2=$($NBS_BUS publish "$EVENTS" acked-src acked-type normal "after ack" --dedup-window=300)
EXIT2=$?

if [[ "$EXIT2" -eq 0 ]]; then
    check "After ack, same key publishes again" "pass"
else
    check "After ack, same key publishes again (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 23: Dedup with invalid window value ---
echo "23. Invalid --dedup-window value rejected..."
EVENTS="$TEST_DIR/events23"
mkdir -p "$EVENTS/processed"

set +e
$NBS_BUS publish "$EVENTS" src typ normal "payload" --dedup-window=abc > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 4 ]]; then
    check "Non-numeric dedup-window rejected (exit 4)" "pass"
else
    check "Non-numeric dedup-window rejected (exit 4, got: $EXIT_CODE)" "fail"
fi

set +e
$NBS_BUS publish "$EVENTS" src typ normal "payload" --dedup-window=0 > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 4 ]]; then
    check "Zero dedup-window rejected (exit 4)" "pass"
else
    check "Zero dedup-window rejected (exit 4, got: $EXIT_CODE)" "fail"
fi

set +e
$NBS_BUS publish "$EVENTS" src typ normal "payload" --dedup-window=-5 > /dev/null 2>&1
EXIT_CODE=$?
set -e
if [[ "$EXIT_CODE" -eq 4 ]]; then
    check "Negative dedup-window rejected (exit 4)" "pass"
else
    check "Negative dedup-window rejected (exit 4, got: $EXIT_CODE)" "fail"
fi

echo ""

# --- Test 24: Config.yaml sets default dedup-window ---
echo "24. Config.yaml sets default dedup-window..."
EVENTS="$TEST_DIR/events24"
mkdir -p "$EVENTS/processed"

# Write config.yaml with dedup-window: 300
cat > "$EVENTS/config.yaml" <<'YAML'
# Bus configuration
dedup-window: 300
YAML

# First publish succeeds (no --dedup-window flag — config provides it)
E1=$($NBS_BUS publish "$EVENTS" cfg-src cfg-type normal "first")
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "First publish with config dedup succeeds" "pass"
else
    check "First publish with config dedup succeeds (exit: $EXIT1)" "fail"
fi

# Second publish should be deduplicated (config sets 300s window)
set +e
E2=$($NBS_BUS publish "$EVENTS" cfg-src cfg-type normal "second" 2>/dev/null)
EXIT2=$?
set -e
if [[ "$EXIT2" -eq 5 ]]; then
    check "Config dedup-window causes dedup (exit 5)" "pass"
else
    check "Config dedup-window causes dedup (exit 5, got: $EXIT2)" "fail"
fi

echo ""

# --- Test 25: Config.yaml sets retention-max-bytes ---
echo "25. Config.yaml sets retention-max-bytes..."
EVENTS="$TEST_DIR/events25"
mkdir -p "$EVENTS/processed"

# Write config.yaml with small retention limit
cat > "$EVENTS/config.yaml" <<'YAML'
retention-max-bytes: 500
YAML

# Create processed events
for i in $(seq 1 20); do
    E=$($NBS_BUS publish "$EVENTS" pruner "cfg-prune-$i" normal "Payload for config prune test $i")
    $NBS_BUS ack "$EVENTS" "$E"
done

BEFORE_COUNT=$(ls "$EVENTS/processed/"*.event | wc -l)

# Prune without --max-bytes — should use config value (500)
$NBS_BUS prune "$EVENTS"

AFTER_COUNT=$(ls "$EVENTS/processed/"*.event 2>/dev/null | wc -l || echo "0")

if [[ "$BEFORE_COUNT" -eq 20 ]]; then
    check "20 processed events before config prune" "pass"
else
    check "20 processed events before config prune (got: $BEFORE_COUNT)" "fail"
fi

if [[ "$AFTER_COUNT" -lt "$BEFORE_COUNT" ]]; then
    check "Config retention-max-bytes causes pruning" "pass"
else
    check "Config retention-max-bytes causes pruning (before=$BEFORE_COUNT, after=$AFTER_COUNT)" "fail"
fi

echo ""

# --- Test 26: CLI args override config.yaml ---
echo "26. CLI args override config.yaml..."
EVENTS="$TEST_DIR/events26"
mkdir -p "$EVENTS/processed"

# Config says dedup-window: 300, but CLI says --dedup-window=1
cat > "$EVENTS/config.yaml" <<'YAML'
dedup-window: 300
YAML

$NBS_BUS publish "$EVENTS" override-src override-type normal "first" > /dev/null
sleep 0.01

# CLI --dedup-window=1 should still catch it (within 1 second)
set +e
$NBS_BUS publish "$EVENTS" override-src override-type normal "second" --dedup-window=1 2>/dev/null
EXIT2=$?
set -e
if [[ "$EXIT2" -eq 5 ]]; then
    check "CLI --dedup-window overrides config" "pass"
else
    check "CLI --dedup-window overrides config (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 27: Missing config.yaml uses defaults ---
echo "27. Missing config.yaml uses defaults..."
EVENTS="$TEST_DIR/events27"
mkdir -p "$EVENTS/processed"
# No config.yaml

# Without config and without --dedup-window, duplicates should be allowed (default=0)
$NBS_BUS publish "$EVENTS" noconf-src noconf-type normal "first" > /dev/null
E2=$($NBS_BUS publish "$EVENTS" noconf-src noconf-type normal "second")
EXIT2=$?

if [[ "$EXIT2" -eq 0 ]]; then
    check "No config, no CLI flag: duplicates allowed" "pass"
else
    check "No config, no CLI flag: duplicates allowed (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 28: Config.yaml with invalid values uses defaults ---
echo "28. Invalid config.yaml values ignored..."
EVENTS="$TEST_DIR/events28"
mkdir -p "$EVENTS/processed"

cat > "$EVENTS/config.yaml" <<'YAML'
retention-max-bytes: notanumber
dedup-window: -5
YAML

# Should work normally (invalid values are ignored, defaults used)
E1=$($NBS_BUS publish "$EVENTS" inv-src inv-type normal "test")
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "Invalid config values ignored, publish works" "pass"
else
    check "Invalid config values ignored, publish works (exit: $EXIT1)" "fail"
fi

# No dedup should happen (invalid dedup-window → default 0)
E2=$($NBS_BUS publish "$EVENTS" inv-src inv-type normal "test2")
EXIT2=$?
if [[ "$EXIT2" -eq 0 ]]; then
    check "Invalid dedup-window → default 0, no dedup" "pass"
else
    check "Invalid dedup-window → default 0, no dedup (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 29: Config.yaml with comments and extra keys ---
echo "29. Config.yaml with comments and unknown keys..."
EVENTS="$TEST_DIR/events29"
mkdir -p "$EVENTS/processed"

cat > "$EVENTS/config.yaml" <<'YAML'
# This is a comment
retention-max-bytes: 8388608
unknown-key: whatever
dedup-window: 60

# Another comment
YAML

# Should work — unknown keys silently ignored
E1=$($NBS_BUS publish "$EVENTS" comment-src comment-type normal "test")
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "Config with comments and unknown keys works" "pass"
else
    check "Config with comments and unknown keys works (exit: $EXIT1)" "fail"
fi

# Second publish should be deduplicated (dedup-window: 60 from config)
set +e
E2=$($NBS_BUS publish "$EVENTS" comment-src comment-type normal "test2" 2>/dev/null)
EXIT2=$?
set -e
if [[ "$EXIT2" -eq 5 ]]; then
    check "Config dedup-window from commented config works" "pass"
else
    check "Config dedup-window from commented config works (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 30: Config.yaml with extremely long lines ---
echo "30. Config.yaml with extremely long line..."
EVENTS="$TEST_DIR/events30"
mkdir -p "$EVENTS/processed"

# Create a config with a line longer than the 512-byte buffer
LONG_VALUE=$(python3 -c "print('x' * 1000)")
cat > "$EVENTS/config.yaml" <<YAML
dedup-window: 60
retention-max-bytes: $LONG_VALUE
YAML

# Should handle gracefully — long line truncated, valid key still works
E1=$($NBS_BUS publish "$EVENTS" long-src long-type normal "test")
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "Long config line handled safely" "pass"
else
    check "Long config line handled safely (exit: $EXIT1)" "fail"
fi

# dedup-window: 60 should still work
set +e
E2=$($NBS_BUS publish "$EVENTS" long-src long-type normal "test2" 2>/dev/null)
EXIT2=$?
set -e
if [[ "$EXIT2" -eq 5 ]]; then
    check "Valid key before long line still works" "pass"
else
    check "Valid key before long line still works (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 31: Config.yaml with binary content ---
echo "31. Config.yaml with binary content..."
EVENTS="$TEST_DIR/events31"
mkdir -p "$EVENTS/processed"

# Write binary garbage followed by a valid key
printf '\x00\x01\x02\xff\xfe\n' > "$EVENTS/config.yaml"
echo "dedup-window: 60" >> "$EVENTS/config.yaml"

E1=$($NBS_BUS publish "$EVENTS" bin-src bin-type normal "test")
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "Binary config content handled safely" "pass"
else
    check "Binary config content handled safely (exit: $EXIT1)" "fail"
fi

echo ""

# --- Test 32: Config.yaml with no trailing newline ---
echo "32. Config.yaml without trailing newline..."
EVENTS="$TEST_DIR/events32"
mkdir -p "$EVENTS/processed"

# Write config without trailing newline
printf 'dedup-window: 60' > "$EVENTS/config.yaml"

E1=$($NBS_BUS publish "$EVENTS" nonl-src nonl-type normal "first")
EXIT1=$?
set +e
E2=$($NBS_BUS publish "$EVENTS" nonl-src nonl-type normal "second" 2>/dev/null)
EXIT2=$?
set -e

if [[ "$EXIT1" -eq 0 ]]; then
    check "No-newline config: first publish works" "pass"
else
    check "No-newline config: first publish works (exit: $EXIT1)" "fail"
fi
if [[ "$EXIT2" -eq 5 ]]; then
    check "No-newline config: dedup-window parsed correctly" "pass"
else
    check "No-newline config: dedup-window parsed correctly (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 33: Config.yaml with empty/whitespace values ---
echo "33. Config.yaml with empty and whitespace values..."
EVENTS="$TEST_DIR/events33"
mkdir -p "$EVENTS/processed"

cat > "$EVENTS/config.yaml" <<'YAML'
dedup-window:
retention-max-bytes:
YAML

# Empty values should be silently ignored (defaults used)
E1=$($NBS_BUS publish "$EVENTS" empty-src empty-type normal "first")
E2=$($NBS_BUS publish "$EVENTS" empty-src empty-type normal "second")
EXIT2=$?
if [[ "$EXIT2" -eq 0 ]]; then
    check "Empty config values → defaults (no dedup)" "pass"
else
    check "Empty config values → defaults (no dedup, exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 34: Config.yaml with overflow values ---
echo "34. Config.yaml with numeric overflow values..."
EVENTS="$TEST_DIR/events34"
mkdir -p "$EVENTS/processed"

cat > "$EVENTS/config.yaml" <<'YAML'
retention-max-bytes: 99999999999999999999999999999999
dedup-window: 99999999999999999999999999999999
YAML

# Overflow should be caught by errno==ERANGE, defaults used
E1=$($NBS_BUS publish "$EVENTS" ovf-src ovf-type normal "first")
E2=$($NBS_BUS publish "$EVENTS" ovf-src ovf-type normal "second")
EXIT2=$?
if [[ "$EXIT2" -eq 0 ]]; then
    check "Overflow values ignored, defaults used (no dedup)" "pass"
else
    check "Overflow values ignored, defaults used (exit: $EXIT2)" "fail"
fi

echo ""

# --- Test 35: Config.yaml with key but colon only ---
echo "35. Config.yaml with bare colons and odd formatting..."
EVENTS="$TEST_DIR/events35"
mkdir -p "$EVENTS/processed"

cat > "$EVENTS/config.yaml" <<'YAML'
:
::
: 60
dedup-window:60
  dedup-window: 60
YAML

# All should be handled gracefully — none should match valid keys
# (indented key won't match, no-space-after-colon value has leading whitespace trimmed)
E1=$($NBS_BUS publish "$EVENTS" bare-src bare-type normal "first")
EXIT1=$?
if [[ "$EXIT1" -eq 0 ]]; then
    check "Bare colons and odd formatting handled safely" "pass"
else
    check "Bare colons and odd formatting handled safely (exit: $EXIT1)" "fail"
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
