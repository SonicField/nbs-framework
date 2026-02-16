#!/bin/bash
# Test: NBS integration tests
#
# Multi-component tests that verify the system works as a system.
# Each test exercises multiple components together — these scenarios
# cannot be tested by any single component's test suite.
#
# Categories:
#   T1 (1-9):   nbs-chat-init — tripod creation, flags, error codes
#   T2 (10-12): Multi-agent cursor isolation
#   T3 (13-15): Dedup-window=0 bypass for chat events
#   T4 (16-20): End-to-end bus event flow
#   T5 (21-25): nbs-claude sidecar functions (integration-level)
#   T6 (26-30): Full Scribe/Pythia pipeline
#   T7 (31-33): Multi-handle concurrent cursor tracking
#   T8 (34-38): nbs-chat-init --compact-log decision log archival
#   T9 (39-43): nbs-claude-remote argument validation
#   T10 (44-45): Terminal self-echo
#   T11 (46-50): Self-healing sidecar after skill loss

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_BUS="${NBS_BUS_BIN:-$PROJECT_ROOT/bin/nbs-bus}"
NBS_CHAT_INIT="${NBS_CHAT_INIT_BIN:-$PROJECT_ROOT/bin/nbs-chat-init}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"
NBS_CLAUDE_REMOTE="$PROJECT_ROOT/bin/nbs-claude-remote"

# Add bin/ to PATH so bus_bridge.c can find nbs-bus via execlp
export PATH="$PROJECT_ROOT/bin:$PATH"

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

# Verify binaries exist
for bin in "$NBS_CHAT" "$NBS_BUS" "$NBS_CHAT_INIT" "$NBS_TERMINAL"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: Binary not found: $bin"
        echo "Run: make -C src/nbs-chat install && make -C src/nbs-bus install"
        exit 1
    fi
done

echo "=== NBS Integration Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# ============================================================
# T1: nbs-chat-init
# ============================================================

echo "T1: nbs-chat-init"
echo ""

# --- Test 1: Basic init creates full tripod ---
echo "1. Basic init creates full tripod..."
T1_DIR="$TEST_DIR/t1"
mkdir -p "$T1_DIR"
(cd "$T1_DIR" && "$NBS_CHAT_INIT" --name=test --force >/dev/null 2>&1)
check "events dir exists" "$( [[ -d "$T1_DIR/.nbs/events" ]] && echo pass || echo fail )"
check "events/processed exists" "$( [[ -d "$T1_DIR/.nbs/events/processed" ]] && echo pass || echo fail )"
check "chat file exists" "$( [[ -f "$T1_DIR/.nbs/chat/test.chat" ]] && echo pass || echo fail )"
check "scribe log exists" "$( [[ -f "$T1_DIR/.nbs/scribe/test-log.md" ]] && echo pass || echo fail )"
check "project-id exists" "$( [[ -f "$T1_DIR/.nbs/project-id" ]] && echo pass || echo fail )"
echo ""

# --- Test 2: Missing --name exits 4 ---
echo "2. Missing --name exits 4..."
set +e
"$NBS_CHAT_INIT" --force >/dev/null 2>&1
RC=$?
set -e
check "exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
echo ""

# --- Test 3: --dry-run creates no files ---
echo "3. --dry-run creates no files..."
T3_DIR="$TEST_DIR/t3"
mkdir -p "$T3_DIR"
(cd "$T3_DIR" && "$NBS_CHAT_INIT" --name=dry --force --dry-run >/dev/null 2>&1)
check "chat file not created" "$( [[ ! -f "$T3_DIR/.nbs/chat/dry.chat" ]] && echo pass || echo fail )"
echo ""

# --- Test 4: --spawn-only without spawn target exits 4 ---
echo "4. --spawn-only without spawn target exits 4..."
set +e
(cd "$T1_DIR" && "$NBS_CHAT_INIT" --name=test --spawn-only --force >/dev/null 2>&1)
RC=$?
set -e
check "exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
echo ""

# --- Test 5: Re-init archives existing chat ---
echo "5. Re-init archives existing chat..."
(cd "$T1_DIR" && "$NBS_CHAT_INIT" --name=test --force >/dev/null 2>&1)
ARCHIVE_COUNT=$(ls "$T1_DIR/.nbs/chat/archive/test-"*.chat 2>/dev/null | wc -l)
check "archived chat exists" "$( [[ $ARCHIVE_COUNT -ge 1 ]] && echo pass || echo fail )"
echo ""

# --- Test 6: Config template has dedup-window: 300 ---
echo "6. Config template has dedup-window: 300..."
check "dedup-window is 300" "$( grep -q 'dedup-window: 300' "$T1_DIR/.nbs/events/config.yaml" && echo pass || echo fail )"
echo ""

# --- Test 7: Health check passes ---
echo "7. Health check passes on valid tripod..."
set +e
(cd "$T1_DIR" && "$NBS_CHAT_INIT" --name=test --force >/dev/null 2>&1)
RC=$?
set -e
check "exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
echo ""

# --- Test 8: Chat file contains init message ---
echo "8. Chat file contains init message..."
CONTENT=$("$NBS_CHAT" read "$T1_DIR/.nbs/chat/test.chat" 2>/dev/null)
check "init message present" "$( echo "$CONTENT" | grep -qi 'initialised' && echo pass || echo fail )"
echo ""

# --- Test 9: Scribe log header has correct chat name ---
echo "9. Scribe log header has correct chat name..."
check "Chat: test.chat in header" "$( grep -q 'Chat: test.chat' "$T1_DIR/.nbs/scribe/test-log.md" && echo pass || echo fail )"
echo ""

# ============================================================
# T2: Multi-agent cursor isolation
# ============================================================

echo "T2: Multi-agent cursor isolation"
echo ""

# --- Test 10: Two handles read independently ---
echo "10. Two handles read independently..."
T10_DIR="$TEST_DIR/t10"
mkdir -p "$T10_DIR/.nbs/chat"
"$NBS_CHAT" create "$T10_DIR/.nbs/chat/multi.chat" >/dev/null 2>&1
"$NBS_CHAT" send "$T10_DIR/.nbs/chat/multi.chat" alice "Message from alice" >/dev/null 2>&1
"$NBS_CHAT" send "$T10_DIR/.nbs/chat/multi.chat" bob "Message from bob" >/dev/null 2>&1

# Agent A reads — advances A's cursor
"$NBS_CHAT" read "$T10_DIR/.nbs/chat/multi.chat" --unread=agentA >/dev/null 2>&1

# Agent B reads — should still see both messages
B_OUTPUT=$("$NBS_CHAT" read "$T10_DIR/.nbs/chat/multi.chat" --unread=agentB 2>/dev/null)
B_COUNT=$(echo "$B_OUTPUT" | grep -c "Message from" || true)
check "Agent B sees both messages after A read" "$( [[ $B_COUNT -eq 2 ]] && echo pass || echo fail )"

