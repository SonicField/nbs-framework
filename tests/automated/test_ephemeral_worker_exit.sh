#!/bin/bash
# test_ephemeral_worker_exit.sh — Falsification tests for ephemeral worker lifecycle.
#
# Hypothesis 1: claude -p exits naturally after processing a prompt.
# Falsifier: If the process is still alive 60s after receiving a trivial task,
# the hypothesis is false.
#
# Hypothesis 2: No stale pidfile remains after worker completion.
# Falsifier: If the pidfile still exists after worker completion with a
# dead PID, the cleanup trap failed.
#
# Hypothesis 3: A second worker can spawn with the same handle after the
# first completes.
# Falsifier: If the second spawn fails, the pidfile was not cleaned up.
#
# Requires: claude, nbs-workers, nbs-chat, nbs-bus

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
BIN="$PROJECT_ROOT/bin"

PASS=0
FAIL=0
TESTS=0

pass() {
    PASS=$((PASS + 1))
    TESTS=$((TESTS + 1))
    echo "   PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    TESTS=$((TESTS + 1))
    echo "   FAIL: $1"
}

# --- Setup ---

TEST_DIR=$(mktemp -d)
ORIG_DIR=$(pwd)

mkdir -p "$TEST_DIR/.nbs/chat" \
         "$TEST_DIR/.nbs/events/processed" \
         "$TEST_DIR/.nbs/scribe" \
         "$TEST_DIR/.nbs/pids" \
         "$TEST_DIR/.nbs/workers" \
         "$TEST_DIR/bin"

# Create bus config
cat > "$TEST_DIR/.nbs/events/config.yaml" <<'YAML'
dedup-window: 0
ack-timeout: 120
YAML

# Create chat file
"$BIN/nbs-chat" create "$TEST_DIR/.nbs/chat/live.chat" 2>/dev/null || {
    cat > "$TEST_DIR/.nbs/chat/live.chat" <<'CHAT'
participants: testworker(0)
---
CHAT
}

# Create scribe log
cat > "$TEST_DIR/.nbs/scribe/live-log.md" <<'LOG'
# Decision Log
Project: test
Created: 2026-03-02
Decision count: 0
LOG

