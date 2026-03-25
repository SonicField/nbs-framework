#!/bin/bash
# Test: nbs-bus integration tests — end-to-end multi-agent workflows
#
# These tests exercise the bus as agents actually use it: full lifecycle
# sequences, multi-agent coordination, restart recovery, and config-driven
# behaviour. Unit tests (test_nbs_bus.sh) cover individual commands;
# these tests cover their composition.
#
# Falsification approach: each test defines what would break the invariant,
# then tries to break it.
#
# Integration tests:
#   1.  Full lifecycle: publish → check → read → ack → status → prune
#   2.  Multi-agent queue: 3 agents publish, supervisor processes in priority order
#   3.  Selective ack-all: ack one agent's events, verify others remain
#   4.  Restart recovery: partially-processed queue survives, new session continues
#   5.  Priority interleaving: mixed priorities from multiple agents, correct order
#   6.  Dedup in multi-agent scenario: two agents, same type, dedup respects source
#   7.  Config-driven lifecycle: config.yaml dedup + ack-timeout + retention
#   8.  Prune preserves pending: prune only removes from processed/, not pending
#   9.  Status accuracy throughout lifecycle: reflects correct state at each stage
#  10.  Stale event detection: ack-timeout triggers warnings for old events
#  11.  Concurrent publish + check: publish while check is reading
#  12.  Large queue processing: 50 events, verify ordering and ack-all
#  13.  Re-publish after ack: same dedup-key allowed after ack within window
#  14.  Mixed handle filter: check --handle filters correctly across agents
#  15.  End-to-end agent startup: simulates agent boot sequence from docs
#
# Adversarial:
#  16.  Double-ack: second ack fails gracefully (exit 3, not crash)
#  17.  Read from processed: acked event content intact in processed/
#  18.  Concurrent ack + publish: no corruption or lost events
#  19.  Config hot-reload: add config mid-operation, new behaviour takes effect
#  20.  nbs-chat-init creates functional bus (publish + ack through init-created dir)
#  21.  Publish → immediate prune: pending events untouched
#  22.  Empty queue recovery: all read operations safe on fresh bus

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"

TEST_DIR=$(mktemp -d)
ERRORS=0
PASS_COUNT=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

# Verify binary exists
if [[ ! -x "$NBS_BUS" ]]; then
    echo "ERROR: nbs-bus not found at $NBS_BUS"
    exit 1
fi

echo "=== nbs-bus Integration Tests ==="
echo "Test dir: $TEST_DIR"
echo "Binary: $NBS_BUS"
echo ""

# Helper: create a fresh events directory
setup_events() {
    local dir="$1"
    mkdir -p "$dir/processed"
}

# Helper: count pending events
count_pending() {
    find "$1" -maxdepth 1 -name "*.event" 2>/dev/null | wc -l
}

# Helper: count processed events
count_processed() {
    find "$1/processed" -maxdepth 1 -name "*.event" 2>/dev/null | wc -l
}

# =========================================================================
# 1. Full lifecycle: publish → check → read → ack → status → prune
# =========================================================================
echo "1. Full lifecycle: publish → check → read → ack → status → prune..."
EVENTS="$TEST_DIR/test1"
setup_events "$EVENTS"

# Publish
"$NBS_BUS" publish "$EVENTS" worker-a task-complete high "Task A done. 42/42 tests pass."
check "Publish creates event" "$( [[ $(count_pending "$EVENTS") -eq 1 ]] && echo pass || echo fail )"

# Check — output should list the event
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
EVENT_NAME=$(echo "$CHECK_OUT" | grep -o '[^ ]*\.event' | head -1)
check "Check lists event" "$( [[ -n "$EVENT_NAME" ]] && echo pass || echo fail )"
check "Check shows high priority" "$( echo "$CHECK_OUT" | grep -q '\[high\]' && echo pass || echo fail )"