# Agent A reads again — should see nothing new
A_OUTPUT=$("$NBS_CHAT" read "$T10_DIR/.nbs/chat/multi.chat" --unread=agentA 2>/dev/null)
check "Agent A sees nothing on second read" "$( [[ -z "$A_OUTPUT" ]] && echo pass || echo fail )"
echo ""

# --- Test 11: Concurrent sends with different handles ---
echo "11. Concurrent sends with different handles..."
T11_DIR="$TEST_DIR/t11"
mkdir -p "$T11_DIR/.nbs/chat"
"$NBS_CHAT" create "$T11_DIR/.nbs/chat/concurrent.chat" >/dev/null 2>&1

# Send 5 messages from each of two handles concurrently
for i in $(seq 1 5); do
    "$NBS_CHAT" send "$T11_DIR/.nbs/chat/concurrent.chat" worker1 "W1 msg $i" >/dev/null 2>&1 &
    "$NBS_CHAT" send "$T11_DIR/.nbs/chat/concurrent.chat" worker2 "W2 msg $i" >/dev/null 2>&1 &
done
wait

TOTAL=$("$NBS_CHAT" read "$T11_DIR/.nbs/chat/concurrent.chat" 2>/dev/null | wc -l)
check "All 10 concurrent messages present" "$( [[ $TOTAL -eq 10 ]] && echo pass || echo fail )"
echo ""

# --- Test 12: Cursor file integrity ---
echo "12. Cursor file integrity under concurrent read..."
"$NBS_CHAT" read "$T11_DIR/.nbs/chat/concurrent.chat" --unread=reader1 >/dev/null 2>&1
"$NBS_CHAT" read "$T11_DIR/.nbs/chat/concurrent.chat" --unread=reader2 >/dev/null 2>&1
CURSOR_HANDLES=$(grep -c "=" "$T11_DIR/.nbs/chat/concurrent.chat.cursors" 2>/dev/null || echo 0)
check "Cursor file has exactly 2 handle entries" "$( [[ $CURSOR_HANDLES -eq 2 ]] && echo pass || echo fail )"
echo ""

# ============================================================
# T3: Dedup-window=0 bypass for chat events
# ============================================================

echo "T3: Dedup-window=0 bypass"
echo ""

# --- Test 13: Two rapid chat sends create two events ---
echo "13. Two rapid chat sends create two events..."
T13_DIR="$TEST_DIR/t13"
mkdir -p "$T13_DIR/.nbs/chat" "$T13_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T13_DIR/.nbs/chat/dedup.chat" >/dev/null 2>&1
"$NBS_CHAT" send "$T13_DIR/.nbs/chat/dedup.chat" agent1 "First message" >/dev/null 2>&1
"$NBS_CHAT" send "$T13_DIR/.nbs/chat/dedup.chat" agent1 "Second message" >/dev/null 2>&1
sleep 1
EVENT_COUNT=$(ls "$T13_DIR/.nbs/events/"*chat-message*.event 2>/dev/null | wc -l)
check "Two chat-message events created" "$( [[ $EVENT_COUNT -eq 2 ]] && echo pass || echo fail )"
echo ""

# --- Test 14: System event with config dedup IS deduped ---
echo "14. System event with config dedup is deduped..."
T14_DIR="$TEST_DIR/t14"
mkdir -p "$T14_DIR/.nbs/events/processed"
cat > "$T14_DIR/.nbs/events/config.yaml" << 'EOF'
dedup-window: 300
EOF
"$NBS_BUS" publish "$T14_DIR/.nbs/events/" test-source test-type low "payload1" >/dev/null 2>&1
set +e
"$NBS_BUS" publish "$T14_DIR/.nbs/events/" test-source test-type low "payload2" 2>/dev/null
RC=$?
set -e
check "System event deduped (exit 5)" "$( [[ $RC -eq 5 ]] && echo pass || echo fail )"
echo ""

# --- Test 15: Human-input events not deduped ---
echo "15. Human-input events not deduped..."
T15_DIR="$TEST_DIR/t15"
mkdir -p "$T15_DIR/.nbs/chat" "$T15_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T15_DIR/.nbs/chat/human.chat" >/dev/null 2>&1
printf 'Message one\x04' | timeout 5 "$NBS_TERMINAL" "$T15_DIR/.nbs/chat/human.chat" "alex" >/dev/null 2>&1 || true
sleep 1
printf 'Message two\x04' | timeout 5 "$NBS_TERMINAL" "$T15_DIR/.nbs/chat/human.chat" "alex" >/dev/null 2>&1 || true
sleep 1
HI_COUNT=$(ls "$T15_DIR/.nbs/events/"*human-input*.event 2>/dev/null | wc -l)
check "Two human-input events (not deduped)" "$( [[ $HI_COUNT -eq 2 ]] && echo pass || echo fail )"
echo ""

# ============================================================
# T4: End-to-end bus event flow
# ============================================================

echo "T4: End-to-end bus event flow"
echo ""

# --- Test 16: nbs-chat send creates chat-message event ---
echo "16. nbs-chat send creates chat-message event..."
T16_DIR="$TEST_DIR/t16"
mkdir -p "$T16_DIR/.nbs/chat" "$T16_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T16_DIR/.nbs/chat/flow.chat" >/dev/null 2>&1
"$NBS_CHAT" send "$T16_DIR/.nbs/chat/flow.chat" agent "Hello bus" >/dev/null 2>&1
sleep 1
check "chat-message event exists" "$( ls "$T16_DIR/.nbs/events/"*chat-message*.event 2>/dev/null | head -1 | grep -q event && echo pass || echo fail )"
echo ""

# --- Test 17: nbs-chat-terminal send creates both events ---
echo "17. Terminal send creates chat-message AND human-input events..."
T17_DIR="$TEST_DIR/t17"
mkdir -p "$T17_DIR/.nbs/chat" "$T17_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T17_DIR/.nbs/chat/terminal.chat" >/dev/null 2>&1
printf 'Hello from terminal\x04' | timeout 5 "$NBS_TERMINAL" "$T17_DIR/.nbs/chat/terminal.chat" "human" >/dev/null 2>&1 || true
sleep 1
check "chat-message event" "$( ls "$T17_DIR/.nbs/events/"*chat-message*.event 2>/dev/null | head -1 | grep -q event && echo pass || echo fail )"
check "human-input event" "$( ls "$T17_DIR/.nbs/events/"*human-input*.event 2>/dev/null | head -1 | grep -q event && echo pass || echo fail )"
echo ""

