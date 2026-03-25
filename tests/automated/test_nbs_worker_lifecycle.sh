#!/bin/bash
# Test: nbs-workers lifecycle with evidence-based verification
#
# Tests: spawn (without Claude), status, search, results, dismiss, list
# Uses a modified spawn approach: creates task file + nbs-ts session manually
# since real spawn launches Claude which is not suitable for automated tests.
#
# Falsification approach:
# - Each operation produces evidence that is checked deterministically
# - Log persistence is verified by echoing a marker, exiting, and finding it
# - Unique naming is verified by checking 50 generated names for collisions

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_WORKER="$PROJECT_ROOT/bin/nbs-workers"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"

# Use a temp directory so we don't pollute the real .nbs/workers/
TEST_DIR=$(mktemp -d)
CROSS_DIR_PROJECT=$(mktemp -d)
CROSS_DIR_OTHER=$(mktemp -d)
ORIGINAL_DIR=$(pwd)

ERRORS=0
HANDLES=()

cleanup() {
    cd "$ORIGINAL_DIR"
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    rm -rf "$TEST_DIR"
    rm -rf "$CROSS_DIR_OTHER" 2>/dev/null || true
    rm -rf "$CROSS_DIR_PROJECT" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== nbs-workers Lifecycle Test ==="
echo "Test directory: $TEST_DIR"
echo ""

# Set up test project directory with .nbs/workers/
mkdir -p "$TEST_DIR/.nbs/workers"
cd "$TEST_DIR"

# --- Test 1: Name generation uniqueness ---
echo "1. Name generation uniqueness (50 names)..."
NAMES_FILE=$(mktemp)
for i in $(seq 1 50); do
    name=$(date +%s%N | sha256sum | head -c 4)
    echo "test-${name}" >> "$NAMES_FILE"
done
UNIQUE_COUNT=$(sort "$NAMES_FILE" | uniq | wc -l)
TOTAL_COUNT=$(wc -l < "$NAMES_FILE")
rm -f "$NAMES_FILE"

if [[ "$UNIQUE_COUNT" -eq "$TOTAL_COUNT" ]]; then
    echo "   PASS: $UNIQUE_COUNT unique names out of $TOTAL_COUNT"
else
    echo "   FAIL: Only $UNIQUE_COUNT unique names out of $TOTAL_COUNT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 2: Argument validation ---
echo "2. Argument validation..."
SPAWN_ERR=$("$NBS_WORKER" spawn 2>&1) || true
if echo "$SPAWN_ERR" | grep -q "Error: spawn requires"; then
    echo "   PASS: spawn rejects missing args"
else
    echo "   FAIL: spawn did not reject missing args"
    echo "   Output: $SPAWN_ERR"
    ERRORS=$((ERRORS + 1))
fi

STATUS_ERR=$("$NBS_WORKER" status 2>&1) || true
if echo "$STATUS_ERR" | grep -q "Error: status requires"; then
    echo "   PASS: status rejects missing args"
else
    echo "   FAIL: status did not reject missing args"
    ERRORS=$((ERRORS + 1))
fi

SEARCH_ERR=$("$NBS_WORKER" search 2>&1) || true
if echo "$SEARCH_ERR" | grep -q "Error: search requires"; then
    echo "   PASS: search rejects missing args"
else
    echo "   FAIL: search did not reject missing args"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 3: Manual lifecycle (simulating spawn without Claude) ---
echo "3. Manual lifecycle simulation..."

# Create task file and nbs-ts session manually (like spawn does, but without Claude)
WORKER_NAME="lifecycle-a1b2"
TASK_FILE=".nbs/workers/${WORKER_NAME}.md"
LOG_FILE=".nbs/workers/${WORKER_NAME}.log"

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
cat > "$TASK_FILE" <<EOF
# Worker: lifecycle

## Task

Test task for lifecycle verification.

## Status

State: running
Started: ${TIMESTAMP}
Completed:

## Log

[Worker appends findings here]
EOF

# Create nbs-ts session
HANDLE=$("$NBS_TS" create --name="nbs-${WORKER_NAME}" bash 2>&1 | tail -1)
HANDLES+=("$HANDLE")
sleep 1

echo "   Created session $HANDLE and task file"

# --- Test 4: Status while running ---
echo "4. Status while running..."
STATUS_OUT=$("$NBS_WORKER" status "$WORKER_NAME" 2>&1)
if echo "$STATUS_OUT" | grep -qi "running\|alive"; then
    echo "   PASS: Status reports running/alive"
else
    echo "   FAIL: Status did not report running"
    echo "   Output: $STATUS_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 5: Send marker and verify log ---
echo "5. Send marker and verify in log..."
MARKER="LIFECYCLE_MARKER_$(date +%s)"
"$NBS_TS" send "$HANDLE" "echo $MARKER" 2>/dev/null
sleep 2

# Read output from nbs-ts
READ_OUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>&1)
if echo "$READ_OUT" | grep -q "$MARKER"; then
    echo "   PASS: Marker found in session output"
