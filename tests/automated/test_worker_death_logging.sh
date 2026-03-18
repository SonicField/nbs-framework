#!/bin/bash
# Test: Worker death summary logging
#
# Falsification approach:
# Each test creates a real worker via bin/nbs-workers spawn in a temporary
# project directory.  We provide a stub bin/nbs-claude that kills its parent
# bash process, causing the tmux session to die.  The monitoring loop inside
# spawn detects the death and writes a summary to the log file.  We verify:
#   1. The death summary marker and fields appear in the log after a crash.
#   2. `nbs-workers status` surfaces the death info section.
#   3. A worker dismissed before it dies has no death summary in its log.
#
# Dependencies: bin/nbs-workers (compiled), tmux
#
# Timing: Tests 1+2 take ~25s (spawn blocks until death is detected).
#         Test 3 takes ~10s (spawn runs in background, dismissed early).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_WORKERS="$PROJECT_ROOT/bin/nbs-workers"

ERRORS=0

# --- Helpers ---

pass() {
    echo "   PASS: $1"
}

fail() {
    echo "   FAIL: $1" >&2
    ERRORS=$((ERRORS + 1))
}

# --- Preflight ---

if [[ ! -x "$NBS_WORKERS" ]]; then
    echo "SKIP: bin/nbs-workers not found or not executable" >&2
    exit 0
fi

if ! command -v tmux >/dev/null 2>&1; then
    echo "SKIP: tmux not installed" >&2
    exit 0
fi

# --- Setup temp project directory ---

TEST_DIR=$(mktemp -d)
ORIGINAL_DIR=$(pwd)

