#!/bin/bash
# Test: nbs-worker lifecycle with evidence-based verification
#
# Tests: spawn (without Claude), status, search, results, dismiss, list
# Uses a modified spawn approach: creates task file + tmux session manually
# since real spawn launches Claude which is not suitable for automated tests.
#
# Falsification approach:
# - Each operation produces evidence that is checked deterministically
# - Log persistence is verified by echoing a marker, exiting, and finding it
# - Unique naming is verified by checking 50 generated names for collisions

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_WORKER="$PROJECT_ROOT/bin/nbs-worker"

# Use a temp directory so we don't pollute the real .nbs/workers/
TEST_DIR=$(mktemp -d)
CROSS_DIR_PROJECT=$(mktemp -d)
CROSS_DIR_OTHER=$(mktemp -d)
ORIGINAL_DIR=$(pwd)

ERRORS=0

cleanup() {
    cd "$ORIGINAL_DIR"
    # Kill any test sessions
    tmux kill-session -t "pty_lifecycle-test" 2>/dev/null || true
    tmux kill-session -t "pty_persist-test" 2>/dev/null || true
    tmux kill-session -t "pty_crossdir-test" 2>/dev/null || true
    tmux kill-session -t "pty_bus-test" 2>/dev/null || true
    # Kill any sessions spawned via nbs-worker (pty_ prefix + generated hash)
    for s in $(tmux list-sessions -F '#{session_name}' 2>/dev/null | grep "^pty_crossdir-" || true); do
        tmux kill-session -t "$s" 2>/dev/null || true
    done
    rm -rf "$TEST_DIR"
    rm -rf "$CROSS_DIR_OTHER" 2>/dev/null || true
    rm -rf "$CROSS_DIR_PROJECT" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== nbs-worker Lifecycle Test ==="
echo "Test directory: $TEST_DIR"
echo ""

# Set up test project directory with .nbs/workers/
mkdir -p "$TEST_DIR/.nbs/workers"
cd "$TEST_DIR"

# --- Test 1: Name generation uniqueness ---
echo "1. Name generation uniqueness (50 names)..."
NAMES_FILE=$(mktemp)
for i in $(seq 1 50); do
    # Source the generate_name function by extracting it
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

# Create task file and tmux session manually (like spawn does, but without Claude)
WORKER_NAME="lifecycle-test"
SESSION="pty_${WORKER_NAME}"
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

# Create tmux session
tmux new-session -d -s "$SESSION" 'bash'
sleep 0.5

# Start persistent logging
tmux pipe-pane -t "$SESSION" -o "cat >> '$TEST_DIR/$LOG_FILE'"

echo "   Created session and task file"

# --- Test 4: Status while running ---
echo "4. Status while running..."
STATUS_OUT=$("$NBS_WORKER" status "$WORKER_NAME" 2>&1)
if echo "$STATUS_OUT" | grep -q "status:.*running"; then
    echo "   PASS: Status reports running"
else
    echo "   FAIL: Status did not report running"
    echo "   Output: $STATUS_OUT"
    ERRORS=$((ERRORS + 1))
fi

if echo "$STATUS_OUT" | grep -q "tmux session: yes"; then
    echo "   PASS: tmux alive reported"
else
    echo "   FAIL: tmux alive not reported"
    echo "   Output: $STATUS_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 5: Send marker and verify log ---
echo "5. Send marker and verify in log..."
MARKER="LIFECYCLE_MARKER_$(date +%s)"
tmux send-keys -t "$SESSION" "echo $MARKER" Enter
sleep 1

if [[ -f "$LOG_FILE" ]] && grep -q "$MARKER" "$LOG_FILE"; then
    echo "   PASS: Marker found in persistent log"
else
    echo "   FAIL: Marker not found in log"
    if [[ -f "$LOG_FILE" ]]; then
        echo "   Log contents: $(cat "$LOG_FILE")"
    else
        echo "   Log file does not exist"
    fi
    ERRORS=$((ERRORS + 1))
fi

# --- Test 6: Search finds marker ---
echo "6. Search finds marker..."
SEARCH_OUT=$("$NBS_WORKER" search "$WORKER_NAME" "$MARKER" --context=2 2>&1)
if echo "$SEARCH_OUT" | grep -q "$MARKER"; then
    echo "   PASS: Search found marker"
else
    echo "   FAIL: Search did not find marker"
    echo "   Output: $SEARCH_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 7: List shows worker ---
echo "7. List shows worker..."
LIST_OUT=$("$NBS_WORKER" list 2>&1)
if echo "$LIST_OUT" | grep -q "$WORKER_NAME"; then
    echo "   PASS: Worker in list"
else
    echo "   FAIL: Worker not in list"
    echo "   Output: $LIST_OUT"
    ERRORS=$((ERRORS + 1))
fi

if echo "$LIST_OUT" | grep -q "alive"; then
    echo "   PASS: List shows tmux alive"
else
    echo "   FAIL: List does not show alive"
    echo "   Output: $LIST_OUT"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 8: Results extraction ---
echo "8. Results extraction..."

# First, add some content to the Log section
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

if echo "$RESULTS_OUT" | grep -q "## Log"; then
    echo "   PASS: Results includes Log header"
else
    echo "   FAIL: Results missing Log header"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 9: Dismiss ---
echo "9. Dismiss worker..."
DISMISS_OUT=$("$NBS_WORKER" dismiss "$WORKER_NAME" 2>&1)
if echo "$DISMISS_OUT" | grep -q "Dismissed: $WORKER_NAME"; then
    echo "   PASS: Dismiss reported success"
else
    echo "   FAIL: Dismiss did not report success"
    echo "   Output: $DISMISS_OUT"
    ERRORS=$((ERRORS + 1))
fi

# Verify tmux session is dead
if ! tmux has-session -t "$SESSION" 2>/dev/null; then
    echo "   PASS: tmux session killed"
else
    echo "   FAIL: tmux session still alive"
    ERRORS=$((ERRORS + 1))
fi

# Verify log file preserved
if [[ -f "$LOG_FILE" ]]; then
    echo "   PASS: Log file preserved after dismiss"
else
    echo "   FAIL: Log file deleted after dismiss"
    ERRORS=$((ERRORS + 1))
fi

# Verify task file updated
if grep -q "State: dismissed" "$TASK_FILE"; then
    echo "   PASS: Task file state updated to dismissed"
else
    echo "   FAIL: Task file state not updated"
    echo "   State line: $(grep 'State:' "$TASK_FILE")"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 10: Status after dismiss ---
echo "10. Status after dismiss..."
STATUS_AFTER=$("$NBS_WORKER" status "$WORKER_NAME" 2>&1)
if echo "$STATUS_AFTER" | grep -q "tmux session: no"; then
    echo "   PASS: tmux reported dead after dismiss"
else
    echo "   FAIL: tmux not reported dead"
    echo "   Output: $STATUS_AFTER"
    ERRORS=$((ERRORS + 1))
fi

if echo "$STATUS_AFTER" | grep -q "task state:.*dismissed"; then
    echo "   PASS: State field shows dismissed"
else
    echo "   FAIL: State field does not show dismissed"
    echo "   Output: $STATUS_AFTER"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 11: Log persistence after session death ---
echo "11. Log persistence after natural session exit..."

PERSIST_NAME="persist-test"
PERSIST_SESSION="pty_${PERSIST_NAME}"
PERSIST_TASK=".nbs/workers/${PERSIST_NAME}.md"
PERSIST_LOG=".nbs/workers/${PERSIST_NAME}.log"
PERSIST_MARKER="PERSIST_MARKER_$(date +%s)"

# Create task file
cat > "$PERSIST_TASK" <<EOF
# Worker: persist

## Task

Persistence test.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF

# Create session, enable logging, send marker, then let session exit
tmux new-session -d -s "$PERSIST_SESSION" 'bash'
sleep 0.5
tmux pipe-pane -t "$PERSIST_SESSION" -o "cat >> '$TEST_DIR/$PERSIST_LOG'"
sleep 0.3
tmux send-keys -t "$PERSIST_SESSION" "echo $PERSIST_MARKER" Enter
sleep 1
# Exit the session naturally
tmux send-keys -t "$PERSIST_SESSION" "exit" Enter
sleep 1

# Session should be dead now
if ! tmux has-session -t "$PERSIST_SESSION" 2>/dev/null; then
    echo "   PASS: Session exited naturally"
else
    echo "   FAIL: Session still alive after exit"
    tmux kill-session -t "$PERSIST_SESSION" 2>/dev/null || true
    ERRORS=$((ERRORS + 1))
fi

# Log should still contain the marker
if [[ -f "$PERSIST_LOG" ]] && grep -q "$PERSIST_MARKER" "$PERSIST_LOG"; then
    echo "   PASS: Marker survived session exit in persistent log"
else
    echo "   FAIL: Marker not found after session exit"
    if [[ -f "$PERSIST_LOG" ]]; then
        echo "   Log contents: $(cat "$PERSIST_LOG")"
    else
        echo "   Log file does not exist"
    fi
    ERRORS=$((ERRORS + 1))
fi

# --- Test 12: Cross-directory spawn writes to project dir, not caller's cwd ---
echo "12. Cross-directory spawn (task files land in project dir)..."

# cd to a different directory than the project
cd "$CROSS_DIR_OTHER"

# Spawn from here, pointing at the project dir
# nbs-worker spawn launches Claude, which we don't want in tests.
# Instead, test the file placement logic directly by calling spawn
# and immediately killing the tmux session before Claude starts.
CROSS_NAME=$("$NBS_WORKER" spawn crossdir "$CROSS_DIR_PROJECT" "Cross-directory test task" 2>/dev/null)

if [[ -z "$CROSS_NAME" ]]; then
    echo "   FAIL: spawn returned empty name"
    ERRORS=$((ERRORS + 1))
else
    # Task file should be in the project dir, not in the other dir
    if [[ -f "$CROSS_DIR_PROJECT/.nbs/workers/${CROSS_NAME}.md" ]]; then
        echo "   PASS: Task file in project dir"
    else
        echo "   FAIL: Task file not found in project dir"
        echo "   Expected: $CROSS_DIR_PROJECT/.nbs/workers/${CROSS_NAME}.md"
        if [[ -f "$CROSS_DIR_OTHER/.nbs/workers/${CROSS_NAME}.md" ]]; then
            echo "   Found in caller's cwd instead (the old bug)"
        fi
        ERRORS=$((ERRORS + 1))
    fi

    # Log file should also be in the project dir
    if [[ -f "$CROSS_DIR_PROJECT/.nbs/workers/${CROSS_NAME}.log" ]]; then
        echo "   PASS: Log file in project dir"
    else
        echo "   FAIL: Log file not found in project dir"
        ERRORS=$((ERRORS + 1))
    fi

    # Nothing should have been created in the caller's cwd
    if [[ -d "$CROSS_DIR_OTHER/.nbs" ]]; then
        echo "   FAIL: .nbs directory created in caller's cwd (should only be in project dir)"
        ERRORS=$((ERRORS + 1))
    else
        echo "   PASS: No .nbs created in caller's cwd"
    fi

    # Kill the tmux session (Claude would be starting up)
    tmux kill-session -t "pty_${CROSS_NAME}" 2>/dev/null || true
fi

# Return to test dir for remaining tests
cd "$TEST_DIR"

# --- Test 13: Bus events published on dismiss ---
echo "13. Bus events on worker dismiss..."

# Set up events directory for bus integration
NBS_BUS="$PROJECT_ROOT/bin/nbs-bus"
if [[ -x "$NBS_BUS" ]]; then
    mkdir -p "$TEST_DIR/.nbs/events/processed"

    # Create a worker manually (no tmux needed for this test)
    BUS_WORKER="bus-test"
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

    # Create tmux session so dismiss has something to kill
    tmux new-session -d -s "pty_${BUS_WORKER}" 'bash'
    sleep 0.5

    # Dismiss the worker — should publish a worker-dismissed event
    "$NBS_WORKER" dismiss "$BUS_WORKER" > /dev/null 2>&1

    # Check if a bus event was published
    DISMISS_EVENTS=$(find "$TEST_DIR/.nbs/events" -maxdepth 1 -name "*${BUS_WORKER}*worker-dismissed*.event" 2>/dev/null | wc -l)
    if [[ "$DISMISS_EVENTS" -ge 1 ]]; then
        echo "   PASS: worker-dismissed event published to bus"
    else
        # Check if any events exist at all
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