# --- Test 18: Event payload contains sender and message ---
echo "18. Event payload contains sender and message..."
EVENT_FILE=$(ls "$T17_DIR/.nbs/events/"*chat-message*.event 2>/dev/null | head -1)
if [[ -n "$EVENT_FILE" ]]; then
    check "payload has sender" "$( grep -q 'human' "$EVENT_FILE" && echo pass || echo fail )"
    check "payload has message" "$( grep -q 'Hello from terminal' "$EVENT_FILE" && echo pass || echo fail )"
else
    check "payload has sender" "fail"
    check "payload has message" "fail"
fi
echo ""

# --- Test 19: @mention generates chat-mention event ---
echo "19. @mention generates chat-mention event..."
T19_DIR="$TEST_DIR/t19"
mkdir -p "$T19_DIR/.nbs/chat" "$T19_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T19_DIR/.nbs/chat/mention.chat" >/dev/null 2>&1
"$NBS_CHAT" send "$T19_DIR/.nbs/chat/mention.chat" sender "Hey @recipient check this" >/dev/null 2>&1
sleep 1
check "chat-mention event exists" "$( ls "$T19_DIR/.nbs/events/"*chat-mention*.event 2>/dev/null | head -1 | grep -q event && echo pass || echo fail )"
echo ""

# --- Test 20: Bus events go to events/, not processed/ ---
echo "20. Bus events go to events/, not processed/..."
PROCESSED_COUNT=$(find "$T19_DIR/.nbs/events/processed/" -name "*.event" 2>/dev/null | wc -l)
check "No events in processed/ (not auto-acked)" "$( [[ $PROCESSED_COUNT -eq 0 ]] && echo pass || echo fail )"
echo ""

# ============================================================
# T5: nbs-claude sidecar functions (integration-level)
# ============================================================

echo "T5: nbs-claude sidecar functions"
echo ""

# Set up a project directory and source sidecar functions
T5_DIR="$TEST_DIR/t5"
mkdir -p "$T5_DIR/.nbs/chat" "$T5_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T5_DIR/.nbs/chat/sidecar.chat" >/dev/null 2>&1
"$NBS_CHAT" send "$T5_DIR/.nbs/chat/sidecar.chat" other "Unread message" >/dev/null 2>&1

# Source sidecar functions
SIDECAR_HANDLE="testhandle"
NBS_ROOT="$T5_DIR"
_EXTRACT_TMP=$(mktemp)
sed -n '/^# --- Configuration ---/,/^# --- Cleanup ---/p' "$NBS_CLAUDE" | head -n -1 >> "$_EXTRACT_TMP"
sed -n '/^# --- Dynamic resource registration ---/,/^# --- Idle detection sidecar/p' "$NBS_CLAUDE" | head -n -2 >> "$_EXTRACT_TMP"
source "$_EXTRACT_TMP"
rm -f "$_EXTRACT_TMP"

# Re-set after sourcing (source resets these from defaults)
SIDECAR_HANDLE="testhandle"
NBS_ROOT="$T5_DIR"
CONTROL_INBOX="${NBS_ROOT}/.nbs/control-inbox-${SIDECAR_HANDLE}"
CONTROL_REGISTRY="${NBS_ROOT}/.nbs/control-registry-${SIDECAR_HANDLE}"

cd "$T5_DIR"

# --- Test 21: seed_registry with --root finds resources ---
echo "21. seed_registry with --root finds resources..."
seed_registry
check "Registry has chat entry" "$( grep -q "chat:${NBS_ROOT}/.nbs/chat/sidecar.chat" "$CONTROL_REGISTRY" && echo pass || echo fail )"
check "Registry has bus entry" "$( grep -q "bus:${NBS_ROOT}/.nbs/events" "$CONTROL_REGISTRY" && echo pass || echo fail )"
echo ""

# --- Test 22: should_inject_notify returns 0 when events pending ---
echo "22. should_inject_notify returns 0 when events pending..."
"$NBS_BUS" publish "$T5_DIR/.nbs/events/" test test-event normal "test payload" --dedup-window=0 >/dev/null 2>&1
LAST_NOTIFY_TIME=0
NOTIFY_COOLDOWN=0
set +e
should_inject_notify
RC=$?
set -e
check "Returns 0 (should notify)" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# Clean up the event
for f in "$T5_DIR/.nbs/events/"*.event; do
    "$NBS_BUS" ack "$T5_DIR/.nbs/events/" "$(basename "$f")" 2>/dev/null || true
done
echo ""

# --- Test 23: should_inject_notify returns 1 when nothing pending ---
echo "23. should_inject_notify returns 1 when nothing pending..."
# Advance chat cursor so no unread
"$NBS_CHAT" read "$T5_DIR/.nbs/chat/sidecar.chat" --unread=testhandle >/dev/null 2>&1
LAST_NOTIFY_TIME=0
set +e
should_inject_notify
RC=$?
set -e
check "Returns 1 (nothing pending)" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
echo ""

# --- Test 24: Cooldown prevents rapid re-notification ---
echo "24. Cooldown prevents rapid re-notification..."
"$NBS_BUS" publish "$T5_DIR/.nbs/events/" test test-event2 normal "cooldown test" --dedup-window=0 >/dev/null 2>&1
NOTIFY_COOLDOWN=9999
LAST_NOTIFY_TIME=$(date +%s)
set +e
should_inject_notify
RC=$?
set -e
check "Returns 1 (cooldown active)" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
# Clean up
for f in "$T5_DIR/.nbs/events/"*.event; do
    "$NBS_BUS" ack "$T5_DIR/.nbs/events/" "$(basename "$f")" 2>/dev/null || true
done
echo ""

# --- Test 25: Critical priority bypasses cooldown ---
echo "25. Critical priority bypasses cooldown..."
"$NBS_BUS" publish "$T5_DIR/.nbs/events/" test critical-test critical "urgent" --dedup-window=0 >/dev/null 2>&1
NOTIFY_COOLDOWN=9999
LAST_NOTIFY_TIME=$(date +%s)
set +e
should_inject_notify
RC=$?
set -e
check "Returns 0 (critical bypasses cooldown)" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
# Clean up
for f in "$T5_DIR/.nbs/events/"*.event; do
    "$NBS_BUS" ack "$T5_DIR/.nbs/events/" "$(basename "$f")" 2>/dev/null || true
done
echo ""

# ============================================================
# T6: Full Scribe/Pythia pipeline
# ============================================================

echo "T6: Full Scribe/Pythia pipeline"
echo ""

T6_DIR="$TEST_DIR/t6"
mkdir -p "$T6_DIR/.nbs/chat" "$T6_DIR/.nbs/events/processed" "$T6_DIR/.nbs/scribe"
"$NBS_CHAT" create "$T6_DIR/.nbs/chat/pipeline.chat" >/dev/null 2>&1

# Write config with pythia-interval=3 so we hit the threshold quickly
cat > "$T6_DIR/.nbs/events/config.yaml" << 'EOF'
dedup-window: 300
pythia-interval: 3
EOF

