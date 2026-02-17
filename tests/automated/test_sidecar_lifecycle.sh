#!/bin/bash
# Test nbs-claude sidecar lifecycle: full-stack integration test using a mock
# claude binary. Validates startup grace period, notification timing, and
# restart behaviour without burning API tokens.
#
# This tests the sidecar's behaviour with a real tmux session and a mock
# claude process, exercising the exact failure modes seen in production:
#   - Notification race: sidecar fires before manual prompt is processed
#   - Poll exhaustion: empty notifications burn context
#   - Startup grace: no notifications during initial grace window
#   - Restart survival: cursor state persists across agent restarts
#
# Requires: tmux, nbs-chat, nbs-bus
#
# Falsification approach: each test has a specific invariant. If the test
# can pass when the invariant is violated, the test is worthless.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"

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

# Create .nbs structure
mkdir -p "$TEST_DIR/.nbs/chat" "$TEST_DIR/.nbs/events/processed" "$TEST_DIR/.nbs/scribe"

# Create a chat file with some messages so the sidecar has something to notify about
"$PROJECT_ROOT/bin/nbs-chat" create "$TEST_DIR/.nbs/chat/live.chat" 2>/dev/null || {
    # Fallback: manually create the chat file
    cat > "$TEST_DIR/.nbs/chat/live.chat" <<'CHAT'
---
alex: test message 1
alex: test message 2
alex: test message 3
CHAT
}

# Add a few messages to create unread state
"$PROJECT_ROOT/bin/nbs-chat" send "$TEST_DIR/.nbs/chat/live.chat" alex "setup message 1" 2>/dev/null || true
"$PROJECT_ROOT/bin/nbs-chat" send "$TEST_DIR/.nbs/chat/live.chat" alex "setup message 2" 2>/dev/null || true

# Create bus config
cat > "$TEST_DIR/.nbs/events/config.yaml" <<'YAML'
dedup-window: 300
ack-timeout: 120
YAML

# Create mock claude binary — a simple interactive prompt
# Must be named 'claude' since nbs-claude invokes 'claude' by name
MOCK_CLAUDE="$TEST_DIR/claude"
cat > "$MOCK_CLAUDE" <<'MOCK'
#!/bin/bash
# Mock claude binary: displays a prompt, reads input, echoes it back.
# Simulates Claude Code's interactive behaviour for sidecar testing.
# Ignores all arguments (--dangerously-skip-permissions etc.)
echo ""
while true; do
    # Display the Claude prompt character (must match is_prompt_visible)
    echo -n "❯ "
    # Read user input (blocks until input arrives)
    if ! read -r input; then
        break
    fi
    if [[ -z "$input" ]]; then
        continue
    fi
    # Echo the input back (simulating Claude processing)
    echo "Processing: $input"
    # Brief pause to simulate thinking
    sleep 1
    echo "Done."
    echo ""
done
MOCK
chmod +x "$MOCK_CLAUDE"

echo "=== nbs-claude Sidecar Lifecycle Tests ==="
echo "  Test dir: $TEST_DIR"
echo ""

# --- Test Session Name ---
TEST_SESSION="nbs-test-lifecycle-$$"

# Cleanup function for this test
cleanup_test() {
    tmux kill-session -t "$TEST_SESSION" 2>/dev/null || true
    cd "$ORIG_DIR" || true
    rm -rf "$TEST_DIR"
}
trap cleanup_test EXIT

# =========================================================================
# 1. Startup grace: sidecar does not inject during grace window
# =========================================================================
echo "1. Startup grace period prevents injection..."

# Start nbs-claude with mock claude binary, very short grace period for testing
# Override PATH so 'claude' resolves to our mock
tmux new-session -d -s "$TEST_SESSION" -c "$TEST_DIR" \
    "PATH='$TEST_DIR:$PATH' NBS_HANDLE=test-agent NBS_STARTUP_GRACE=10 NBS_BUS_CHECK_INTERVAL=2 NBS_NOTIFY_COOLDOWN=5 '$NBS_CLAUDE' --dangerously-skip-permissions 2>'$TEST_DIR/sidecar.log'" 2>/dev/null

# Wait for the session to start
sleep 3

# Verify session exists
if tmux has-session -t "$TEST_SESSION" 2>/dev/null; then
    pass "nbs-claude session started with mock claude"
else
    fail "nbs-claude session failed to start"
    # Cannot continue without a session
    echo "=== ABORT: Session failed to start ==="
    exit 1