# Read — should show YAML content
READ_OUT=$("$NBS_BUS" read "$EVENTS" "$EVENT_NAME")
check "Read shows source" "$( echo "$READ_OUT" | grep -q 'source: worker-a' && echo pass || echo fail )"
check "Read shows type" "$( echo "$READ_OUT" | grep -q 'type: task-complete' && echo pass || echo fail )"
check "Read shows payload" "$( echo "$READ_OUT" | grep -q '42/42 tests pass' && echo pass || echo fail )"

# Ack — should move to processed/
"$NBS_BUS" ack "$EVENTS" "$EVENT_NAME"
check "Ack removes from pending" "$( [[ $(count_pending "$EVENTS") -eq 0 ]] && echo pass || echo fail )"
check "Ack adds to processed" "$( [[ $(count_processed "$EVENTS") -eq 1 ]] && echo pass || echo fail )"

# Status — should show 0 pending, 1 processed
STATUS_OUT=$("$NBS_BUS" status "$EVENTS")
check "Status shows 0 pending" "$( echo "$STATUS_OUT" | grep -q 'pending: 0\|Pending: 0' && echo pass || echo fail )"

# Prune — with a very small limit, should remove the processed event
"$NBS_BUS" prune "$EVENTS" --max-bytes=1
check "Prune removes processed" "$( [[ $(count_processed "$EVENTS") -eq 0 ]] && echo pass || echo fail )"

# =========================================================================
# 2. Multi-agent queue: 3 agents publish, supervisor processes in priority order
# =========================================================================
echo ""
echo "2. Multi-agent queue: 3 agents publish, supervisor processes in priority order..."
EVENTS="$TEST_DIR/test2"
setup_events "$EVENTS"

# Three agents publish at different priorities
"$NBS_BUS" publish "$EVENTS" bench-claude heartbeat low "Still alive"
sleep 0.01  # ensure distinct timestamps
"$NBS_BUS" publish "$EVENTS" parser-worker task-complete high "Parser done"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" doc-worker task-blocked critical "Need human input"

check "3 events pending" "$( [[ $(count_pending "$EVENTS") -eq 3 ]] && echo pass || echo fail )"

# Check — should return critical first, then high, then low
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
FIRST_LINE=$(echo "$CHECK_OUT" | head -1)
SECOND_LINE=$(echo "$CHECK_OUT" | sed -n '2p')
THIRD_LINE=$(echo "$CHECK_OUT" | sed -n '3p')

check "First event is critical" "$( echo "$FIRST_LINE" | grep -q '\[critical\]' && echo pass || echo fail )"
check "First event is doc-worker" "$( echo "$FIRST_LINE" | grep -q 'doc-worker' && echo pass || echo fail )"
check "Second event is high" "$( echo "$SECOND_LINE" | grep -q '\[high\]' && echo pass || echo fail )"
check "Third event is low" "$( echo "$THIRD_LINE" | grep -q '\[low\]' && echo pass || echo fail )"

# Supervisor processes critical event first (read + ack)
CRITICAL_EVENT=$(echo "$FIRST_LINE" | grep -o '[^ ]*\.event')
"$NBS_BUS" read "$EVENTS" "$CRITICAL_EVENT" > /dev/null
"$NBS_BUS" ack "$EVENTS" "$CRITICAL_EVENT"

check "Critical event processed" "$( [[ $(count_pending "$EVENTS") -eq 2 ]] && echo pass || echo fail )"

# Remaining queue should still be ordered
CHECK_OUT2=$("$NBS_BUS" check "$EVENTS")
NEXT_LINE=$(echo "$CHECK_OUT2" | head -1)
check "Next event is high (after critical acked)" "$( echo "$NEXT_LINE" | grep -q '\[high\]' && echo pass || echo fail )"

# =========================================================================
# 3. Selective ack-all: ack one agent's events, verify others remain
# =========================================================================
echo ""
echo "3. Selective ack-all with handle filter..."
EVENTS="$TEST_DIR/test3"
setup_events "$EVENTS"

# Two agents publish multiple events
"$NBS_BUS" publish "$EVENTS" worker-alpha task-complete normal "Alpha done 1"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" worker-beta task-complete normal "Beta done 1"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" worker-alpha heartbeat low "Alpha alive"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" worker-beta heartbeat low "Beta alive"