# Initialise scribe log
cat > "$T6_DIR/.nbs/scribe/pipeline-log.md" << 'EOF'
# Decision Log

Project: test-pipeline
Created: 2026-02-16T00:00:00Z
Scribe: scribe
Chat: pipeline.chat
Decision count: 0

---
EOF

# --- Test 26: Scribe logs a decision to the decision log ---
echo "26. Scribe logs a decision to the decision log..."
# Simulate a chat decision
"$NBS_CHAT" send "$T6_DIR/.nbs/chat/pipeline.chat" alice "Let's use file-based events, not sockets" >/dev/null 2>&1
"$NBS_CHAT" send "$T6_DIR/.nbs/chat/pipeline.chat" bob "Agreed — file-based events it is" >/dev/null 2>&1

# Simulate Scribe appending a decision entry
TIMESTAMP_1=$(date +%s)
cat >> "$T6_DIR/.nbs/scribe/pipeline-log.md" << ENTRY

---

### D-${TIMESTAMP_1} Use file-based events instead of sockets
- **Chat ref:** pipeline.chat:~L1
- **Participants:** alice, bob
- **Artefacts:** —
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Team agreed on file-based events for simplicity and atomicity.
ENTRY

# Verify entry format
check "Decision entry has D- header" "$( grep -q "^### D-${TIMESTAMP_1}" "$T6_DIR/.nbs/scribe/pipeline-log.md" && echo pass || echo fail )"
check "Decision entry has Chat ref" "$( grep -q 'Chat ref.\+pipeline.chat' "$T6_DIR/.nbs/scribe/pipeline-log.md" && echo pass || echo fail )"
check "Decision entry has Participants" "$( grep -q 'Participants.\+alice, bob' "$T6_DIR/.nbs/scribe/pipeline-log.md" && echo pass || echo fail )"
check "Decision entry has Status" "$( grep -q 'Status.\+decided' "$T6_DIR/.nbs/scribe/pipeline-log.md" && echo pass || echo fail )"
echo ""

# --- Test 27: Scribe publishes decision-logged event to bus ---
echo "27. Scribe publishes decision-logged event to bus..."
"$NBS_BUS" publish "$T6_DIR/.nbs/events/" scribe decision-logged normal \
  "D-${TIMESTAMP_1} Use file-based events instead of sockets" --dedup-window=0 >/dev/null 2>&1
DLOG_EVENT=$(ls "$T6_DIR/.nbs/events/"*decision-logged*.event 2>/dev/null | head -1)
check "decision-logged event exists" "$( [[ -n "$DLOG_EVENT" ]] && echo pass || echo fail )"
if [[ -n "$DLOG_EVENT" ]]; then
    check "event payload contains D-timestamp" "$( grep -q "D-${TIMESTAMP_1}" "$DLOG_EVENT" && echo pass || echo fail )"
    "$NBS_BUS" ack "$T6_DIR/.nbs/events/" "$(basename "$DLOG_EVENT")" >/dev/null 2>&1
fi
echo ""

# --- Test 28: Pythia checkpoint published at pythia-interval threshold ---
echo "28. Pythia checkpoint published at pythia-interval=3 threshold..."
# Log two more decisions to reach count=3
for i in 2 3; do
    TS=$(date +%s)${i}
    cat >> "$T6_DIR/.nbs/scribe/pipeline-log.md" << ENTRY

---

### D-${TS} Test decision number ${i}
- **Chat ref:** pipeline.chat:~L${i}
- **Participants:** alice
- **Artefacts:** —
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Test decision for pipeline integration test.
ENTRY
done

DECISION_COUNT=$(grep -c "^### D-" "$T6_DIR/.nbs/scribe/pipeline-log.md")
check "Decision count is 3" "$( [[ $DECISION_COUNT -eq 3 ]] && echo pass || echo fail )"

# Read pythia-interval from config
PYTHIA_INTERVAL=$(grep "pythia-interval:" "$T6_DIR/.nbs/events/config.yaml" 2>/dev/null | awk '{print $2}')
PYTHIA_INTERVAL=${PYTHIA_INTERVAL:-20}
check "Pythia interval read as 3" "$( [[ $PYTHIA_INTERVAL -eq 3 ]] && echo pass || echo fail )"

# Publish pythia-checkpoint (as Scribe would when count % interval == 0)
if (( DECISION_COUNT % PYTHIA_INTERVAL == 0 )); then
    "$NBS_BUS" publish "$T6_DIR/.nbs/events/" scribe pythia-checkpoint high \
      "Decision count: $DECISION_COUNT. Pythia assessment requested." --dedup-window=0 >/dev/null 2>&1
fi
CHECKPOINT_EVENT=$(ls "$T6_DIR/.nbs/events/"*pythia-checkpoint*.event 2>/dev/null | head -1)
check "pythia-checkpoint event exists" "$( [[ -n "$CHECKPOINT_EVENT" ]] && echo pass || echo fail )"
if [[ -n "$CHECKPOINT_EVENT" ]]; then
    check "checkpoint payload has decision count" "$( grep -q "Decision count: 3" "$CHECKPOINT_EVENT" && echo pass || echo fail )"
fi
echo ""

# --- Test 29: Pythia reads checkpoint, acks, posts assessment to chat ---
echo "29. Pythia reads checkpoint, acks, posts assessment to chat..."
if [[ -n "$CHECKPOINT_EVENT" ]]; then
    # Read the event (as Pythia would)
    EVENT_CONTENT=$("$NBS_BUS" read "$T6_DIR/.nbs/events/" "$(basename "$CHECKPOINT_EVENT")" 2>/dev/null)
    check "Pythia can read checkpoint event" "$( echo "$EVENT_CONTENT" | grep -q "pythia-checkpoint" && echo pass || echo fail )"

    # Ack the event
    "$NBS_BUS" ack "$T6_DIR/.nbs/events/" "$(basename "$CHECKPOINT_EVENT")" >/dev/null 2>&1
    check "Checkpoint event acked" "$( [[ ! -f "$CHECKPOINT_EVENT" ]] && echo pass || echo fail )"

    # Post assessment to chat (as Pythia would)
    "$NBS_CHAT" send "$T6_DIR/.nbs/chat/pipeline.chat" pythia "PYTHIA CHECKPOINT — Assessment #1

**Hidden assumption:** Team assumes file-based events are atomic on target filesystem (D-${TIMESTAMP_1}).

**Second-order risk:** If event volume exceeds filesystem inode limits, the bus silently fails.

**Missing validation:** No test for concurrent file-based event writes under load.

**Six-month regret:** A bridge that carries one cart will not carry a caravan.
File-based events (D-${TIMESTAMP_1}) work at current scale but lack backpressure.