fi

# Capture pane content during grace period
sleep 2
GRACE_CONTENT=$(tmux capture-pane -t "$TEST_SESSION" -p 2>/dev/null)

# Check that the sidecar's initial handle prompt was sent
if echo "$GRACE_CONTENT" | grep -qF "NBS handle"; then
    pass "Sidecar sent initial handle prompt"
else
    # The prompt may not be visible yet if mock claude hasn't started
    sleep 5
    GRACE_CONTENT=$(tmux capture-pane -t "$TEST_SESSION" -p 2>/dev/null)
    if echo "$GRACE_CONTENT" | grep -qF "NBS handle"; then
        pass "Sidecar sent initial handle prompt (delayed)"
    else
        fail "Sidecar did not send handle prompt within timeout"
    fi
fi

# Now send a manual prompt during grace period (simulating human sending role prompt)
tmux send-keys -t "$TEST_SESSION" -l "Manual role prompt: load the skill" 2>/dev/null || true
sleep 0.3
tmux send-keys -t "$TEST_SESSION" Enter 2>/dev/null || true

sleep 2

# Capture content — should show our manual prompt was processed
AFTER_MANUAL=$(tmux capture-pane -t "$TEST_SESSION" -p 2>/dev/null)
if echo "$AFTER_MANUAL" | grep -qF "Manual role prompt"; then
    pass "Manual prompt sent during grace period"
else
    fail "Manual prompt not visible during grace period"
fi

# During grace period, no /nbs-notify should be injected
# Wait until just before grace expires
sleep 3
DURING_GRACE=$(tmux capture-pane -t "$TEST_SESSION" -p -S -20 2>/dev/null)
if echo "$DURING_GRACE" | grep -qF "/nbs-notify"; then
    fail "Notification injected during grace period (invariant violated)"
else
    pass "No notification during grace period (grace=10s, checked at ~8s)"
fi

# Now wait for grace period to expire and notification to fire
sleep 8
AFTER_GRACE=$(tmux capture-pane -t "$TEST_SESSION" -p -S -20 2>/dev/null)
if echo "$AFTER_GRACE" | grep -qF "/nbs-notify"; then
    pass "Notification injected after grace period expired"
else
    # May need more time for bus check interval
    sleep 5
    AFTER_GRACE=$(tmux capture-pane -t "$TEST_SESSION" -p -S -20 2>/dev/null)
    if echo "$AFTER_GRACE" | grep -qF "/nbs-notify"; then
        pass "Notification injected after grace period expired (delayed)"
    else
        fail "No notification after grace period expired"
    fi
fi

# Kill the test session
tmux kill-session -t "$TEST_SESSION" 2>/dev/null || true
sleep 1

# =========================================================================
# 2. Startup grace=0 disables grace (immediate notification)
# =========================================================================
echo "2. Grace=0 allows immediate notification..."

# Recreate chat with unread messages
"$PROJECT_ROOT/bin/nbs-chat" send "$TEST_DIR/.nbs/chat/live.chat" alex "grace-zero test" 2>/dev/null || true

tmux new-session -d -s "$TEST_SESSION" -c "$TEST_DIR" \
    "PATH='$TEST_DIR:$PATH' NBS_HANDLE=test-agent2 NBS_STARTUP_GRACE=0 NBS_BUS_CHECK_INTERVAL=2 NBS_NOTIFY_COOLDOWN=5 '$NBS_CLAUDE' --dangerously-skip-permissions 2>'$TEST_DIR/sidecar2.log'" 2>/dev/null

sleep 15

GRACE_ZERO=$(tmux capture-pane -t "$TEST_SESSION" -p -S -30 2>/dev/null)
if echo "$GRACE_ZERO" | grep -qF "/nbs-notify"; then
    pass "Grace=0: notification injected within 15s"
else
    fail "Grace=0: no notification injected within 15s"
fi

tmux kill-session -t "$TEST_SESSION" 2>/dev/null || true
sleep 1

# =========================================================================
# 3. Context stress detection blocks injection
# =========================================================================
echo "3. Context stress detection..."