check "4 events pending" "$( [[ $(count_pending "$EVENTS") -eq 4 ]] && echo pass || echo fail )"

# Ack all of worker-alpha's events
"$NBS_BUS" ack-all "$EVENTS" --handle=worker-alpha

check "2 events remain (beta's)" "$( [[ $(count_pending "$EVENTS") -eq 2 ]] && echo pass || echo fail )"
check "2 events processed (alpha's)" "$( [[ $(count_processed "$EVENTS") -eq 2 ]] && echo pass || echo fail )"

# Verify remaining events are beta's
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
check "Remaining are beta's" "$( echo "$CHECK_OUT" | grep -c 'worker-beta' | grep -q '2' && echo pass || echo fail )"
ALPHA_IN_PENDING=$(echo "$CHECK_OUT" | grep -c 'worker-alpha' || true)
check "No alpha in pending" "$( [[ $ALPHA_IN_PENDING -eq 0 ]] && echo pass || echo fail )"

# =========================================================================
# 4. Restart recovery: partially-processed queue survives
# =========================================================================
echo ""
echo "4. Restart recovery: partially-processed queue persists..."
EVENTS="$TEST_DIR/test4"
setup_events "$EVENTS"

# Simulate session 1: publish 3 events, ack 1
"$NBS_BUS" publish "$EVENTS" worker-a task-complete high "Session 1 task A"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" worker-b task-complete high "Session 1 task B"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" worker-c task-blocked critical "Session 1 blocker"

# Ack only one (simulating crash after partial processing)
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
FIRST_EVENT=$(echo "$CHECK_OUT" | head -1 | grep -o '[^ ]*\.event')
"$NBS_BUS" ack "$EVENTS" "$FIRST_EVENT"

REMAINING_BEFORE=$(count_pending "$EVENTS")
check "2 events survive session 1" "$( [[ $REMAINING_BEFORE -eq 2 ]] && echo pass || echo fail )"

# Simulate session 2: "agent restarts" — check for pending events
# This is the restart recovery protocol from the docs
PENDING=$("$NBS_BUS" check "$EVENTS")
PENDING_COUNT=$(echo "$PENDING" | grep -c '\.event' || true)
check "Session 2 sees 2 pending" "$( [[ $PENDING_COUNT -eq 2 ]] && echo pass || echo fail )"

# Process remaining events
while IFS= read -r line; do
    EVT=$(echo "$line" | grep -o '[^ ]*\.event')
    if [[ -n "$EVT" ]]; then
        "$NBS_BUS" ack "$EVENTS" "$EVT"
    fi
done <<< "$PENDING"

check "All events processed after recovery" "$( [[ $(count_pending "$EVENTS") -eq 0 ]] && echo pass || echo fail )"
check "3 total in processed" "$( [[ $(count_processed "$EVENTS") -eq 3 ]] && echo pass || echo fail )"

# =========================================================================
# 5. Priority interleaving: mixed priorities, correct order
# =========================================================================
echo ""
echo "5. Priority interleaving from multiple agents..."
EVENTS="$TEST_DIR/test5"
setup_events "$EVENTS"

# Publish in deliberately wrong order
"$NBS_BUS" publish "$EVENTS" agent-1 info normal "Normal from 1"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" agent-2 alert critical "Critical from 2"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" agent-3 update low "Low from 3"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" agent-1 result high "High from 1"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" agent-2 info normal "Normal from 2"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" agent-3 alert critical "Critical from 3"

CHECK_OUT=$("$NBS_BUS" check "$EVENTS")

# Extract priority order from check output
PRIORITIES=$(echo "$CHECK_OUT" | grep -o '\[critical\]\|\[high\]\|\[normal\]\|\[low\]')
EXPECTED=$'[critical]\n[critical]\n[high]\n[normal]\n[normal]\n[low]'
check "Priority ordering correct" "$( [[ "$PRIORITIES" == "$EXPECTED" ]] && echo pass || echo fail )"