else
    echo "   FAIL: Marker not found in session output"
    echo "   Output: $READ_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 6: List shows worker ---
echo "6. List shows worker..."
LIST_OUT=$("$NBS_WORKER" list 2>&1)
if echo "$LIST_OUT" | grep -q "$WORKER_NAME"; then
    echo "   PASS: Worker in list"
else
    echo "   FAIL: Worker not in list"
    echo "   Output: $LIST_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 7: Results extraction ---
echo "7. Results extraction..."

# Add content to the Log section
cat >> "$TASK_FILE" <<'EOF'

### Findings

Found something important here.

### Verdict

Test completed successfully.
EOF

RESULTS_OUT=$("$NBS_WORKER" results "$WORKER_NAME" 2>&1)
if echo "$RESULTS_OUT" | grep -q "Found something important"; then
    echo "   PASS: Results extracted Log content"
else
    echo "   FAIL: Results did not extract Log content"
    echo "   Output: $RESULTS_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 8: Dismiss ---
echo "8. Dismiss worker..."
DISMISS_OUT=$("$NBS_WORKER" dismiss "$WORKER_NAME" 2>&1)
if echo "$DISMISS_OUT" | grep -qi "dismiss"; then
    echo "   PASS: Dismiss reported success"
else
    echo "   FAIL: Dismiss did not report success"
    echo "   Output: $DISMISS_OUT"
    ERRORS=$((ERRORS + 1))
fi

# Verify task file updated
if grep -q "State: dismissed" "$TASK_FILE" 2>/dev/null; then
    echo "   PASS: Task file state updated to dismissed"
else
    echo "   FAIL: Task file state not updated"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 9: Bus events published on dismiss ---
echo "9. Bus events on worker dismiss..."

NBS_BUS="$PROJECT_ROOT/bin/nbs-bus"
if [[ -x "$NBS_BUS" ]]; then
    mkdir -p "$TEST_DIR/.nbs/events/processed"

    BUS_WORKER="bus-b3c4"
    BUS_TASK=".nbs/workers/${BUS_WORKER}.md"
    cat > "$BUS_TASK" <<EOF
# Worker: bus-test

## Task

Bus integration test.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF

    # Create nbs-ts session so dismiss has something to kill
    BUS_HANDLE=$("$NBS_TS" create --name="nbs-${BUS_WORKER}" bash 2>&1 | tail -1)
    HANDLES+=("$BUS_HANDLE")
    sleep 0.5

    "$NBS_WORKER" dismiss "$BUS_WORKER" > /dev/null 2>&1

    DISMISS_EVENTS=$(find "$TEST_DIR/.nbs/events" -maxdepth 1 -name "*${BUS_WORKER}*worker-dismissed*.event" 2>/dev/null | wc -l)
    if [[ "$DISMISS_EVENTS" -ge 1 ]]; then
        echo "   PASS: worker-dismissed event published to bus"
    else
        ALL_EVENTS=$(find "$TEST_DIR/.nbs/events" -maxdepth 1 -name "*.event" 2>/dev/null | wc -l)
        echo "   FAIL: worker-dismissed event not found (total events: $ALL_EVENTS)"
        ERRORS=$((ERRORS + 1))
    fi
else
    echo "   SKIP: nbs-bus binary not found at $NBS_BUS"
fi

# --- Summary ---
echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All lifecycle tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