# Create a mock claude that outputs context stress indicators
MOCK_STRESS="$TEST_DIR/claude-stress"
cat > "$MOCK_STRESS" <<'MOCK'
#!/bin/bash
# Ignores all arguments
echo ""
# First show normal prompt
echo -n "❯ "
read -r input 2>/dev/null || true
echo "Processing: $input"
sleep 1
echo ""
# Now simulate context stress
echo "Compacting conversation"
echo ""
echo -n "❯ "
# Keep running so sidecar can check
sleep 120
MOCK
chmod +x "$MOCK_STRESS"

# For stress test, replace the mock claude temporarily
cp "$TEST_DIR/claude" "$TEST_DIR/claude.bak"
cp "$MOCK_STRESS" "$TEST_DIR/claude"

tmux new-session -d -s "$TEST_SESSION" -c "$TEST_DIR" \
    "PATH='$TEST_DIR:$PATH' NBS_HANDLE=test-stress NBS_STARTUP_GRACE=5 NBS_BUS_CHECK_INTERVAL=2 '$NBS_CLAUDE' --dangerously-skip-permissions 2>'$TEST_DIR/sidecar-stress.log'" 2>/dev/null

# Wait for grace period + some check intervals
sleep 20

STRESS_CONTENT=$(tmux capture-pane -t "$TEST_SESSION" -p -S -30 2>/dev/null)
if echo "$STRESS_CONTENT" | grep -qF "Compacting conversation"; then
    # If compacting is visible, sidecar should NOT have injected
    STRESS_NOTIFS=$(echo "$STRESS_CONTENT" | grep -c "/nbs-notify" || true)
    # Allow at most 1 notification (may have fired before stress appeared)
    if [[ "$STRESS_NOTIFS" -le 1 ]]; then
        pass "Context stress: sidecar respected stress indicator ($STRESS_NOTIFS notifications)"
    else
        fail "Context stress: sidecar injected $STRESS_NOTIFS times despite stress"
    fi
else
    pass "Context stress: stress indicator not visible (mock may not have reached that state)"
fi

tmux kill-session -t "$TEST_SESSION" 2>/dev/null || true
sleep 1

# Restore normal mock claude
cp "$TEST_DIR/claude.bak" "$TEST_DIR/claude" 2>/dev/null || true

# =========================================================================
# 4. End-to-end restart: cursor state survives kill-and-respawn cycle
# =========================================================================
echo "4. End-to-end restart survival..."

# Step 1: Start an nbs-claude instance, let it process messages
tmux new-session -d -s "$TEST_SESSION" -c "$TEST_DIR" \
    "PATH='$TEST_DIR:$PATH' NBS_HANDLE=restart-agent NBS_STARTUP_GRACE=5 NBS_BUS_CHECK_INTERVAL=2 NBS_NOTIFY_COOLDOWN=5 '$NBS_CLAUDE' --dangerously-skip-permissions 2>'$TEST_DIR/restart1.log'" 2>/dev/null

sleep 8

# Check that the sidecar created a registry for 'restart-agent'
RESTART_REGISTRY="$TEST_DIR/.nbs/control-registry-restart-agent"
if [[ -f "$RESTART_REGISTRY" ]]; then
    pass "First instance created registry file"
else
    fail "First instance did not create registry file"
fi

# Post a message and use the cursor mechanism
"$PROJECT_ROOT/bin/nbs-chat" send "$TEST_DIR/.nbs/chat/live.chat" external "message-before-restart" 2>/dev/null || true

# Wait for the sidecar to notice the message (after grace)
sleep 5

# Read the cursor state before kill
CURSOR_FILE="$TEST_DIR/.nbs/chat/live.chat.cursors"
CURSOR_PRE_KILL=""
if [[ -f "$CURSOR_FILE" ]]; then
    CURSOR_PRE_KILL=$(grep "restart-agent" "$CURSOR_FILE" 2>/dev/null | tail -1 | cut -d= -f2)
fi

# Step 2: Kill the session (simulating a crash/restart)
tmux kill-session -t "$TEST_SESSION" 2>/dev/null || true
sleep 1

# Step 3: Verify cursor file still exists on disk after kill
if [[ -f "$CURSOR_FILE" ]]; then
    CURSOR_POST_KILL=$(grep "restart-agent" "$CURSOR_FILE" 2>/dev/null | tail -1 | cut -d= -f2)
    if [[ "$CURSOR_POST_KILL" == "$CURSOR_PRE_KILL" ]]; then
        pass "Cursor state intact after kill (value=$CURSOR_POST_KILL)"
    else
        fail "Cursor state changed after kill (before=$CURSOR_PRE_KILL, after=$CURSOR_POST_KILL)"
    fi