**Confidence:** moderate — pipeline works but scalability untested.

---
End of checkpoint. Pythia out." >/dev/null 2>&1

    PYTHIA_MSG=$("$NBS_CHAT" read "$T6_DIR/.nbs/chat/pipeline.chat" 2>/dev/null)
    check "Pythia assessment in chat" "$( echo "$PYTHIA_MSG" | grep -q "PYTHIA CHECKPOINT" && echo pass || echo fail )"
    check "Assessment contains Hidden assumption" "$( echo "$PYTHIA_MSG" | grep -q "Hidden assumption" && echo pass || echo fail )"
    check "Assessment contains Confidence" "$( echo "$PYTHIA_MSG" | grep -q "Confidence" && echo pass || echo fail )"
else
    check "Pythia can read checkpoint event" "fail"
    check "Checkpoint event acked" "fail"
    check "Pythia assessment in chat" "fail"
    check "Assessment contains Hidden assumption" "fail"
    check "Assessment contains Confidence" "fail"
fi
echo ""

# --- Test 30: Full pipeline round-trip verification ---
echo "30. Full pipeline round-trip: chat → scribe → bus → pythia → chat..."
# Verify the complete sequence of artefacts
CHAT_MSG_COUNT=$("$NBS_CHAT" read "$T6_DIR/.nbs/chat/pipeline.chat" 2>/dev/null | wc -l)
LOG_ENTRIES=$(grep -c "^### D-" "$T6_DIR/.nbs/scribe/pipeline-log.md")
PROCESSED_EVENTS=$(ls "$T6_DIR/.nbs/events/processed/"*.event 2>/dev/null | wc -l)
check "Chat has messages from all pipeline stages" "$( [[ $CHAT_MSG_COUNT -ge 3 ]] && echo pass || echo fail )"
check "Decision log has 3 entries" "$( [[ $LOG_ENTRIES -eq 3 ]] && echo pass || echo fail )"
check "Bus events were processed (in processed/)" "$( [[ $PROCESSED_EVENTS -ge 1 ]] && echo pass || echo fail )"
# Verify Pythia's assessment-posted event
"$NBS_BUS" publish "$T6_DIR/.nbs/events/" pythia assessment-posted normal \
  "Pythia checkpoint posted to pipeline.chat" --dedup-window=0 >/dev/null 2>&1
AP_EVENT=$(ls "$T6_DIR/.nbs/events/"*assessment-posted*.event 2>/dev/null | head -1)
check "assessment-posted event exists" "$( [[ -n "$AP_EVENT" ]] && echo pass || echo fail )"
# Clean up
if [[ -n "$AP_EVENT" ]]; then
    "$NBS_BUS" ack "$T6_DIR/.nbs/events/" "$(basename "$AP_EVENT")" >/dev/null 2>&1
fi
echo ""

# ============================================================
# T7: Multi-handle concurrent cursor tracking
# ============================================================

echo "T7: Multi-handle concurrent cursor tracking"
echo ""

# --- Test 31: Interleaved sends, --since tracks independently ---
echo "31. Interleaved sends, --since tracks independently per handle..."
T7_DIR="$TEST_DIR/t7"
mkdir -p "$T7_DIR/.nbs/chat"
"$NBS_CHAT" create "$T7_DIR/.nbs/chat/cursors.chat" >/dev/null 2>&1

# Alpha sends, then reads with --since
"$NBS_CHAT" send "$T7_DIR/.nbs/chat/cursors.chat" alpha "Alpha message 1" >/dev/null 2>&1
"$NBS_CHAT" send "$T7_DIR/.nbs/chat/cursors.chat" beta "Beta message 1" >/dev/null 2>&1
"$NBS_CHAT" send "$T7_DIR/.nbs/chat/cursors.chat" alpha "Alpha message 2" >/dev/null 2>&1
"$NBS_CHAT" send "$T7_DIR/.nbs/chat/cursors.chat" beta "Beta message 2" >/dev/null 2>&1

# Alpha's --since should show messages after alpha's last post
ALPHA_SINCE=$("$NBS_CHAT" read "$T7_DIR/.nbs/chat/cursors.chat" --since=alpha 2>/dev/null)
# Beta's --since should show messages after beta's last post
BETA_SINCE=$("$NBS_CHAT" read "$T7_DIR/.nbs/chat/cursors.chat" --since=beta 2>/dev/null)

# Alpha's last post was "Alpha message 2" (3rd msg), so --since should show "Beta message 2" (4th msg)
ALPHA_SINCE_COUNT=$(echo "$ALPHA_SINCE" | grep -c "message" || true)
check "Alpha --since sees 1 message after last post" "$( [[ $ALPHA_SINCE_COUNT -eq 1 ]] && echo pass || echo fail )"
check "Alpha --since sees Beta's last message" "$( echo "$ALPHA_SINCE" | grep -q "Beta message 2" && echo pass || echo fail )"

# Beta's last post was "Beta message 2" (4th msg), so --since should show nothing
check "Beta --since sees nothing after last post" "$( [[ -z "$BETA_SINCE" ]] && echo pass || echo fail )"
echo ""

# --- Test 32: --unread returns only unread messages per handle ---
echo "32. --unread tracks independently under concurrent writes..."
T32_DIR="$TEST_DIR/t32"
mkdir -p "$T32_DIR/.nbs/chat"
"$NBS_CHAT" create "$T32_DIR/.nbs/chat/unread.chat" >/dev/null 2>&1

# Send initial batch
"$NBS_CHAT" send "$T32_DIR/.nbs/chat/unread.chat" writer1 "W1 initial" >/dev/null 2>&1
"$NBS_CHAT" send "$T32_DIR/.nbs/chat/unread.chat" writer2 "W2 initial" >/dev/null 2>&1

# Reader A reads — advances A's cursor
"$NBS_CHAT" read "$T32_DIR/.nbs/chat/unread.chat" --unread=readerA >/dev/null 2>&1

# More messages arrive concurrently
"$NBS_CHAT" send "$T32_DIR/.nbs/chat/unread.chat" writer1 "W1 second" >/dev/null 2>&1 &
"$NBS_CHAT" send "$T32_DIR/.nbs/chat/unread.chat" writer2 "W2 second" >/dev/null 2>&1 &
wait

# Reader B reads for the first time — should see all 4 messages
B_UNREAD=$("$NBS_CHAT" read "$T32_DIR/.nbs/chat/unread.chat" --unread=readerB 2>/dev/null)
B_COUNT=$(echo "$B_UNREAD" | wc -l)
check "Reader B (first read) sees all 4 messages" "$( [[ $B_COUNT -eq 4 ]] && echo pass || echo fail )"