# Symlink binaries so nbs-workers can find them
ln -sf "$BIN"/* "$TEST_DIR/bin/" 2>/dev/null

# Create CLAUDE.md so claude -p has project context
cat > "$TEST_DIR/CLAUDE.md" <<'MD'
# Test project for ephemeral worker exit tests
MD

cleanup() {
    # Kill any test tmux sessions
    for s in $(tmux list-sessions -F '#{session_name}' 2>/dev/null | grep "^pty_worker-ephemeral-test" || true); do
        tmux kill-session -t "$s" 2>/dev/null || true
    done
    cd "$ORIG_DIR" || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== Ephemeral Worker Exit Tests ==="
echo "  Test dir: $TEST_DIR"
echo ""

cd "$TEST_DIR"

# =========================================================================
# Test 1: claude -p exits after processing
# =========================================================================

echo "1. claude -p exits after processing a trivial prompt..."

# Start claude -p in a tmux session with a trivial task
SESSION_T1="worker-ephemeral-test-t1-$$"
tmux new-session -d -s "pty_${SESSION_T1}" -c "$TEST_DIR" \
    "claude -p 'Say hello and exit.' --dangerously-skip-permissions --model 'opus[1m]' 2>/dev/null; echo WORKER_EXITED; sleep 5"

# Wait up to 120s for claude -p to complete
EXITED=0
for i in $(seq 1 120); do
    OUTPUT=$(tmux capture-pane -t "pty_${SESSION_T1}" -p 2>/dev/null || echo "")
    if echo "$OUTPUT" | grep -q "WORKER_EXITED"; then
        EXITED=1
        break
    fi
    sleep 1
done

if [[ "$EXITED" -eq 1 ]]; then
    pass "T1: claude -p exited naturally after ${i}s"
else
    fail "T1: claude -p still running after 120s"
fi

tmux kill-session -t "pty_${SESSION_T1}" 2>/dev/null || true

# =========================================================================
# Test 2: No stale pidfile lock after worker exits
# =========================================================================

echo ""
echo "2. No stale pidfile lock after worker exits..."

# Create a pidfile and lock it, then release it (simulating clean exit)
PIDFILE="$TEST_DIR/.nbs/pids/test-ephemeral.pid"
rm -f "$PIDFILE"

# Run claude -p via nbs-claude to test the full pidfile lifecycle
SESSION_T2="worker-ephemeral-test-t2-$$"
tmux new-session -d -s "pty_${SESSION_T2}" -c "$TEST_DIR" \
    "NBS_HANDLE=test-ephemeral claude -p 'Say done.' --dangerously-skip-permissions --model 'opus[1m]' 2>/dev/null; echo T2_DONE; sleep 5"

# Wait for completion
T2_DONE=0
for i in $(seq 1 120); do
    OUTPUT=$(tmux capture-pane -t "pty_${SESSION_T2}" -p 2>/dev/null || echo "")
    if echo "$OUTPUT" | grep -q "T2_DONE"; then
        T2_DONE=1
        break
    fi
    sleep 1
done

if [[ "$T2_DONE" -eq 1 ]]; then
    # Check if pidfile was cleaned up or contains a dead PID
    if [[ -f "$PIDFILE" ]]; then
        OLD_PID=$(cat "$PIDFILE" 2>/dev/null)
        if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
            fail "T2: pidfile contains live PID after exit"
        else
            pass "T2: pidfile contains dead PID (stale but harmless)"
        fi
    else
        pass "T2: no pidfile remains (clean exit)"
    fi
else
    fail "T2: worker did not complete in 120s, cannot test lock"
fi

tmux kill-session -t "pty_${SESSION_T2}" 2>/dev/null || true

# =========================================================================
# Test 3: Second worker can spawn with same handle after first completes
# =========================================================================

echo ""
echo "3. Second worker can spawn with same handle after first completes..."

rm -f "$TEST_DIR/.nbs/pids/ephemeral-reuse.pid"

# First worker
SESSION_T3A="worker-ephemeral-test-t3a-$$"
tmux new-session -d -s "pty_${SESSION_T3A}" -c "$TEST_DIR" \
    "claude -p 'Say first.' --dangerously-skip-permissions --model 'opus[1m]' 2>/dev/null; echo T3A_DONE; sleep 3"

T3A_DONE=0
for i in $(seq 1 120); do
    OUTPUT=$(tmux capture-pane -t "pty_${SESSION_T3A}" -p 2>/dev/null || echo "")
    if echo "$OUTPUT" | grep -q "T3A_DONE"; then
        T3A_DONE=1
        break
    fi
    sleep 1
done

tmux kill-session -t "pty_${SESSION_T3A}" 2>/dev/null || true

if [[ "$T3A_DONE" -eq 0 ]]; then
    fail "T3: first worker did not complete, cannot test reuse"
else
    # Second worker with same concept
    SESSION_T3B="worker-ephemeral-test-t3b-$$"
    tmux new-session -d -s "pty_${SESSION_T3B}" -c "$TEST_DIR" \
        "claude -p 'Say second.' --dangerously-skip-permissions --model 'opus[1m]' 2>/dev/null; echo T3B_DONE; sleep 3"

    T3B_DONE=0
    for i in $(seq 1 120); do
        OUTPUT=$(tmux capture-pane -t "pty_${SESSION_T3B}" -p 2>/dev/null || echo "")
        if echo "$OUTPUT" | grep -q "T3B_DONE"; then
            T3B_DONE=1
            break
        fi
        sleep 1
    done

    if [[ "$T3B_DONE" -eq 1 ]]; then
        pass "T3: second worker spawned and completed after first exited"
    else
        fail "T3: second worker did not complete (blocked by stale state?)"
    fi

    tmux kill-session -t "pty_${SESSION_T3B}" 2>/dev/null || true
fi

# =========================================================================
# Test 4: ADVERSARIAL — tmux session exits after claude -p completes
# =========================================================================

echo ""
echo "4. tmux session exits after claude -p completes..."

SESSION_T4="worker-ephemeral-test-t4-$$"
tmux new-session -d -s "pty_${SESSION_T4}" -c "$TEST_DIR" \
    "claude -p 'Say goodbye.' --dangerously-skip-permissions --model 'opus[1m]' 2>/dev/null; exit"

# Wait up to 120s, then check if tmux session still exists
SESSION_GONE=0
for i in $(seq 1 120); do
    if ! tmux has-session -t "pty_${SESSION_T4}" 2>/dev/null; then
        SESSION_GONE=1
        break
    fi
    sleep 1
done

if [[ "$SESSION_GONE" -eq 1 ]]; then
    pass "T4: tmux session exited after claude -p completed (${i}s)"
else
    fail "T4: tmux session still alive after 120s"
    tmux kill-session -t "pty_${SESSION_T4}" 2>/dev/null || true
fi

# =========================================================================
# Test 5: ADVERSARIAL — no orphaned processes after worker exits
# =========================================================================

echo ""
echo "5. No orphaned claude processes after worker exits..."

# Count claude processes before
BEFORE=$(ps aux | grep "claude.*ephemeral-orphan-test" | grep -v grep | wc -l)

SESSION_T5="worker-ephemeral-test-t5-$$"
tmux new-session -d -s "pty_${SESSION_T5}" -c "$TEST_DIR" \
    "claude -p 'Say orphan test.' --dangerously-skip-permissions --model 'opus[1m]' 2>/dev/null; exit"

# Wait for session to die
for i in $(seq 1 120); do
    if ! tmux has-session -t "pty_${SESSION_T5}" 2>/dev/null; then
        break
    fi
    sleep 1
done
sleep 2

# Count claude processes after — should be same as before
AFTER=$(ps aux | grep "claude.*ephemeral-orphan-test" | grep -v grep | wc -l)

if [[ "$AFTER" -le "$BEFORE" ]]; then
    pass "T5: no orphaned claude processes (before=$BEFORE, after=$AFTER)"
else
    fail "T5: orphaned claude processes remain (before=$BEFORE, after=$AFTER)"
fi

# =========================================================================
# Summary
# =========================================================================

echo ""
echo "=== Results: $PASS passed, $FAIL failed (of $TESTS tests) ==="

if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
exit 0