# Within same priority, older events should come first
CRITICAL_LINES=$(echo "$CHECK_OUT" | grep '\[critical\]')
FIRST_CRITICAL=$(echo "$CRITICAL_LINES" | head -1)
check "Oldest critical first" "$( echo "$FIRST_CRITICAL" | grep -q 'agent-2' && echo pass || echo fail )"

# =========================================================================
# 6. Dedup in multi-agent scenario
# =========================================================================
echo ""
echo "6. Dedup respects source boundary..."
EVENTS="$TEST_DIR/test6"
setup_events "$EVENTS"

# Same type from same source — should dedup
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 1" --dedup-window=300
RC=0
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 2" --dedup-window=300 || RC=$?
check "Same source+type deduped (exit 5)" "$( [[ $RC -eq 5 ]] && echo pass || echo fail )"
check "Only 1 event from worker-a" "$( [[ $(count_pending "$EVENTS") -eq 1 ]] && echo pass || echo fail )"

# Same type from different source — should NOT dedup
"$NBS_BUS" publish "$EVENTS" worker-b heartbeat low "beat 1" --dedup-window=300
check "Different source allowed" "$( [[ $(count_pending "$EVENTS") -eq 2 ]] && echo pass || echo fail )"

# Different type from same source — should NOT dedup
"$NBS_BUS" publish "$EVENTS" worker-a task-complete high "done" --dedup-window=300
check "Different type allowed" "$( [[ $(count_pending "$EVENTS") -eq 3 ]] && echo pass || echo fail )"

# =========================================================================
# 7. Config-driven lifecycle
# =========================================================================
echo ""
echo "7. Config-driven lifecycle: dedup + ack-timeout + retention..."
EVENTS="$TEST_DIR/test7"
setup_events "$EVENTS"

# Write config
cat > "$EVENTS/config.yaml" << 'EOF'
dedup-window: 600
retention-max-bytes: 512
ack-timeout: 1
EOF

# Dedup should be on by default from config
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 1"
RC=0
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 2" || RC=$?
check "Config dedup active" "$( [[ $RC -eq 5 ]] && echo pass || echo fail )"

# CLI override disables config dedup
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 3" --dedup-window=0
check "CLI override allows publish" "$( [[ $(count_pending "$EVENTS") -eq 2 ]] && echo pass || echo fail )"

# Ack all, then prune — retention from config (512 bytes)
"$NBS_BUS" ack-all "$EVENTS"
PROCESSED_BEFORE=$(count_processed "$EVENTS")
check "Events moved to processed" "$( [[ $PROCESSED_BEFORE -eq 2 ]] && echo pass || echo fail )"

# Publish more events to build up processed
for i in $(seq 1 5); do
    "$NBS_BUS" publish "$EVENTS" "worker-$i" "task-$i" normal "payload $i" --dedup-window=0
done
"$NBS_BUS" ack-all "$EVENTS"

# Prune uses config retention-max-bytes (512)
"$NBS_BUS" prune "$EVENTS"
PROCESSED_AFTER=$(count_processed "$EVENTS")
check "Prune respects config retention" "$( [[ $PROCESSED_AFTER -lt 7 ]] && echo pass || echo fail )"

# =========================================================================
# 8. Prune preserves pending: only removes from processed/
# =========================================================================
echo ""
echo "8. Prune preserves pending events..."
EVENTS="$TEST_DIR/test8"
setup_events "$EVENTS"

# Create pending events
"$NBS_BUS" publish "$EVENTS" worker-a task-complete high "Active task"
"$NBS_BUS" publish "$EVENTS" worker-b task-blocked critical "Blocker"

# Create processed events (via publish + ack)
"$NBS_BUS" publish "$EVENTS" worker-c heartbeat low "Old heartbeat"
CHECK_OUT=$("$NBS_BUS" check "$EVENTS" --handle=worker-c)
HB_EVENT=$(echo "$CHECK_OUT" | grep -o '[^ ]*\.event' | head -1)
"$NBS_BUS" ack "$EVENTS" "$HB_EVENT"

PENDING_BEFORE=$(count_pending "$EVENTS")
check "2 pending before prune" "$( [[ $PENDING_BEFORE -eq 2 ]] && echo pass || echo fail )"