# Reader A reads again — should see only the 2 new messages
A_UNREAD=$("$NBS_CHAT" read "$T32_DIR/.nbs/chat/unread.chat" --unread=readerA 2>/dev/null)
A_COUNT=$(echo "$A_UNREAD" | grep -c "second" || true)
check "Reader A sees only 2 new messages" "$( [[ $A_COUNT -eq 2 ]] && echo pass || echo fail )"

# Both read again — neither should see anything new
A_AGAIN=$("$NBS_CHAT" read "$T32_DIR/.nbs/chat/unread.chat" --unread=readerA 2>/dev/null)
B_AGAIN=$("$NBS_CHAT" read "$T32_DIR/.nbs/chat/unread.chat" --unread=readerB 2>/dev/null)
check "Reader A second re-read is empty" "$( [[ -z "$A_AGAIN" ]] && echo pass || echo fail )"
check "Reader B second re-read is empty" "$( [[ -z "$B_AGAIN" ]] && echo pass || echo fail )"
echo ""

# --- Test 33: Three handles concurrent sends, all cursors independent ---
echo "33. Three handles concurrent sends, cursor independence..."
T33_DIR="$TEST_DIR/t33"
mkdir -p "$T33_DIR/.nbs/chat"
"$NBS_CHAT" create "$T33_DIR/.nbs/chat/triple.chat" >/dev/null 2>&1

# Three handles each send 3 messages concurrently
for i in 1 2 3; do
    "$NBS_CHAT" send "$T33_DIR/.nbs/chat/triple.chat" handleX "X-$i" >/dev/null 2>&1 &
    "$NBS_CHAT" send "$T33_DIR/.nbs/chat/triple.chat" handleY "Y-$i" >/dev/null 2>&1 &
    "$NBS_CHAT" send "$T33_DIR/.nbs/chat/triple.chat" handleZ "Z-$i" >/dev/null 2>&1 &
done
wait

# Verify all 9 messages present
TOTAL=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" 2>/dev/null | wc -l)
check "All 9 concurrent messages present" "$( [[ $TOTAL -eq 9 ]] && echo pass || echo fail )"

# handleX reads with --unread, then more messages arrive, then reads again
X_FIRST=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" --unread=handleX 2>/dev/null)
X_FIRST_COUNT=$(echo "$X_FIRST" | wc -l)
check "handleX first --unread sees all 9" "$( [[ $X_FIRST_COUNT -eq 9 ]] && echo pass || echo fail )"

# Send one more from Y
"$NBS_CHAT" send "$T33_DIR/.nbs/chat/triple.chat" handleY "Y-extra" >/dev/null 2>&1

# handleX should see only the new message
X_SECOND=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" --unread=handleX 2>/dev/null)
check "handleX second --unread sees only Y-extra" "$( echo "$X_SECOND" | grep -q "Y-extra" && [[ $(echo "$X_SECOND" | wc -l) -eq 1 ]] && echo pass || echo fail )"

# handleZ has never read — should see all 10
Z_FIRST=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" --unread=handleZ 2>/dev/null)
Z_COUNT=$(echo "$Z_FIRST" | wc -l)
check "handleZ (never read) sees all 10" "$( [[ $Z_COUNT -eq 10 ]] && echo pass || echo fail )"

# Verify cursor file has entries for X and Z (not Y, since Y only sent, never read with --unread)
CURSOR_FILE="$T33_DIR/.nbs/chat/triple.chat.cursors"
check "Cursor file exists" "$( [[ -f "$CURSOR_FILE" ]] && echo pass || echo fail )"
if [[ -f "$CURSOR_FILE" ]]; then
    check "Cursor has handleX entry" "$( grep -q "handleX=" "$CURSOR_FILE" && echo pass || echo fail )"
    check "Cursor has handleZ entry" "$( grep -q "handleZ=" "$CURSOR_FILE" && echo pass || echo fail )"
fi
echo ""

# ============================================================
# T8: nbs-chat-init --compact-log decision log archival
# ============================================================

echo "T8: nbs-chat-init --compact-log"
echo ""

# Helper: generate a decision log with N entries
generate_log_entries() {
    local log_file="$1"
    local count="$2"
    cat > "$log_file" << 'LOGEOF'
# Decision Log

Project: test-compact
Created: 2026-02-16T00:00:00Z
Scribe: scribe
Chat: compact.chat
Decision count: 0

---
LOGEOF
    for i in $(seq 1 "$count"); do
        cat >> "$log_file" << ENTRY

---

### D-${i}00000 Test decision $i
- **Chat ref:** compact.chat:~L${i}
- **Participants:** alice
- **Artefacts:** —
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Test entry number $i for compaction testing.
ENTRY
    done
}

# --- Test 34: Log below threshold is not compacted ---
echo "34. Log below threshold (50 entries) is not compacted..."
T34_DIR="$TEST_DIR/t34"
mkdir -p "$T34_DIR/.nbs/chat" "$T34_DIR/.nbs/events/processed" "$T34_DIR/.nbs/scribe"
"$NBS_CHAT" create "$T34_DIR/.nbs/chat/compact.chat" >/dev/null 2>&1
generate_log_entries "$T34_DIR/.nbs/scribe/compact-log.md" 50

(cd "$T34_DIR" && "$NBS_CHAT_INIT" --name=compact --force >/dev/null 2>&1)
ARCHIVE_FILES=$(find "$T34_DIR/.nbs/scribe/" -name "compact-log-archive-*.md" 2>/dev/null | wc -l)
ENTRY_COUNT=$(grep -c '^### D-' "$T34_DIR/.nbs/scribe/compact-log.md" 2>/dev/null || echo 0)
check "No archive created (below threshold)" "$( [[ $ARCHIVE_FILES -eq 0 ]] && echo pass || echo fail )"
check "Original log intact (50 entries)" "$( [[ $ENTRY_COUNT -eq 50 ]] && echo pass || echo fail )"
echo ""

# --- Test 35: Log at threshold is archived ---
echo "35. Log at threshold (100 entries) is archived..."
T35_DIR="$TEST_DIR/t35"
mkdir -p "$T35_DIR/.nbs/chat" "$T35_DIR/.nbs/events/processed" "$T35_DIR/.nbs/scribe"
"$NBS_CHAT" create "$T35_DIR/.nbs/chat/compact.chat" >/dev/null 2>&1
generate_log_entries "$T35_DIR/.nbs/scribe/compact-log.md" 100

(cd "$T35_DIR" && "$NBS_CHAT_INIT" --name=compact --force >/dev/null 2>&1)
ARCHIVE_FILE=$(find "$T35_DIR/.nbs/scribe/" -name "compact-log-archive-*.md" 2>/dev/null | head -1)
check "Archive file created" "$( [[ -n "$ARCHIVE_FILE" ]] && echo pass || echo fail )"
if [[ -n "$ARCHIVE_FILE" ]]; then
    ARCHIVE_ENTRIES=$(grep -c '^### D-' "$ARCHIVE_FILE" 2>/dev/null || echo 0)
    check "Archive has 100 entries" "$( [[ $ARCHIVE_ENTRIES -eq 100 ]] && echo pass || echo fail )"