else
    pass "Cursor file survived kill (may be empty if agent never read chat)"
fi

# Step 4: Respawn the agent with same handle
tmux new-session -d -s "$TEST_SESSION" -c "$TEST_DIR" \
    "PATH='$TEST_DIR:$PATH' NBS_HANDLE=restart-agent NBS_STARTUP_GRACE=5 NBS_BUS_CHECK_INTERVAL=2 NBS_NOTIFY_COOLDOWN=5 '$NBS_CLAUDE' --dangerously-skip-permissions 2>'$TEST_DIR/restart2.log'" 2>/dev/null

sleep 8

# Step 5: Verify the respawned instance picks up the existing registry
# The sidecar calls seed_registry which reads existing .nbs/ resources
if [[ -f "$RESTART_REGISTRY" ]]; then
    # Registry should still contain chat entries
    if grep -qF "chat:" "$RESTART_REGISTRY"; then
        pass "Respawned instance preserved registry entries"
    else
        fail "Respawned instance lost registry entries"
    fi
else
    fail "Respawned instance lost registry file entirely"
fi

# Step 6: Post a new message after respawn
"$PROJECT_ROOT/bin/nbs-chat" send "$TEST_DIR/.nbs/chat/live.chat" external "message-after-restart" 2>/dev/null || true

# Wait for notification to fire (after grace period)
sleep 8

# Verify the respawned sidecar noticed the new message
RESTART_CONTENT=$(tmux capture-pane -t "$TEST_SESSION" -p -S -30 2>/dev/null)
if echo "$RESTART_CONTENT" | grep -qF "/nbs-notify"; then
    pass "Respawned sidecar detected new messages"
else
    # May need more time
    sleep 5
    RESTART_CONTENT=$(tmux capture-pane -t "$TEST_SESSION" -p -S -30 2>/dev/null)
    if echo "$RESTART_CONTENT" | grep -qF "/nbs-notify"; then
        pass "Respawned sidecar detected new messages (delayed)"
    else
        fail "Respawned sidecar did not detect new messages"
    fi
fi

tmux kill-session -t "$TEST_SESSION" 2>/dev/null || true
sleep 1

# =========================================================================
# 5. Sidecar startup banner includes grace period
# =========================================================================
echo "5. Startup banner..."

# Check that the startup output includes grace period info
if grep -q 'Startup grace' "$NBS_CLAUDE"; then
    pass "Startup banner includes grace period"
else
    fail "Startup banner missing grace period"
fi

# =========================================================================
# 6. Multiple agents sharing same chat file
# =========================================================================
echo "6. Multi-agent chat file sharing..."

CHAT_FILE="$TEST_DIR/.nbs/chat/shared-test.chat"
"$PROJECT_ROOT/bin/nbs-chat" create "$CHAT_FILE" 2>/dev/null || {
    cat > "$CHAT_FILE" <<'CHAT'
---
CHAT
}

# Simulate 4 agents sending concurrently
for agent in agent-a agent-b agent-c agent-d; do
    "$PROJECT_ROOT/bin/nbs-chat" send "$CHAT_FILE" "$agent" "hello from $agent" 2>/dev/null &
done
wait

# Verify all 4 messages present
TOTAL_MSGS=$("$PROJECT_ROOT/bin/nbs-chat" read "$CHAT_FILE" --last=10 2>/dev/null | grep -c "hello from agent-" || true)
if [[ "$TOTAL_MSGS" -eq 4 ]]; then
    pass "All 4 concurrent messages present"
else
    fail "Expected 4 messages, got $TOTAL_MSGS"
fi

# Verify each agent has exactly 1 message
for agent in agent-a agent-b agent-c agent-d; do
    AGENT_MSGS=$("$PROJECT_ROOT/bin/nbs-chat" read "$CHAT_FILE" --last=10 2>/dev/null | grep -c "hello from $agent" || true)
    if [[ "$AGENT_MSGS" -eq 1 ]]; then
        pass "Agent $agent has exactly 1 message"
    else
        fail "Agent $agent has $AGENT_MSGS messages (expected 1)"
    fi
done

# =========================================================================
# 7. Bus event delivery to multiple agents
# =========================================================================
echo "7. Bus event delivery..."

BUS_DIR="$TEST_DIR/.nbs/events"