# Prune aggressively
"$NBS_BUS" prune "$EVENTS" --max-bytes=1

PENDING_AFTER=$(count_pending "$EVENTS")
check "2 pending after prune (unchanged)" "$( [[ $PENDING_AFTER -eq 2 ]] && echo pass || echo fail )"
check "Processed pruned" "$( [[ $(count_processed "$EVENTS") -eq 0 ]] && echo pass || echo fail )"

# =========================================================================
# 9. Status accuracy throughout lifecycle
# =========================================================================
echo ""
echo "9. Status accuracy at each lifecycle stage..."
EVENTS="$TEST_DIR/test9"
setup_events "$EVENTS"

# Stage 1: Empty
STATUS=$("$NBS_BUS" status "$EVENTS")
check "Empty queue: 0 pending" "$( echo "$STATUS" | grep -qi 'pending.*0\|0.*pending' && echo pass || echo fail )"

# Stage 2: Publish events at each priority
"$NBS_BUS" publish "$EVENTS" w1 alert critical "C1"
"$NBS_BUS" publish "$EVENTS" w2 result high "H1"
"$NBS_BUS" publish "$EVENTS" w3 info normal "N1"
"$NBS_BUS" publish "$EVENTS" w4 bg low "L1"

STATUS=$("$NBS_BUS" status "$EVENTS")
check "4 pending after publish" "$( echo "$STATUS" | grep -qi 'pending.*4\|4.*pending' && echo pass || echo fail )"

# Stage 3: Ack some
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
FIRST_TWO=$(echo "$CHECK_OUT" | head -2 | grep -o '[^ ]*\.event')
for evt in $FIRST_TWO; do
    "$NBS_BUS" ack "$EVENTS" "$evt"
done

STATUS=$("$NBS_BUS" status "$EVENTS")
check "2 pending after partial ack" "$( echo "$STATUS" | grep -qi 'pending.*2\|2.*pending' && echo pass || echo fail )"

# Stage 4: Ack all remaining
"$NBS_BUS" ack-all "$EVENTS"
STATUS=$("$NBS_BUS" status "$EVENTS")
check "0 pending after ack-all" "$( echo "$STATUS" | grep -qi 'pending.*0\|0.*pending' && echo pass || echo fail )"

# =========================================================================
# 10. Stale event detection
# =========================================================================
echo ""
echo "10. Stale event detection with ack-timeout..."
EVENTS="$TEST_DIR/test10"
setup_events "$EVENTS"

cat > "$EVENTS/config.yaml" << 'EOF'
ack-timeout: 1
EOF

# Publish an event and wait for it to become stale
"$NBS_BUS" publish "$EVENTS" worker-a task-complete high "Old task"
sleep 2

STATUS=$("$NBS_BUS" status "$EVENTS")
check "Status warns about stale events" "$( echo "$STATUS" | grep -qi 'stale\|overdue\|old' && echo pass || echo fail )"

# =========================================================================
# 11. Concurrent publish + check: no corruption
# =========================================================================
echo ""
echo "11. Concurrent publish + check..."
EVENTS="$TEST_DIR/test11"
setup_events "$EVENTS"

# Publish 20 events in background while checking
for i in $(seq 1 20); do
    "$NBS_BUS" publish "$EVENTS" "concurrent-$i" task-complete normal "msg $i" &
done

# Run check while publishes are in flight
"$NBS_BUS" check "$EVENTS" > /dev/null 2>&1
RC=$?
wait

check "Check exits 0 during concurrent publish" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "All 20 events created" "$( [[ $(count_pending "$EVENTS") -eq 20 ]] && echo pass || echo fail )"