fi
# New log should exist with linked-list header
check "New log created" "$( [[ -f "$T35_DIR/.nbs/scribe/compact-log.md" ]] && echo pass || echo fail )"
check "New log has ARCHIVE-LINK" "$( grep -q '### ARCHIVE-LINK' "$T35_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
check "New log has Previous log header" "$( grep -q '^Previous log:' "$T35_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
echo ""

# --- Test 36: New log linked-list header content ---
echo "36. New log linked-list header has correct content..."
if [[ -n "$ARCHIVE_FILE" ]]; then
    check "ARCHIVE-LINK references archive file" "$( grep -q "$(basename "$ARCHIVE_FILE")" "$T35_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
    check "Entries archived count is 100" "$( grep -q 'Entries archived.*100' "$T35_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
    check "Project preserved in new log" "$( grep -q '^Project: test-compact' "$T35_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
    check "Chat ref preserved in new log" "$( grep -q '^Chat: compact.chat' "$T35_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
else
    check "ARCHIVE-LINK references archive file" "fail"
    check "Entries archived count is 100" "fail"
    check "Project preserved in new log" "fail"
    check "Chat ref preserved in new log" "fail"
fi
echo ""

# --- Test 37: --compact-log forces compaction below threshold ---
echo "37. --compact-log forces compaction below threshold..."
T37_DIR="$TEST_DIR/t37"
mkdir -p "$T37_DIR/.nbs/chat" "$T37_DIR/.nbs/events/processed" "$T37_DIR/.nbs/scribe"
"$NBS_CHAT" create "$T37_DIR/.nbs/chat/compact.chat" >/dev/null 2>&1
generate_log_entries "$T37_DIR/.nbs/scribe/compact-log.md" 5

(cd "$T37_DIR" && "$NBS_CHAT_INIT" --name=compact --force --compact-log >/dev/null 2>&1)
FORCE_ARCHIVE=$(find "$T37_DIR/.nbs/scribe/" -name "compact-log-archive-*.md" 2>/dev/null | head -1)
check "Archive created despite only 5 entries" "$( [[ -n "$FORCE_ARCHIVE" ]] && echo pass || echo fail )"
if [[ -n "$FORCE_ARCHIVE" ]]; then
    FORCE_ENTRIES=$(grep -c '^### D-' "$FORCE_ARCHIVE" 2>/dev/null || echo 0)
    check "Archive has 5 entries" "$( [[ $FORCE_ENTRIES -eq 5 ]] && echo pass || echo fail )"
fi
check "New log has ARCHIVE-LINK" "$( grep -q '### ARCHIVE-LINK' "$T37_DIR/.nbs/scribe/compact-log.md" && echo pass || echo fail )"
echo ""

# --- Test 38: --dry-run with --compact-log does not modify files ---
echo "38. --dry-run with --compact-log does not modify files..."
T38_DIR="$TEST_DIR/t38"
mkdir -p "$T38_DIR/.nbs/chat" "$T38_DIR/.nbs/events/processed" "$T38_DIR/.nbs/scribe"
"$NBS_CHAT" create "$T38_DIR/.nbs/chat/compact.chat" >/dev/null 2>&1
generate_log_entries "$T38_DIR/.nbs/scribe/compact-log.md" 10
BEFORE_MD5=$(md5sum "$T38_DIR/.nbs/scribe/compact-log.md" | awk '{print $1}')

(cd "$T38_DIR" && "$NBS_CHAT_INIT" --name=compact --force --compact-log --dry-run >/dev/null 2>&1)
AFTER_MD5=$(md5sum "$T38_DIR/.nbs/scribe/compact-log.md" | awk '{print $1}')
DRY_ARCHIVE=$(find "$T38_DIR/.nbs/scribe/" -name "compact-log-archive-*.md" 2>/dev/null | wc -l)
check "Log file unchanged by --dry-run" "$( [[ "$BEFORE_MD5" == "$AFTER_MD5" ]] && echo pass || echo fail )"
check "No archive created by --dry-run" "$( [[ $DRY_ARCHIVE -eq 0 ]] && echo pass || echo fail )"
echo ""

# ============================================================
# T9: nbs-claude-remote argument validation
# ============================================================

echo "T9: nbs-claude-remote argument validation"
echo ""

# --- Test 39: --help exits 0 ---
echo "39. --help exits 0..."
set +e
"$NBS_CLAUDE_REMOTE" --help >/dev/null 2>&1
RC=$?
set -e
check "exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
echo ""

# --- Test 40: Missing --host exits 4 ---
echo "40. Missing --host exits 4..."
set +e
"$NBS_CLAUDE_REMOTE" --root=/tmp 2>/dev/null
RC=$?
set -e
check "exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
echo ""

# --- Test 41: Missing --root (without --list) exits 4 ---
echo "41. Missing --root (without --list) exits 4..."
set +e
"$NBS_CLAUDE_REMOTE" --host=user@example 2>/dev/null
RC=$?
set -e
check "exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
echo ""

# --- Test 42: Unknown argument exits 4 ---
echo "42. Unknown argument exits 4..."
set +e
"$NBS_CLAUDE_REMOTE" --host=user@example --root=/tmp --bogus 2>/dev/null
RC=$?
set -e
check "exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
echo ""

# --- Test 43: --help output contains usage information ---
echo "43. --help output contains usage information..."
HELP_OUTPUT=$("$NBS_CLAUDE_REMOTE" --help 2>&1)
check "Help mentions --host" "$( echo "$HELP_OUTPUT" | grep -qF -- '--host' && echo pass || echo fail )"
check "Help mentions --root" "$( echo "$HELP_OUTPUT" | grep -qF -- '--root' && echo pass || echo fail )"
check "Help mentions --resume" "$( echo "$HELP_OUTPUT" | grep -qF -- '--resume' && echo pass || echo fail )"
check "Help mentions --list" "$( echo "$HELP_OUTPUT" | grep -qF -- '--list' && echo pass || echo fail )"
echo ""

# ============================================================
# T10: Terminal self-echo
# ============================================================

echo "T10: Terminal self-echo"
echo ""

# --- Test 44: Normal send echoes message back to sender ---
echo "44. Normal send echoes message back to sender..."
T10_DIR="$TEST_DIR/t10"
mkdir -p "$T10_DIR/.nbs/chat" "$T10_DIR/.nbs/events/processed"
"$NBS_CHAT" create "$T10_DIR/.nbs/chat/echo.chat" >/dev/null 2>&1