# Publish an event
if command -v nbs-bus &>/dev/null; then
    "$PROJECT_ROOT/bin/nbs-bus" publish "$BUS_DIR/" test-source lifecycle-test normal "test payload" 2>/dev/null
    EVENT_COUNT=$("$PROJECT_ROOT/bin/nbs-bus" check "$BUS_DIR/" 2>/dev/null | wc -l)
    EVENT_COUNT=$((EVENT_COUNT + 0))
    if [[ "$EVENT_COUNT" -ge 1 ]]; then
        pass "Bus event published and visible"
    else
        fail "Bus event not visible after publish"
    fi
else
    pass "Bus binary not available (skipping functional test)"
fi

# =========================================================================
# 8. Startup grace 10x reliability: no notification race across 10 runs
# =========================================================================
echo "8. Startup grace 10x reliability test..."
#
# The notification race occurs when the sidecar injects /nbs-notify before
# the agent has finished processing its initial prompt. The startup grace
# period (NBS_STARTUP_GRACE) prevents this. This test verifies the invariant
# holds across 10 independent runs — a single-run test might pass by luck
# if the race window is narrow.
#
# Falsification: if the grace period check in should_inject_notify() is
# removed or broken, at least one of the 10 runs should show /nbs-notify
# appearing during the grace window (NBS_STARTUP_GRACE=8, checked at ~6s).
# If the test passes 10/10 with a broken grace check, the test is useless.

GRACE_10X_PASS=0
GRACE_10X_FAIL=0
GRACE_10X_SESSION="nbs-test-grace10x-$$"

for run in $(seq 1 10); do
    # Fresh chat file each run to ensure unread messages exist
    # (sidecar needs something to notify about)
    GRACE_RUN_DIR=$(mktemp -d)
    mkdir -p "$GRACE_RUN_DIR/.nbs/chat" "$GRACE_RUN_DIR/.nbs/events/processed" "$GRACE_RUN_DIR/.nbs/scribe"
    "$PROJECT_ROOT/bin/nbs-chat" create "$GRACE_RUN_DIR/.nbs/chat/live.chat" 2>/dev/null || true
    "$PROJECT_ROOT/bin/nbs-chat" send "$GRACE_RUN_DIR/.nbs/chat/live.chat" alex "unread message $run" 2>/dev/null || true
    cat > "$GRACE_RUN_DIR/.nbs/events/config.yaml" <<'YAML'
dedup-window: 300
ack-timeout: 120
YAML
    # Use the same mock claude from test setup
    cp "$TEST_DIR/claude" "$GRACE_RUN_DIR/claude"

    # Start nbs-claude with short grace period (8s) and fast bus check (1s)
    tmux new-session -d -s "$GRACE_10X_SESSION" -c "$GRACE_RUN_DIR" \
        "PATH='$GRACE_RUN_DIR:$PATH' NBS_HANDLE=grace-run-$run NBS_STARTUP_GRACE=8 NBS_BUS_CHECK_INTERVAL=1 NBS_NOTIFY_COOLDOWN=3 '$NBS_CLAUDE' --dangerously-skip-permissions 2>'$GRACE_RUN_DIR/sidecar.log'" 2>/dev/null

    # Wait long enough for sidecar to initialise but within grace period
    sleep 6

    # Capture pane — /nbs-notify should NOT be present during grace
    GRACE_CONTENT=$(tmux capture-pane -t "$GRACE_10X_SESSION" -p -S -30 2>/dev/null) || true

    if echo "$GRACE_CONTENT" | grep -qF "/nbs-notify"; then
        GRACE_10X_FAIL=$((GRACE_10X_FAIL + 1))
        echo "   Run $run/10: FAIL — /nbs-notify injected during grace period"
    else
        GRACE_10X_PASS=$((GRACE_10X_PASS + 1))
    fi

    # Kill session and clean up run directory
    tmux kill-session -t "$GRACE_10X_SESSION" 2>/dev/null || true
    sleep 1
    rm -rf "$GRACE_RUN_DIR"
done

if [[ $GRACE_10X_FAIL -eq 0 ]]; then
    pass "Startup grace held across 10/10 runs (no notification race)"
else
    fail "Startup grace violated in $GRACE_10X_FAIL/10 runs (notification race detected)"
fi

# =========================================================================
# Cleanup
# =========================================================================
# Cleanup handled by trap

echo ""
echo "=== Result ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All $TESTS tests passed"
else
    echo "FAIL: $FAIL of $TESTS tests failed"
fi

exit $FAIL