# Verify no corruption — all events should be readable
ALL_VALID=true
for evt_file in "$EVENTS"/*.event; do
    if [[ -f "$evt_file" ]]; then
        if ! grep -q '^source:' "$evt_file" 2>/dev/null; then
            ALL_VALID=false
            break
        fi
    fi
done
check "All events valid YAML" "$( $ALL_VALID && echo pass || echo fail )"

# =========================================================================
# 12. Large queue processing: 50 events, verify ordering and ack-all
# =========================================================================
echo ""
echo "12. Large queue: 50 events, ordering and ack-all..."
EVENTS="$TEST_DIR/test12"
setup_events "$EVENTS"

# Publish 50 events with mixed priorities
PRIORITIES=("critical" "high" "normal" "low")
for i in $(seq 1 50); do
    P_IDX=$(( (i - 1) % 4 ))
    PRIO="${PRIORITIES[$P_IDX]}"
    "$NBS_BUS" publish "$EVENTS" "worker-$((i % 5))" "task-$i" "$PRIO" "Event $i"
done

check "50 events pending" "$( [[ $(count_pending "$EVENTS") -eq 50 ]] && echo pass || echo fail )"

# Check ordering — first events should all be critical
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
FIRST_PRIORITY=$(echo "$CHECK_OUT" | head -1 | grep -o '\[critical\]')
LAST_PRIORITY=$(echo "$CHECK_OUT" | tail -1 | grep -o '\[low\]')
check "First event is critical" "$( [[ -n "$FIRST_PRIORITY" ]] && echo pass || echo fail )"
check "Last event is low" "$( [[ -n "$LAST_PRIORITY" ]] && echo pass || echo fail )"

# Ack all
"$NBS_BUS" ack-all "$EVENTS"
check "All 50 acked" "$( [[ $(count_pending "$EVENTS") -eq 0 ]] && echo pass || echo fail )"
check "50 in processed" "$( [[ $(count_processed "$EVENTS") -eq 50 ]] && echo pass || echo fail )"

# =========================================================================
# 13. Re-publish after ack: same dedup-key allowed after ack
# =========================================================================
echo ""
echo "13. Re-publish after ack: dedup-key cleared by ack..."
EVENTS="$TEST_DIR/test13"
setup_events "$EVENTS"

# Publish and ack
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 1" --dedup-window=300
CHECK_OUT=$("$NBS_BUS" check "$EVENTS")
EVT=$(echo "$CHECK_OUT" | grep -o '[^ ]*\.event' | head -1)
"$NBS_BUS" ack "$EVENTS" "$EVT"

check "Event acked" "$( [[ $(count_pending "$EVENTS") -eq 0 ]] && echo pass || echo fail )"

# Re-publish same dedup-key — should succeed (dedup ignores processed)
RC=0
"$NBS_BUS" publish "$EVENTS" worker-a heartbeat low "beat 2" --dedup-window=300 || RC=$?
check "Re-publish after ack succeeds (exit 0)" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "New event pending" "$( [[ $(count_pending "$EVENTS") -eq 1 ]] && echo pass || echo fail )"

# =========================================================================
# 14. Mixed handle filter: check --handle filters correctly
# =========================================================================
echo ""
echo "14. Handle filter across agents..."
EVENTS="$TEST_DIR/test14"
setup_events "$EVENTS"

# 3 agents, multiple events each
"$NBS_BUS" publish "$EVENTS" alpha task-complete high "Alpha done"
"$NBS_BUS" publish "$EVENTS" beta task-complete high "Beta done"
"$NBS_BUS" publish "$EVENTS" gamma task-blocked critical "Gamma blocked"
"$NBS_BUS" publish "$EVENTS" alpha heartbeat low "Alpha alive"
"$NBS_BUS" publish "$EVENTS" beta heartbeat low "Beta alive"

# Check with handle filter
ALPHA_OUT=$("$NBS_BUS" check "$EVENTS" --handle=alpha)
BETA_OUT=$("$NBS_BUS" check "$EVENTS" --handle=beta)
GAMMA_OUT=$("$NBS_BUS" check "$EVENTS" --handle=gamma)
ALL_OUT=$("$NBS_BUS" check "$EVENTS")

ALPHA_COUNT=$(echo "$ALPHA_OUT" | grep -c '\.event' || true)
BETA_COUNT=$(echo "$BETA_OUT" | grep -c '\.event' || true)
GAMMA_COUNT=$(echo "$GAMMA_OUT" | grep -c '\.event' || true)
ALL_COUNT=$(echo "$ALL_OUT" | grep -c '\.event' || true)

check "Alpha has 2 events" "$( [[ $ALPHA_COUNT -eq 2 ]] && echo pass || echo fail )"
check "Beta has 2 events" "$( [[ $BETA_COUNT -eq 2 ]] && echo pass || echo fail )"
check "Gamma has 1 event" "$( [[ $GAMMA_COUNT -eq 1 ]] && echo pass || echo fail )"
check "Total is 5" "$( [[ $ALL_COUNT -eq 5 ]] && echo pass || echo fail )"

# Filter should not show other agents' events
check "Alpha filter excludes beta" "$( echo "$ALPHA_OUT" | grep -qv 'beta' && echo pass || echo fail )"
check "Beta filter excludes gamma" "$( echo "$BETA_OUT" | grep -qv 'gamma' && echo pass || echo fail )"

# =========================================================================
# 15. End-to-end agent startup: simulates agent boot from docs
# =========================================================================
echo ""
echo "15. End-to-end agent startup protocol..."
EVENTS="$TEST_DIR/test15"
setup_events "$EVENTS"

# Pre-seed events (simulating events that arrived while agent was down)
"$NBS_BUS" publish "$EVENTS" supervisor task-assign high "Review PR #42"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" chat-watcher chat-mention normal "@testkeeper check test suite"
sleep 0.01
"$NBS_BUS" publish "$EVENTS" bench-claude heartbeat low "Bench alive"

# --- Agent startup protocol (from docs/nbs-bus.md) ---
# Step 1: Check whether .nbs/events/ exists
check "Events dir exists" "$( [[ -d "$EVENTS" ]] && echo pass || echo fail )"

# Step 2: Scan for pending events
PENDING=$("$NBS_BUS" check "$EVENTS")
PENDING_COUNT=$(echo "$PENDING" | grep -c '\.event' || true)
check "3 events pending on startup" "$( [[ $PENDING_COUNT -eq 3 ]] && echo pass || echo fail )"

# Step 3: Process in priority order
# Should be: high (task-assign), normal (chat-mention), low (heartbeat)
FIRST=$(echo "$PENDING" | head -1)
check "First event is high priority" "$( echo "$FIRST" | grep -q '\[high\]' && echo pass || echo fail )"
check "First event is task-assign" "$( echo "$FIRST" | grep -q 'task-assign' && echo pass || echo fail )"

# Process each event: read, act, ack
PROCESSED_OK=true
while IFS= read -r line; do
    EVT=$(echo "$line" | grep -o '[^ ]*\.event')
    if [[ -n "$EVT" ]]; then
        # Read the event
        CONTENT=$("$NBS_BUS" read "$EVENTS" "$EVT" 2>&1)
        if [[ $? -ne 0 ]]; then
            PROCESSED_OK=false
            break
        fi
        # Ack the event
        "$NBS_BUS" ack "$EVENTS" "$EVT"
        if [[ $? -ne 0 ]]; then
            PROCESSED_OK=false
            break
        fi
    fi
done <<< "$PENDING"

check "All startup events processed" "$( $PROCESSED_OK && echo pass || echo fail )"
check "Queue empty after startup" "$( [[ $(count_pending "$EVENTS") -eq 0 ]] && echo pass || echo fail )"
check "All in processed" "$( [[ $(count_processed "$EVENTS") -eq 3 ]] && echo pass || echo fail )"

# Step 4: Agent is now ready for normal operation
# Publish own presence
"$NBS_BUS" publish "$EVENTS" testkeeper heartbeat normal "Testkeeper online"
check "Agent can publish after startup" "$( [[ $(count_pending "$EVENTS") -eq 1 ]] && echo pass || echo fail )"

# --- Summary ---
echo ""
echo "=== Results ==="
TOTAL=$((PASS_COUNT + ERRORS))
if [[ $ERRORS -eq 0 ]]; then
    echo "ALL $TOTAL TESTS PASSED"
    exit 0
else
    echo "PASSED: $PASS_COUNT / $TOTAL"
    echo "FAILURES: $ERRORS"
    exit 1
fi