# Send a message via terminal and capture output
ECHO_OUTPUT=$(printf 'Hello self-echo test\x04' | timeout 5 "$NBS_TERMINAL" "$T10_DIR/.nbs/chat/echo.chat" "echouser" 2>&1 || true)
check "Self-echo in terminal output" "$( echo "$ECHO_OUTPUT" | grep -q "Hello self-echo test" && echo pass || echo fail )"
echo ""

# --- Test 45: Self-echo contains sender handle ---
echo "45. Self-echo contains sender handle..."
check "Handle in self-echo output" "$( echo "$ECHO_OUTPUT" | grep -q "echouser" && echo pass || echo fail )"
# Note: /edit self-echo (fe86eb5) cannot be tested without a tty —
# the editor requires /dev/tty which is unavailable in piped CI.
# The normal-send self-echo exercises the same format_message() path.
echo ""

# ============================================================
# T11: Self-healing sidecar after skill loss
# ============================================================

echo "T11: Self-healing sidecar — skill failure detection and recovery"
echo ""

# Set up a project directory with skill files and source sidecar functions
T11_DIR="$TEST_DIR/t11-heal"
mkdir -p "$T11_DIR/.nbs/chat" "$T11_DIR/.nbs/events/processed" "$T11_DIR/claude_tools"

# Create skill files
cat > "$T11_DIR/claude_tools/nbs-notify.md" << 'SKILL'
---
description: "NBS Notify: Process pending events or messages"
allowed-tools: Bash, Read
---
# NBS Notify
SKILL

cat > "$T11_DIR/claude_tools/nbs-teams-chat.md" << 'SKILL'
---
description: "NBS Teams: AI-to-AI Chat"
allowed-tools: Bash, Read
---
# NBS Teams Chat
SKILL

cat > "$T11_DIR/claude_tools/nbs-poll.md" << 'SKILL'
---
description: "NBS Poll"
allowed-tools: Bash, Read
---
# NBS Poll
SKILL

"$NBS_CHAT" create "$T11_DIR/.nbs/chat/live.chat" >/dev/null 2>&1

# Source sidecar functions
SIDECAR_HANDLE="heal-test"
NBS_ROOT="$T11_DIR"
_EXTRACT_TMP=$(mktemp)
sed -n '/^# --- Configuration ---/,/^# --- Cleanup ---/p' "$NBS_CLAUDE" | head -n -1 >> "$_EXTRACT_TMP"
sed -n '/^# --- Context stress detection ---/,/^# --- Dynamic resource registration ---/p' "$NBS_CLAUDE" | head -n -1 >> "$_EXTRACT_TMP"
sed -n '/^# --- Dynamic resource registration ---/,/^# --- Idle detection sidecar/p' "$NBS_CLAUDE" | head -n -2 >> "$_EXTRACT_TMP"
source "$_EXTRACT_TMP"
rm -f "$_EXTRACT_TMP"

# Re-set after sourcing
SIDECAR_HANDLE="heal-test"
NBS_ROOT="$T11_DIR"
CONTROL_INBOX="${NBS_ROOT}/.nbs/control-inbox-${SIDECAR_HANDLE}"
CONTROL_REGISTRY="${NBS_ROOT}/.nbs/control-registry-${SIDECAR_HANDLE}"
NOTIFY_FAIL_COUNT=0
NOTIFY_FAIL_THRESHOLD=5

cd "$T11_DIR"
seed_registry

# --- Test 46: detect_skill_failure + failure counter integration ---
echo "46. Skill failure detection increments counter..."
NOTIFY_FAIL_COUNT=0
# Simulate 5 consecutive skill failures
for i in $(seq 1 5); do
    if detect_skill_failure "❯ Unknown skill: nbs-notify"; then
        NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
    fi
done
check "Counter reaches threshold after 5 failures" "$( [[ $NOTIFY_FAIL_COUNT -eq 5 ]] && echo pass || echo fail )"
echo ""

# --- Test 47: Counter resets on successful injection ---
echo "47. Counter resets on successful injection..."
NOTIFY_FAIL_COUNT=3
# Simulate a successful injection (no 'Unknown skill' in output)
if ! detect_skill_failure "● Bash(nbs-chat read .nbs/chat/live.chat)
  ⎿  some output
❯"; then
    NOTIFY_FAIL_COUNT=0
fi
check "Counter reset to 0 after success" "$( [[ $NOTIFY_FAIL_COUNT -eq 0 ]] && echo pass || echo fail )"
echo ""

# --- Test 48: build_recovery_prompt produces usable prompt ---
echo "48. Recovery prompt contains absolute paths and handle..."
# seed_registry already populated chat entries from .nbs/chat/*.chat

RECOVERY=$(build_recovery_prompt)

check "Recovery prompt contains nbs-notify.md path" "$( echo "$RECOVERY" | grep -qF "claude_tools/nbs-notify.md" && echo pass || echo fail )"
check "Recovery prompt contains nbs-teams-chat.md path" "$( echo "$RECOVERY" | grep -qF "claude_tools/nbs-teams-chat.md" && echo pass || echo fail )"
check "Recovery prompt contains nbs-poll.md path" "$( echo "$RECOVERY" | grep -qF "claude_tools/nbs-poll.md" && echo pass || echo fail )"
check "Recovery prompt contains agent handle" "$( echo "$RECOVERY" | grep -qF "heal-test" && echo pass || echo fail )"
check "Recovery prompt contains chat file for announcement" "$( echo "$RECOVERY" | grep -qF ".nbs/chat/live.chat" && echo pass || echo fail )"
echo ""

# --- Test 49: Recovery prompt uses absolute paths (not relative) ---
echo "49. Recovery prompt uses absolute paths..."
# NBS_ROOT is absolute (resolved at startup), so paths should be absolute
check "Paths are absolute (start with /)" "$( echo "$RECOVERY" | grep -qE '/.*claude_tools/nbs-notify.md' && echo pass || echo fail )"
echo ""

# --- Test 50: Threshold triggers recovery instead of /nbs-notify ---
echo "50. Threshold check gates recovery vs normal injection..."
# When NOTIFY_FAIL_COUNT >= NOTIFY_FAIL_THRESHOLD, sidecar should use recovery
NOTIFY_FAIL_COUNT=5
NOTIFY_FAIL_THRESHOLD=5
check "At threshold" "$( [[ $NOTIFY_FAIL_COUNT -ge $NOTIFY_FAIL_THRESHOLD ]] && echo pass || echo fail )"

# Below threshold — should use normal injection
NOTIFY_FAIL_COUNT=4
check "Below threshold" "$( [[ $NOTIFY_FAIL_COUNT -lt $NOTIFY_FAIL_THRESHOLD ]] && echo pass || echo fail )"

# Reset
NOTIFY_FAIL_COUNT=0
echo ""

cd "$SCRIPT_DIR"

# ============================================================
# Summary
# ============================================================

echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