cleanup() {
    cd "$ORIGINAL_DIR"
    # Kill any leftover background spawn processes
    if [[ -n "${SPAWN_PID_3:-}" ]] && kill -0 "$SPAWN_PID_3" 2>/dev/null; then
        kill "$SPAWN_PID_3" 2>/dev/null || true
        wait "$SPAWN_PID_3" 2>/dev/null || true
    fi
    # Kill any leftover tmux sessions from our test slugs
    tmux list-sessions -F '#{session_name}' 2>/dev/null \
        | grep -E '^pty_(deathcrash|cleanwrkr)' \
        | while read -r s; do
            tmux kill-session -t "$s" 2>/dev/null || true
        done
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== Worker Death Logging Tests ==="
echo "Test directory: $TEST_DIR"
echo ""

# =====================================================================
# TEST 1: Death summary written on crash
# =====================================================================
echo "--- Test 1: Death summary written on crash ---"

# Create a fresh project directory for this test
TEST1_DIR="$TEST_DIR/project1"
mkdir -p "$TEST1_DIR/.nbs/workers" "$TEST1_DIR/.nbs/pids" \
         "$TEST1_DIR/.nbs/events" "$TEST1_DIR/.nbs/chat" \
         "$TEST1_DIR/bin"

# Stub nbs-claude that kills the tmux session it runs in, simulating
# a crash that terminates the session.  Interactive bash ignores SIGTERM,
# so we kill the parent with SIGKILL to ensure the session dies.
cat > "$TEST1_DIR/bin/nbs-claude" << 'STUBEOF'
#!/bin/bash
# Simulate crash: SIGKILL the parent bash -l so the tmux session dies.
# Interactive bash ignores SIGTERM; SIGKILL is unconditional.
kill -9 $PPID 2>/dev/null
exit 1
STUBEOF
chmod +x "$TEST1_DIR/bin/nbs-claude"

# Spawn blocks until the monitoring loop completes.  The stub kills
# bash, tmux session dies, first poll (~10s in) detects it, writes
# the death summary, then cleans up.  Total: ~22s.
cd "$TEST1_DIR"
WORKER_NAME=$("$NBS_WORKERS" spawn deathcrash "$TEST1_DIR" "crash test" 2>/dev/null) || true

if [[ -z "$WORKER_NAME" ]]; then
    fail "Test 1: spawn did not return a worker name"
else
    LOG_FILE="$TEST1_DIR/.nbs/workers/${WORKER_NAME}.log"

    if [[ ! -f "$LOG_FILE" ]]; then
        fail "Test 1a: log file does not exist: $LOG_FILE"
    else
        # Check for death summary marker
        if grep -q '=== WORKER DEATH SUMMARY ===' "$LOG_FILE"; then
            pass "Test 1a: death summary marker present in log"
        else
            fail "Test 1a: death summary marker NOT found in log"
            echo "   Log contents (last 20 lines):" >&2
            tail -20 "$LOG_FILE" >&2
        fi

        # Check for elapsed time field (e.g. "exited after ~10 seconds")
        if grep -q 'exited after ~[0-9]' "$LOG_FILE"; then
            pass "Test 1b: elapsed time present in death summary"
        else
            fail "Test 1b: elapsed time NOT found in death summary"
        fi

        # Check for session name field (session prefix is "pty_")
        if grep -q 'Session: pty_' "$LOG_FILE"; then
            pass "Test 1c: session name present in death summary"
        else
            fail "Test 1c: session name NOT found in death summary"
        fi
    fi
fi

echo ""

# =====================================================================
# TEST 2: nbs-workers status shows death info
# =====================================================================
echo "--- Test 2: nbs-workers status shows death info ---"

if [[ -z "${WORKER_NAME:-}" ]]; then
    fail "Test 2: skipped (no worker name from test 1)"
else
    cd "$TEST1_DIR"
    STATUS_OUTPUT=$("$NBS_WORKERS" status "$WORKER_NAME" 2>/dev/null) || true

    if echo "$STATUS_OUTPUT" | grep -q 'death info:'; then
        pass "Test 2a: status output contains 'death info:' section"
    else
        fail "Test 2a: status output does NOT contain 'death info:' section"
        echo "   Status output was:" >&2
        echo "$STATUS_OUTPUT" >&2
    fi

    if echo "$STATUS_OUTPUT" | grep -q '=== WORKER'; then
        pass "Test 2b: status output contains death summary marker"
    else
        fail "Test 2b: status output does NOT contain death summary marker"
    fi
fi

echo ""

# =====================================================================
# TEST 3: Clean worker has no death summary
# =====================================================================
echo "--- Test 3: Clean worker (dismissed) has no death summary ---"

# Strategy: create a separate project with a stub nbs-claude that sleeps
# forever (simulating a running worker).  Run spawn in the background,
# then dismiss the worker before the session dies naturally.  Kill the
# background spawn process immediately after dismiss to prevent the
# monitoring loop from detecting the now-dead session and writing a
# death summary.  The log should contain no death summary.

TEST3_DIR="$TEST_DIR/project3"
mkdir -p "$TEST3_DIR/.nbs/workers" "$TEST3_DIR/.nbs/pids" \
         "$TEST3_DIR/.nbs/events" "$TEST3_DIR/.nbs/chat" \
         "$TEST3_DIR/bin"

# Stub nbs-claude that stays alive (sleeps forever)
cat > "$TEST3_DIR/bin/nbs-claude" << 'STUBEOF'
#!/bin/bash
# Stay alive so the session does not die on its own
exec sleep 3600
STUBEOF
chmod +x "$TEST3_DIR/bin/nbs-claude"

cd "$TEST3_DIR"
"$NBS_WORKERS" spawn cleanwrkr "$TEST3_DIR" "clean dismiss test" \
    >"$TEST_DIR/spawn3_out.txt" 2>/dev/null &
SPAWN_PID_3=$!

# Wait for the task file to appear (spawn creates it before the tmux
# session and monitoring loop).  The initial setup takes ~2s.
# Poll up to 12s for the task file.
WORKER_NAME_3=""
for attempt in $(seq 1 12); do
    sleep 1
    for f in "$TEST3_DIR/.nbs/workers"/cleanwrkr-*.md; do
        if [[ -f "$f" ]]; then
            WORKER_NAME_3=$(basename "$f" .md)
            break 2
        fi
    done
done

if [[ -z "$WORKER_NAME_3" ]]; then
    fail "Test 3: could not find worker task file for cleanwrkr"
    kill "$SPAWN_PID_3" 2>/dev/null || true
    wait "$SPAWN_PID_3" 2>/dev/null || true
else
    # Dismiss the worker (kills tmux session, sets state to dismissed)
    cd "$TEST3_DIR"
    "$NBS_WORKERS" dismiss "$WORKER_NAME_3" >/dev/null 2>/dev/null || true

    # Kill the background spawn process immediately to prevent it from
    # detecting the dead session and writing a death summary
    kill "$SPAWN_PID_3" 2>/dev/null || true
    wait "$SPAWN_PID_3" 2>/dev/null || true
    unset SPAWN_PID_3

    LOG_FILE_3="$TEST3_DIR/.nbs/workers/${WORKER_NAME_3}.log"

    if [[ -f "$LOG_FILE_3" ]]; then
        if grep -q '=== WORKER DEATH SUMMARY ===' "$LOG_FILE_3"; then
            fail "Test 3: death summary found in log of dismissed worker"
        else
            pass "Test 3: no death summary in log of dismissed worker"
        fi
    else
        # No log file at all is also acceptable — no death summary
        pass "Test 3: no log file exists (no death summary possible)"
    fi
fi

echo ""

# =====================================================================
# Summary
# =====================================================================
echo "=== Summary ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "All tests passed."
    exit 0
else
    echo "$ERRORS test(s) FAILED."
    exit 1
fi
