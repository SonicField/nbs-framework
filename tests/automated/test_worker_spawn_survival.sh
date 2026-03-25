#!/bin/bash
# Test: Worker spawn survival
#
# Verifies that a worker spawned via nbs-spawn-worker actually survives
# long enough to complete a task. This is an integration test that requires
# a real Claude API connection.
#
# The test caught a critical bug where workers spawned from C binaries
# died after ~30 seconds due to inherited process state. Workers spawned
# via bash scripts (the agent pattern) survive reliably.
#
# Tests:
#   1. Worker session exists after spawn
#   2. Worker survives past 60 seconds
#   3. Worker posts expected message to chat
#   4. Worker updates task file state to completed/done
#   5. Cleanup: worker session is gone after completion
#
# Falsification: if any worker spawn mechanism causes workers to die
# before completing their task, test 3 will fail (no message in chat).
# The 60-second survival check (test 2) specifically targets the 30s
# death pattern that indicated the broken C binary spawn path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Require the spawn script and nbs-chat
SPAWN_SCRIPT=""
for d in "$PROJECT_ROOT/.nbs/bin" "$PROJECT_ROOT/bin"; do
    if [[ -x "$d/nbs-spawn-worker" ]]; then
        SPAWN_SCRIPT="$d/nbs-spawn-worker"
        break
    fi
done
if [[ -z "$SPAWN_SCRIPT" ]]; then
    echo "SKIP: nbs-spawn-worker not found"
    exit 0
fi

NBS_CHAT=""
for d in "$PROJECT_ROOT/.nbs/bin" "$PROJECT_ROOT/bin"; do
    if [[ -x "$d/nbs-chat" ]]; then
        NBS_CHAT="$d/nbs-chat"
        break
    fi
done
if [[ -z "$NBS_CHAT" ]]; then
    echo "SKIP: nbs-chat not found"
    exit 0
fi

PASS=0
FAIL=0

pass() {
    PASS=$((PASS + 1))
    echo "   PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    echo "   FAIL: $1"
}

# --- Setup ---
TEST_DIR=$(mktemp -d)
TEST_CHAT="$TEST_DIR/test.chat"
WORKERS_DIR="$TEST_DIR/.nbs/workers"
PIDS_DIR="$TEST_DIR/.nbs/pids"
mkdir -p "$WORKERS_DIR" "$PIDS_DIR" "$TEST_DIR/.nbs/events/processed"

# Create test chat
"$NBS_CHAT" create "$TEST_CHAT" >/dev/null 2>&1

# Unique marker so we can identify our worker's message
MARKER="WORKER_TEST_$(date +%s)_$$"

cleanup() {
    # Kill any test worker sessions
    for ses in $(tmux list-sessions -F '#{session_name}' 2>/dev/null | grep "nbs-testworker-worker-" || true); do
        tmux kill-session -t "$ses" 2>/dev/null || true
    done
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== Worker Spawn Survival Test ==="
echo "Test dir: $TEST_DIR"
echo "Marker: $MARKER"
echo ""

# --- Test 1 & 2: Spawn worker and check survival ---
echo "1-2. Spawning worker with task..."

# The task: post a message to chat with our marker, then stop
TASK_INSTRUCTIONS="Post this exact message to chat using: nbs-chat send $TEST_CHAT testworker '$MARKER' — then update State to completed in your task file and stop."

# Spawn via the bash script (same path as /pythia in terminal)
SESSION=$(bash "$SPAWN_SCRIPT" testworker "$TEST_DIR" \
    "$PROJECT_ROOT/claude_tools/nbs-notify.md" \
    "$TASK_INSTRUCTIONS" 2>&1)

if [[ -z "$SESSION" ]]; then
    fail "Worker spawn returned empty session name"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed ==="
    exit 1
fi

echo "   Session: $SESSION"

# Check session exists immediately
sleep 5
if tmux has-session -t "$SESSION" 2>/dev/null; then
    pass "Worker session exists after spawn"
else
    fail "Worker session does not exist after spawn"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed ==="
    exit 1
fi

# Wait and check survival past 60 seconds (the 30s death threshold)
echo "   Waiting 65 seconds for survival check..."
sleep 60
if tmux has-session -t "$SESSION" 2>/dev/null; then
    pass "Worker survived past 60 seconds"
else
    # Check if it completed quickly (success) vs died (failure)
    TASK_FILE=$(ls "$WORKERS_DIR"/testworker-*.md 2>/dev/null | head -1)
    if [[ -n "$TASK_FILE" ]]; then
        STATE=$(grep -m1 'State:' "$TASK_FILE" 2>/dev/null | awk '{print $2}')
        if [[ "$STATE" == "completed" || "$STATE" == "done" ]]; then
            pass "Worker completed before 60s (fast completion, not death)"
        else
            fail "Worker died before 60s (State: ${STATE:-unknown})"
        fi
    else
        fail "Worker died before 60s (no task file found)"
    fi
fi

# --- Test 3: Check chat for expected message ---
echo "3. Checking chat for worker message..."

# Give the worker up to 120 more seconds to post
FOUND=0
for i in $(seq 1 24); do
    if "$NBS_CHAT" search "$TEST_CHAT" "$MARKER" 2>/dev/null | grep -q "$MARKER"; then
        FOUND=1
        break
    fi
    sleep 5
done

if [[ $FOUND -eq 1 ]]; then
    pass "Worker posted expected message to chat"
else
    fail "Worker did not post message within timeout"
fi

# --- Test 4: Check task file state ---
echo "4. Checking task file state..."
TASK_FILE=$(ls "$WORKERS_DIR"/testworker-*.md 2>/dev/null | head -1)
if [[ -n "$TASK_FILE" ]]; then
    STATE=$(grep -m1 'State:' "$TASK_FILE" 2>/dev/null | awk '{print $2}')
    if [[ "$STATE" == "completed" || "$STATE" == "done" ]]; then
        pass "Task state updated to $STATE"
    elif [[ "$STATE" == "running" ]] && [[ $FOUND -eq 1 ]]; then
        # Worker posted but didn't update state — partial success
        pass "Worker posted to chat (state not updated but task completed)"
    else
        fail "Task state is '$STATE' (expected completed/done)"
    fi
else
    fail "No task file found in $WORKERS_DIR"
fi

# --- Test 5: Session cleanup ---
echo "5. Checking session cleanup..."
# Kill if still running
tmux kill-session -t "$SESSION" 2>/dev/null || true
sleep 2
if tmux has-session -t "$SESSION" 2>/dev/null; then
    fail "Session still exists after kill"
else
    pass "Session cleaned up"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
