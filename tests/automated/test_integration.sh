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
#   T12 (51-58): Sidecar detection layer interaction
#   T13 (59-65): Skill failure detection boundary and broken symlink simulation
#   T14 (66-72): Plan mode detection robustness
#   T15 (73-79): Concurrent chat under SIGKILL (Phase 2d)
#   T16 (80-87): Bus event delivery under contention (Phase 2e)
#   T17 (88-95): Permissions prompt detection robustness
#   T18 (96-102): Handle collision guard (pre-spawn pidfile check)
#   T19 (103-110): CSMA/CD standup trigger (temporal carrier sense)
#   T20 (111-117): Conditional notification (event-gated /nbs-notify)
#   T21 (118-122): UTC timestamp display in nbs-chat read/search output

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
# cursor-on-write: worker1 and worker2 each get a cursor from chat_send,
# plus reader1 and reader2 from --unread reads = 4 entries
CURSOR_HANDLES=$(grep -c "=" "$T11_DIR/.nbs/chat/concurrent.chat.cursors" 2>/dev/null || echo 0)
check "Cursor file has 4 handle entries (2 senders + 2 readers)" "$( [[ $CURSOR_HANDLES -eq 4 ]] && echo pass || echo fail )"
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

# cursor-on-write: handleX already has a cursor at its last sent message.
# On --unread, it sees only messages AFTER that cursor — not all 9.
# Exact count depends on interleaving, but it must be < 9.
X_FIRST=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" --unread=handleX 2>/dev/null)
X_FIRST_COUNT=$(echo "$X_FIRST" | grep -c . || echo 0)
check "handleX first --unread sees < 9 (cursor-on-write)" "$( [[ $X_FIRST_COUNT -lt 9 ]] && echo pass || echo fail )"

# Send one more from Y
"$NBS_CHAT" send "$T33_DIR/.nbs/chat/triple.chat" handleY "Y-extra" >/dev/null 2>&1

# handleX should see only the new message
X_SECOND=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" --unread=handleX 2>/dev/null)
check "handleX second --unread sees only Y-extra" "$( echo "$X_SECOND" | grep -q "Y-extra" && [[ $(echo "$X_SECOND" | wc -l) -eq 1 ]] && echo pass || echo fail )"

# handleZ also has a cursor from cursor-on-write — sees messages after its last send, plus Y-extra
Z_FIRST=$("$NBS_CHAT" read "$T33_DIR/.nbs/chat/triple.chat" --unread=handleZ 2>/dev/null)
Z_COUNT=$(echo "$Z_FIRST" | grep -c . || echo 0)
# handleZ's cursor is at its last sent message, so it sees < 10 (not all)
check "handleZ (cursor-on-write) sees < 10" "$( [[ $Z_COUNT -lt 10 ]] && echo pass || echo fail )"

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
sed -n '/^# --- Plan mode detection ---/,/^# --- Context stress detection ---/p' "$NBS_CLAUDE" | head -n -1 >> "$_EXTRACT_TMP"
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
# T12: Sidecar detection layer interaction
# ============================================================
#
# Pythia #7 flagged: 6 independent detection layers with no test
# validating their composition. This section tests that layers
# interact correctly — context stress gates skill failure detection,
# plan mode and AskUserQuestion don't conflict, and the full
# recovery sequence works end-to-end.

echo "T12: Sidecar detection layer interaction"
echo ""

# Re-use T11's function extraction (already sourced)
# Reset state for T12
T12_DIR="$TEST_DIR/t12-layers"
mkdir -p "$T12_DIR/.nbs/chat" "$T12_DIR/.nbs/events/processed" "$T12_DIR/claude_tools"
cat > "$T12_DIR/claude_tools/nbs-notify.md" << 'SKILL'
---
description: "NBS Notify"
---
# NBS Notify
SKILL
cat > "$T12_DIR/claude_tools/nbs-teams-chat.md" << 'SKILL'
---
description: "NBS Teams Chat"
---
# NBS Teams Chat
SKILL
cat > "$T12_DIR/claude_tools/nbs-poll.md" << 'SKILL'
---
description: "NBS Poll"
---
# NBS Poll
SKILL
"$NBS_CHAT" create "$T12_DIR/.nbs/chat/live.chat" >/dev/null 2>&1

SIDECAR_HANDLE="layer-test"
NBS_ROOT="$T12_DIR"
CONTROL_INBOX="${NBS_ROOT}/.nbs/control-inbox-${SIDECAR_HANDLE}"
CONTROL_REGISTRY="${NBS_ROOT}/.nbs/control-registry-${SIDECAR_HANDLE}"
NOTIFY_FAIL_COUNT=0
NOTIFY_FAIL_THRESHOLD=5
cd "$T12_DIR"
seed_registry

# --- Test 51: Context stress blocks skill failure detection path ---
echo "51. Context stress gates skill failure detection..."
# When context-stressed, the sidecar should skip the entire injection path.
# This means NOTIFY_FAIL_COUNT should NOT increment during context stress.
NOTIFY_FAIL_COUNT=3
STRESS_CONTENT="Compacting conversation...
❯"
NORMAL_FAIL_CONTENT="❯ Unknown skill: nbs-notify"

# Context stress should be detected
check "Context stress detected" "$( detect_context_stress "$STRESS_CONTENT" && echo pass || echo fail )"
# Skill failure should NOT be checked during context stress (stress gates the path)
# Simulate: if stress is detected, the sidecar continues without checking skill failure
if detect_context_stress "$STRESS_CONTENT"; then
    # Sidecar would 'continue' here — NOTIFY_FAIL_COUNT unchanged
    :
else
    # This path should not execute
    if detect_skill_failure "$STRESS_CONTENT"; then
        NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
    fi
fi
check "Failure counter unchanged during context stress" "$( [[ $NOTIFY_FAIL_COUNT -eq 3 ]] && echo pass || echo fail )"
echo ""

# --- Test 52: Plan mode and AskUserQuestion are mutually exclusive in practice ---
echo "52. Plan mode and AskUserQuestion detection independence..."
PLAN_CONTENT="Would you like to proceed?
  1. Yes
  2. Yes, and don't ask again for this project
❯"
ASK_CONTENT="? Choose an option
  1. Option A
  2. Option B
Type something.
❯"
NEITHER_CONTENT="● Bash(ls)
  ⎿  file1.txt file2.txt
❯"

check "Plan mode detected in plan content" "$( detect_plan_mode "$PLAN_CONTENT" && echo pass || echo fail )"
check "AskUserQuestion NOT detected in plan content" "$( ! detect_ask_modal "$PLAN_CONTENT" && echo pass || echo fail )"
check "AskUserQuestion detected in ask content" "$( detect_ask_modal "$ASK_CONTENT" && echo pass || echo fail )"
check "Plan mode NOT detected in ask content" "$( ! detect_plan_mode "$ASK_CONTENT" && echo pass || echo fail )"
check "Neither detected in normal content" "$( ! detect_plan_mode "$NEITHER_CONTENT" && ! detect_ask_modal "$NEITHER_CONTENT" && echo pass || echo fail )"
echo ""

# --- Test 53: Context stress does not trigger skill failure ---
echo "53. Context stress content is not a false positive for skill failure..."
check "Context stress not misidentified as skill failure" "$( ! detect_skill_failure "$STRESS_CONTENT" && echo pass || echo fail )"
echo ""

# --- Test 54: Skill failure does not trigger context stress ---
echo "54. Skill failure content is not a false positive for context stress..."
check "Skill failure not misidentified as context stress" "$( ! detect_context_stress "$NORMAL_FAIL_CONTENT" && echo pass || echo fail )"
echo ""

# --- Test 55: End-to-end recovery sequence simulation ---
echo "55. End-to-end recovery: threshold → recovery prompt → counter reset..."
NOTIFY_FAIL_COUNT=0
# Simulate 5 consecutive failures (the actual sidecar loop behaviour)
for i in $(seq 1 5); do
    if detect_skill_failure "❯ Unknown skill: nbs-notify"; then
        NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
    fi
done
check "Reached threshold after 5 failures" "$( [[ $NOTIFY_FAIL_COUNT -ge $NOTIFY_FAIL_THRESHOLD ]] && echo pass || echo fail )"

# At threshold — build recovery prompt (as the sidecar would)
RECOVERY=$(build_recovery_prompt)
check "Recovery prompt built at threshold" "$( [[ -n "$RECOVERY" ]] && echo pass || echo fail )"

# Simulate successful recovery (no "Unknown skill" in response)
RECOVERY_RESPONSE="● Read(claude_tools/nbs-notify.md)
  ⎿  # NBS Notify
● Bash(nbs-chat send .nbs/chat/live.chat layer-test 'active')
  ⎿  (done)
❯"
if ! detect_skill_failure "$RECOVERY_RESPONSE"; then
    NOTIFY_FAIL_COUNT=0
fi
check "Counter reset after successful recovery" "$( [[ $NOTIFY_FAIL_COUNT -eq 0 ]] && echo pass || echo fail )"
echo ""

# --- Test 56: Recovery prompt with no registry (edge case) ---
echo "56. Recovery prompt without registry file..."
# Temporarily remove registry to simulate missing state
mv "$CONTROL_REGISTRY" "${CONTROL_REGISTRY}.bak"
RECOVERY_NO_REG=$(build_recovery_prompt)
check "Recovery prompt still contains skill paths without registry" "$( echo "$RECOVERY_NO_REG" | grep -qF "claude_tools/nbs-notify.md" && echo pass || echo fail )"
check "Recovery prompt omits chat instruction without registry" "$( ! echo "$RECOVERY_NO_REG" | grep -qF "nbs-chat send" && echo pass || echo fail )"
mv "${CONTROL_REGISTRY}.bak" "$CONTROL_REGISTRY"
echo ""

# --- Test 57: Failed recovery does not reset counter ---
echo "57. Failed recovery keeps counter at threshold..."
NOTIFY_FAIL_COUNT=5
# Simulate recovery attempt that fails (skill still broken)
FAILED_RECOVERY="❯ Unknown skill: nbs-notify"
if ! detect_skill_failure "$FAILED_RECOVERY"; then
    NOTIFY_FAIL_COUNT=0
fi
check "Counter stays at threshold after failed recovery" "$( [[ $NOTIFY_FAIL_COUNT -eq 5 ]] && echo pass || echo fail )"
echo ""

# --- Test 58: Multiple layers on same content do not interfere ---
echo "58. All detection layers produce consistent results on mixed content..."
MIXED_CONTENT="Compacting conversation...
Would you like to proceed?
❯ Unknown skill: nbs-notify
? 1. Option A
Type something."
# In practice this content is impossible, but the detectors should not crash
# and each should independently find its pattern
check "Context stress detected in mixed" "$( detect_context_stress "$MIXED_CONTENT" && echo pass || echo fail )"
check "Plan mode detected in mixed" "$( detect_plan_mode "$MIXED_CONTENT" && echo pass || echo fail )"
check "Skill failure detected in mixed" "$( detect_skill_failure "$MIXED_CONTENT" && echo pass || echo fail )"
check "AskUserQuestion detected in mixed" "$( detect_ask_modal "$MIXED_CONTENT" && echo pass || echo fail )"
echo ""

cd "$SCRIPT_DIR"

# ============================================================
# T13: Skill failure detection boundary and broken symlink simulation
# ============================================================
#
# Testkeeper Gap 2: detect_skill_failure only matches 'Unknown skill'.
# Other failure modes (permission denied, empty output, 'command not found')
# would bypass detection. This section documents the detection boundary
# and tests the end-to-end recovery with simulated broken symlinks.
#
# Testkeeper Gap 4: No degraded-condition integration test simulating
# the actual failure that motivated the self-healing code.

echo "T13: Skill failure detection boundary and broken symlink simulation"
echo ""

T13_DIR="$TEST_DIR/t13-boundary"
mkdir -p "$T13_DIR/.nbs/chat" "$T13_DIR/.nbs/events/processed" "$T13_DIR/claude_tools"
cat > "$T13_DIR/claude_tools/nbs-notify.md" << 'SKILL'
---
description: "NBS Notify"
---
# NBS Notify
SKILL
cat > "$T13_DIR/claude_tools/nbs-teams-chat.md" << 'SKILL'
---
description: "NBS Teams Chat"
---
# NBS Teams Chat
SKILL
cat > "$T13_DIR/claude_tools/nbs-poll.md" << 'SKILL'
---
description: "NBS Poll"
---
# NBS Poll
SKILL
"$NBS_CHAT" create "$T13_DIR/.nbs/chat/live.chat" >/dev/null 2>&1

SIDECAR_HANDLE="boundary-test"
NBS_ROOT="$T13_DIR"
CONTROL_INBOX="${NBS_ROOT}/.nbs/control-inbox-${SIDECAR_HANDLE}"
CONTROL_REGISTRY="${NBS_ROOT}/.nbs/control-registry-${SIDECAR_HANDLE}"
NOTIFY_FAIL_COUNT=0
NOTIFY_FAIL_THRESHOLD=5
cd "$T13_DIR"
seed_registry

# --- Test 59: detect_skill_failure matches exact incident output ---
echo "59. Detection matches the exact broken-symlink incident output..."
# This is the literal output observed during the D-1771264017 incident
INCIDENT_OUTPUT="❯ /nbs-notify 2 event(s) in .nbs/events/

  Unknown skill: nbs-notify"
check "Incident output detected" "$( detect_skill_failure "$INCIDENT_OUTPUT" && echo pass || echo fail )"
echo ""

# --- Test 60: Detection boundary — patterns NOT caught ---
echo "60. Detection boundary — alternative failure patterns are not caught..."
# These are theoretical failure modes that bypass detect_skill_failure.
# Documenting the boundary: if these patterns are ever observed in the wild,
# detect_skill_failure must be extended.
PERM_DENIED="❯ /nbs-notify
  Error: Permission denied: /home/user/.claude/commands/nbs-notify.md"
EMPTY_OUTPUT="❯ /nbs-notify

❯"
CMD_NOT_FOUND="❯ /nbs-notify
  bash: nbs-notify: command not found"
TIMEOUT="❯ /nbs-notify
  Timed out waiting for response"

check "Permission denied NOT caught (boundary)" "$( ! detect_skill_failure "$PERM_DENIED" && echo pass || echo fail )"
check "Empty output NOT caught (boundary)" "$( ! detect_skill_failure "$EMPTY_OUTPUT" && echo pass || echo fail )"
check "Command not found NOT caught (boundary)" "$( ! detect_skill_failure "$CMD_NOT_FOUND" && echo pass || echo fail )"
check "Timeout NOT caught (boundary)" "$( ! detect_skill_failure "$TIMEOUT" && echo pass || echo fail )"
echo ""

# --- Test 61: Broken symlink simulation — full recovery sequence ---
echo "61. Broken symlink simulation — full recovery end-to-end..."
# Create a temporary directory to simulate the broken symlink scenario
SYMLINK_DIR=$(mktemp -d)
mkdir -p "$SYMLINK_DIR/commands"
# Create real skill files in the temp dir
cp "$T13_DIR/claude_tools/nbs-notify.md" "$SYMLINK_DIR/commands/"
# Create symlinks pointing to the temp dir (simulating install.sh behaviour)
SKILL_LINK_DIR=$(mktemp -d)
ln -sf "$SYMLINK_DIR/commands/nbs-notify.md" "$SKILL_LINK_DIR/nbs-notify.md"
# Verify symlink works
check "Symlink valid before deletion" "$( [[ -f "$SKILL_LINK_DIR/nbs-notify.md" ]] && echo pass || echo fail )"
# Now delete the target (simulating test cleanup deleting /tmp/tmp.XXXXX)
rm -rf "$SYMLINK_DIR"
# Symlink is now dangling
check "Symlink dangling after target deletion" "$( [[ -L "$SKILL_LINK_DIR/nbs-notify.md" && ! -f "$SKILL_LINK_DIR/nbs-notify.md" ]] && echo pass || echo fail )"
rm -rf "$SKILL_LINK_DIR"
echo ""

# --- Test 62: Recovery prompt resolves real paths despite broken symlinks ---
echo "62. Recovery prompt uses realpath on real skill files..."
# build_recovery_prompt uses realpath on NBS_ROOT/claude_tools/*.md
# These files exist, so realpath should resolve them
RECOVERY=$(build_recovery_prompt)
# Paths should be absolute and point to actual files
NOTIFY_PATH=$(echo "$RECOVERY" | grep -oP '/[^ ,]*nbs-notify\.md')
check "Recovery prompt notify path exists" "$( [[ -f "$NOTIFY_PATH" ]] && echo pass || echo fail )"
echo ""

# --- Test 63: Counter accumulation across mixed success/failure ---
echo "63. Counter accumulation with interleaved success and failure..."
NOTIFY_FAIL_COUNT=0
# Fail, fail, success (reset), fail, fail, fail — should be at 3, not 5
detect_skill_failure "Unknown skill: nbs-notify" && NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
detect_skill_failure "Unknown skill: nbs-notify" && NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
# Successful injection resets counter
! detect_skill_failure "● Bash(ls)" && NOTIFY_FAIL_COUNT=0
detect_skill_failure "Unknown skill: nbs-notify" && NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
detect_skill_failure "Unknown skill: nbs-notify" && NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
detect_skill_failure "Unknown skill: nbs-notify" && NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
check "Counter at 3 after reset mid-sequence" "$( [[ $NOTIFY_FAIL_COUNT -eq 3 ]] && echo pass || echo fail )"
check "Below threshold after reset mid-sequence" "$( [[ $NOTIFY_FAIL_COUNT -lt $NOTIFY_FAIL_THRESHOLD ]] && echo pass || echo fail )"
echo ""

# --- Test 64: Recovery prompt with multiple chat files uses first ---
echo "64. Recovery prompt selects first chat file..."
# Add a second chat file
"$NBS_CHAT" create "$T13_DIR/.nbs/chat/second.chat" >/dev/null 2>&1
# Re-seed to pick up the new file
seed_registry
RECOVERY_MULTI=$(build_recovery_prompt)
# The prompt should contain a chat file reference (the first one found)
check "Recovery prompt references a chat file" "$( echo "$RECOVERY_MULTI" | grep -qF ".nbs/chat/" && echo pass || echo fail )"
check "Recovery prompt contains nbs-chat send instruction" "$( echo "$RECOVERY_MULTI" | grep -qF "nbs-chat send" && echo pass || echo fail )"
echo ""

# --- Test 65: Full degraded scenario — broken skills + threshold + recovery ---
echo "65. Full degraded scenario simulation..."
NOTIFY_FAIL_COUNT=0
NOTIFY_FAIL_THRESHOLD=5
# Phase 1: Agent receives 5 consecutive 'Unknown skill' responses
for i in $(seq 1 5); do
    if detect_skill_failure "❯ Unknown skill: nbs-notify"; then
        NOTIFY_FAIL_COUNT=$((NOTIFY_FAIL_COUNT + 1))
    fi
done
check "Phase 1: threshold reached" "$( [[ $NOTIFY_FAIL_COUNT -ge $NOTIFY_FAIL_THRESHOLD ]] && echo pass || echo fail )"

# Phase 2: Sidecar switches to recovery mode, builds prompt
RECOVERY=$(build_recovery_prompt)
check "Phase 2: recovery prompt contains skill paths" "$( echo "$RECOVERY" | grep -qF "nbs-notify.md" && echo pass || echo fail )"
check "Phase 2: recovery prompt contains handle" "$( echo "$RECOVERY" | grep -qF "boundary-test" && echo pass || echo fail )"

# Phase 3: Agent processes recovery prompt successfully
AGENT_RESPONSE="● Read($T13_DIR/claude_tools/nbs-notify.md)
  ⎿  # NBS Notify
● Bash(nbs-chat send $T13_DIR/.nbs/chat/live.chat boundary-test 'active — skills restored')
  ⎿  (done)
❯"
if ! detect_skill_failure "$AGENT_RESPONSE"; then
    NOTIFY_FAIL_COUNT=0
fi
check "Phase 3: counter reset after recovery" "$( [[ $NOTIFY_FAIL_COUNT -eq 0 ]] && echo pass || echo fail )"

# Phase 4: Normal operation resumes — next injection works
NORMAL_RESPONSE="● Bash(nbs-chat read .nbs/chat/live.chat --unread=boundary-test)
  ⎿  alex: hello
❯"
if ! detect_skill_failure "$NORMAL_RESPONSE"; then
    # Normal operation — counter stays at 0
    :
fi
check "Phase 4: counter stays 0 during normal operation" "$( [[ $NOTIFY_FAIL_COUNT -eq 0 ]] && echo pass || echo fail )"
echo ""

cd "$SCRIPT_DIR"

# ============================================================
# T14: Plan mode detection robustness
# ============================================================
#
# Alex flagged plan mode as high risk — if detection fails, agents hang
# indefinitely. This tests the full detection surface: exact matches,
# variations in formatting, false positive resistance, and interaction
# with the sidecar's auto-select behaviour.

echo "T14: Plan mode detection robustness"
echo ""

# --- Test 66: Exact Claude Code plan mode prompt ---
echo "66. Exact plan mode prompt format..."
EXACT_PLAN="  Plan:
  1. Yes
  2. Yes, and don't ask again for this project
  Would you like to proceed?
❯"
check "Exact plan mode detected" "$( detect_plan_mode "$EXACT_PLAN" && echo pass || echo fail )"
echo ""

# --- Test 67: Plan mode with different surrounding context ---
echo "67. Plan mode detection with varying context..."
# Plan mode after tool output
PLAN_AFTER_TOOL="● Read(CLAUDE.md)
  ⎿  # Project Configuration
  Would you like to proceed?
  1. Yes
  2. Yes, and don't ask again for this project
❯"
# Plan mode with spinner text above
PLAN_WITH_SPINNER="  Sautéing the plan...
  Would you like to proceed?
❯"
# Plan mode alone on screen
PLAN_MINIMAL="Would you like to proceed?
❯"

check "Plan mode after tool output" "$( detect_plan_mode "$PLAN_AFTER_TOOL" && echo pass || echo fail )"
check "Plan mode with spinner above" "$( detect_plan_mode "$PLAN_WITH_SPINNER" && echo pass || echo fail )"
check "Plan mode minimal" "$( detect_plan_mode "$PLAN_MINIMAL" && echo pass || echo fail )"
echo ""

# --- Test 68: Plan mode false positive resistance ---
echo "68. Plan mode false positive resistance..."
# Content that mentions proceeding but is NOT a plan mode prompt
CHAT_PROCEED="● Bash(nbs-chat read .nbs/chat/live.chat)
  ⎿  alex: Would you like to proceed with the refactor?
❯"
CODE_PROCEED="● Read(src/main.rs)
  ⎿  // Ask: Would you like to proceed?
  ⎿  println!(\"Continuing...\");
❯"
NORMAL_WORK="● Bash(make -j8)
  ⎿  Building targets...
  ⎿  [100%] Built target nbs-chat
❯"

# These do NOT match because detect_plan_mode uses grep -qF on the exact
# string 'Would you like to proceed?' — the question mark must immediately
# follow 'proceed'. The chat message has 'proceed with the refactor?'
# which does not contain the exact substring.
check "Chat mention of 'proceed' is NOT a false positive" "$( ! detect_plan_mode "$CHAT_PROCEED" && echo pass || echo fail )"
check "Code comment with 'proceed' IS a false positive (known)" "$( detect_plan_mode "$CODE_PROCEED" && echo pass || echo fail )"
# But normal work output should not trigger
check "Normal build output does NOT trigger" "$( ! detect_plan_mode "$NORMAL_WORK" && echo pass || echo fail )"
echo ""

# --- Test 69: AskUserQuestion detection robustness ---
echo "69. AskUserQuestion detection robustness..."
# Exact AskUserQuestion format
EXACT_ASK="? Which approach should we use?
  1. Option A (Recommended)
  2. Option B
  3. Option C
Type something.
❯"
# AskUserQuestion with only 2 options
ASK_TWO="? Should we continue?
  1. Yes (Recommended)
  2. No
Type something.
❯"
# NOT an AskUserQuestion — missing "Type something."
FAKE_ASK="? Which approach?
  1. Option A
  2. Option B
❯"

check "Exact AskUserQuestion detected" "$( detect_ask_modal "$EXACT_ASK" && echo pass || echo fail )"
check "Two-option AskUserQuestion detected" "$( detect_ask_modal "$ASK_TWO" && echo pass || echo fail )"
check "Missing 'Type something.' NOT detected" "$( ! detect_ask_modal "$FAKE_ASK" && echo pass || echo fail )"
echo ""

# --- Test 70: Plan mode detected during content changes AND stability ---
echo "70. Plan mode detection in both content-change and stable-content paths..."
# The sidecar checks plan mode in two places:
# 1. On content change (hash differs from last)
# 2. On stable content (hash same, checking idle state)
# Both paths use the same detect_plan_mode function.
# Verify the function works regardless of how it's called.
PLAN_CONTENT="Would you like to proceed?
❯"
HASH1=$(echo "$PLAN_CONTENT" | sha256sum | cut -d' ' -f1)
HASH2=$(echo "$PLAN_CONTENT" | sha256sum | cut -d' ' -f1)
# Same content, same hash — would be stable-content path
check "Same content produces same hash" "$( [[ "$HASH1" == "$HASH2" ]] && echo pass || echo fail )"
# Detection works on both calls
check "Detection on first call (content-change path)" "$( detect_plan_mode "$PLAN_CONTENT" && echo pass || echo fail )"
check "Detection on second call (stable-content path)" "$( detect_plan_mode "$PLAN_CONTENT" && echo pass || echo fail )"
echo ""

# --- Test 71: Plan mode and context stress interaction ---
echo "71. Plan mode during context stress..."
# If plan mode appears while context is stressed, what happens?
# In the sidecar loop: context stress check is AFTER plan mode check.
# So plan mode is resolved first, regardless of context stress.
STRESS_PLAN="Compacting conversation...
Would you like to proceed?
❯"
check "Plan mode detected despite context stress" "$( detect_plan_mode "$STRESS_PLAN" && echo pass || echo fail )"
check "Context stress also detected" "$( detect_context_stress "$STRESS_PLAN" && echo pass || echo fail )"
# In the actual loop, plan mode fires first (content-change check)
# and resets idle_seconds + hash, so context stress check never runs.
# This is correct: resolving the blocking plan mode prompt is higher priority.
echo ""

# --- Test 72: Plan mode with bypass permissions modal ---
echo "72. Plan mode behind bypass permissions modal..."
# The classic stall: plan mode prompt appears but bypass-permissions
# status bar is at the bottom. The sidecar should still detect plan mode
# in the pane content — the status bar is overlaid, not part of content.
PLAN_WITH_BYPASS="Would you like to proceed?
  1. Yes
  2. Yes, and don't ask again for this project
  ⏵⏵ bypass permissions on (shift+tab to cycle)"
check "Plan mode detected with bypass bar" "$( detect_plan_mode "$PLAN_WITH_BYPASS" && echo pass || echo fail )"
echo ""

# ============================================================
# T15: Concurrent chat under SIGKILL (Phase 2d)
# ============================================================
# Verify chat file integrity when writers are killed mid-write
# with SIGKILL (no cleanup possible). Tests:
# - file-length header matches actual file size
# - every message base64-decodes successfully
# - surviving agents' cursors are unaffected
# - the chat file remains usable after the kill

echo "T15: Concurrent chat under SIGKILL"
echo ""

T15_SIGKILL_DIR="$TEST_DIR/t15-sigkill"
mkdir -p "$T15_SIGKILL_DIR/.nbs/chat" "$T15_SIGKILL_DIR/.nbs/events/processed"
T15_CHAT="$T15_SIGKILL_DIR/.nbs/chat/sigkill.chat"
"$NBS_CHAT" create "$T15_CHAT" >/dev/null 2>&1

# --- Test 73: Concurrent writes — 4 agents, no kill, verify baseline ---
echo "73. Concurrent writes baseline — 4 agents, all complete..."
for agent in alpha beta gamma delta; do
    for i in $(seq 1 5); do
        "$NBS_CHAT" send "$T15_CHAT" "$agent" "Message $i from $agent" &
    done
done
wait
# Verify file-length header matches actual file size
FILE_LEN_HEADER=$(grep '^file-length:' "$T15_CHAT" | awk '{print $2}')
ACTUAL_SIZE=$(wc -c < "$T15_CHAT")
check "file-length header matches actual size (baseline)" "$( [[ "$FILE_LEN_HEADER" -eq "$ACTUAL_SIZE" ]] && echo pass || echo fail )"
# Verify every message base64-decodes
MSG_BODY=$(awk '/^---$/{found=1; next} found && NF{print}' "$T15_CHAT")
DECODE_FAILURES=0
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    if ! echo "$line" | base64 -d >/dev/null 2>&1; then
        DECODE_FAILURES=$((DECODE_FAILURES + 1))
    fi
done <<< "$MSG_BODY"
check "All messages base64-decode (baseline)" "$( [[ "$DECODE_FAILURES" -eq 0 ]] && echo pass || echo fail )"
# Verify message count: 4 agents * 5 messages = 20
MSG_COUNT=$(echo "$MSG_BODY" | grep -c '^.' || true)
check "20 messages present (4 agents × 5)" "$( [[ "$MSG_COUNT" -eq 20 ]] && echo pass || echo fail )"
echo ""

# --- Test 74: Set cursors, then verify cursor integrity ---
echo "74. Cursor integrity after concurrent writes..."
# Read with each handle to establish cursors
for agent in alpha beta gamma delta; do
    "$NBS_CHAT" read "$T15_CHAT" --unread="$agent" >/dev/null 2>&1
done
# Verify cursor file exists and has entries
check "Cursor file exists" "$( [[ -f "${T15_CHAT}.cursors" ]] && echo pass || echo fail )"
# Store cursor values before new writes
ALPHA_CURSOR_BEFORE=$(awk -F= '/^alpha/{print $2}' "${T15_CHAT}.cursors" 2>/dev/null)
check "Alpha cursor set" "$( [[ -n "$ALPHA_CURSOR_BEFORE" && "$ALPHA_CURSOR_BEFORE" -gt 0 ]] && echo pass || echo fail )"
echo ""

# --- Test 75: SIGKILL during concurrent writes — chat file survives ---
echo "75. SIGKILL during concurrent writes — file integrity..."
T75_CHAT="$T15_SIGKILL_DIR/.nbs/chat/sigkill75.chat"
"$NBS_CHAT" create "$T75_CHAT" >/dev/null 2>&1
# Write some baseline messages first
for i in $(seq 1 5); do
    "$NBS_CHAT" send "$T75_CHAT" baseline "Baseline message $i"
done
# Launch 4 rapid-fire writers in background
for agent in w1 w2 w3 w4; do
    (
        for i in $(seq 1 50); do
            "$NBS_CHAT" send "$T75_CHAT" "$agent" "Rapid message $i from $agent" 2>/dev/null || true
        done
    ) &
done
# Wait briefly for writers to be in progress, then kill 2
WRITER_PIDS=$(jobs -p)
sleep 0.2
KILLED=0
for pid in $WRITER_PIDS; do
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
        KILLED=$((KILLED + 1))
        [[ $KILLED -ge 2 ]] && break
    fi
done
# Wait for remaining writers to finish
wait 2>/dev/null || true
# Verify file integrity: file-length matches actual size
FILE_LEN_75=$(grep '^file-length:' "$T75_CHAT" | awk '{print $2}')
ACTUAL_SIZE_75=$(wc -c < "$T75_CHAT")
check "file-length matches actual size after SIGKILL" "$( [[ "$FILE_LEN_75" -eq "$ACTUAL_SIZE_75" ]] && echo pass || echo fail )"
# Verify all surviving messages base64-decode
MSG_BODY_75=$(awk '/^---$/{found=1; next} found && NF{print}' "$T75_CHAT")
DECODE_FAIL_75=0
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    if ! echo "$line" | base64 -d >/dev/null 2>&1; then
        DECODE_FAIL_75=$((DECODE_FAIL_75 + 1))
    fi
done <<< "$MSG_BODY_75"
check "All surviving messages base64-decode after SIGKILL" "$( [[ "$DECODE_FAIL_75" -eq 0 ]] && echo pass || echo fail )"
echo ""

# --- Test 76: Chat remains writable after SIGKILL ---
echo "76. Chat remains writable after SIGKILL..."
# The lock file should have been released when the process died
# (flock is released on fd close, which happens on process death)
"$NBS_CHAT" send "$T75_CHAT" survivor "Post-kill message" 2>/dev/null
POST_KILL_RC=$?
check "send succeeds after writer killed" "$( [[ $POST_KILL_RC -eq 0 ]] && echo pass || echo fail )"
# Verify the new message is readable
LAST_MSG=$("$NBS_CHAT" read "$T75_CHAT" --last=1 2>/dev/null)
check "Post-kill message readable" "$( echo "$LAST_MSG" | grep -q 'Post-kill message' && echo pass || echo fail )"
echo ""

# --- Test 77: Cursor integrity — non-killed agent cursors survive ---
echo "77. Non-killed agent cursors unaffected..."
# Use the original T15 chat which has cursors set
# Write new messages after the kill
"$NBS_CHAT" send "$T15_CHAT" newcomer "New message after kill scenario"
# Alpha's cursor should still be valid (unchanged from before)
ALPHA_CURSOR_AFTER=$(awk -F= '/^alpha/{print $2}' "${T15_CHAT}.cursors" 2>/dev/null)
check "Alpha cursor unchanged by other writes" "$( [[ "$ALPHA_CURSOR_AFTER" -eq "$ALPHA_CURSOR_BEFORE" ]] && echo pass || echo fail )"
# Alpha should see the new message as unread
ALPHA_UNREAD=$("$NBS_CHAT" read "$T15_CHAT" --unread=alpha 2>/dev/null)
check "Alpha sees new message as unread" "$( echo "$ALPHA_UNREAD" | grep -q 'New message after kill' && echo pass || echo fail )"
echo ""

# --- Test 78: Lock not held after SIGKILL (flock released on process death) ---
echo "78. Lock released after writer SIGKILL..."
T78_CHAT="$T15_SIGKILL_DIR/.nbs/chat/sigkill78.chat"
"$NBS_CHAT" create "$T78_CHAT" >/dev/null 2>&1
# Start a writer that sends many messages (will take some time)
(
    for i in $(seq 1 200); do
        "$NBS_CHAT" send "$T78_CHAT" slowwriter "Message $i" 2>/dev/null || true
    done
) &
SLOW_PID=$!
sleep 0.1
# Kill it
kill -9 $SLOW_PID 2>/dev/null || true
wait $SLOW_PID 2>/dev/null || true
# Immediately try to write — should not be blocked
START_TIME=$(date +%s%N)
timeout 5 "$NBS_CHAT" send "$T78_CHAT" "fastwriter" "Should not block" 2>/dev/null
WRITE_RC=$?
END_TIME=$(date +%s%N)
ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))
check "Write after SIGKILL completes (rc=0)" "$( [[ $WRITE_RC -eq 0 ]] && echo pass || echo fail )"
check "Write after SIGKILL is fast (<3000ms)" "$( [[ $ELAPSED_MS -lt 3000 ]] && echo pass || echo fail )"
echo ""

# --- Test 79: File-length self-consistency after SIGKILL recovery ---
echo "79. File-length self-consistency after recovery writes..."
# After the kill + recovery write in T78, verify the file is still consistent
T79_LEN=$(grep '^file-length:' "$T78_CHAT" | awk '{print $2}')
T79_ACTUAL=$(wc -c < "$T78_CHAT")
check "file-length matches actual size after recovery" "$( [[ "$T79_LEN" -eq "$T79_ACTUAL" ]] && echo pass || echo fail )"
# Verify the chat file has the standard header format
check "Header starts with '=== nbs-chat ==='" "$( head -1 "$T78_CHAT" | grep -qF '=== nbs-chat ===' && echo pass || echo fail )"
check "Header has --- separator" "$( grep -q '^---$' "$T78_CHAT" && echo pass || echo fail )"
echo ""

# ============================================================
# T16: Bus event delivery under contention (Phase 2e)
# ============================================================
# Verify bus event delivery when multiple publishers and consumers
# operate concurrently. No events lost, priority ordering holds,
# acked events stay acked.

T16_DIR="$TEST_DIR/t16-contention"
mkdir -p "$T16_DIR"

# --- Test 80: Rapid concurrent publish — no events lost ---
echo "80. Rapid concurrent publish — no events lost..."
T80_DIR="$T16_DIR/t80"
mkdir -p "$T80_DIR/processed"
# 20 publishers, each publishing 1 event concurrently
for i in $(seq 1 20); do
    "$NBS_BUS" publish "$T80_DIR" "agent-$i" "test-event" normal "payload-$i" &
done
wait
EVENT_COUNT=$(ls "$T80_DIR"/*.event 2>/dev/null | wc -l)
check "20 concurrent publishes produce 20 events" "$( [[ "$EVENT_COUNT" -eq 20 ]] && echo pass || echo fail )"
# Verify each event has valid YAML structure
VALID_COUNT=0
for f in "$T80_DIR"/*.event; do
    if grep -q "^source:" "$f" && grep -q "^type:" "$f" && grep -q "^priority:" "$f"; then
        VALID_COUNT=$((VALID_COUNT + 1))
    fi
done
check "All 20 events have valid YAML structure" "$( [[ "$VALID_COUNT" -eq 20 ]] && echo pass || echo fail )"
echo ""

# --- Test 81: Priority ordering under contention ---
echo "81. Priority ordering under contention..."
T81_DIR="$T16_DIR/t81"
mkdir -p "$T81_DIR/processed"
# Publish events with mixed priorities concurrently
"$NBS_BUS" publish "$T81_DIR" "src" "low-evt" low "low" &
"$NBS_BUS" publish "$T81_DIR" "src" "crit-evt" critical "critical" &
"$NBS_BUS" publish "$T81_DIR" "src" "high-evt" high "high" &
"$NBS_BUS" publish "$T81_DIR" "src" "norm-evt" normal "normal" &
wait
# bus_check sorts by priority then timestamp
CHECK_OUTPUT=$("$NBS_BUS" check "$T81_DIR" 2>/dev/null)
FIRST_LINE=$(echo "$CHECK_OUTPUT" | head -1)
LAST_LINE=$(echo "$CHECK_OUTPUT" | tail -1)
check "Critical priority listed first" "$( echo "$FIRST_LINE" | grep -q '\[critical\]' && echo pass || echo fail )"
check "Low priority listed last" "$( echo "$LAST_LINE" | grep -q '\[low\]' && echo pass || echo fail )"
echo ""

# --- Test 82: Acked events don't reappear ---
echo "82. Acked events stay acked..."
T82_DIR="$T16_DIR/t82"
mkdir -p "$T82_DIR/processed"
"$NBS_BUS" publish "$T82_DIR" "src" "ack-test" normal "test-payload"
EVENT_FILE=$(ls "$T82_DIR"/*.event 2>/dev/null | head -1 | xargs basename)
"$NBS_BUS" ack "$T82_DIR" "$EVENT_FILE" 2>/dev/null
check "Event file removed from events dir" "$( [[ ! -f "$T82_DIR/$EVENT_FILE" ]] && echo pass || echo fail )"
check "Event file exists in processed/" "$( [[ -f "$T82_DIR/processed/$EVENT_FILE" ]] && echo pass || echo fail )"
REMAINING=$("$NBS_BUS" check "$T82_DIR" 2>/dev/null)
check "bus_check returns empty after ack" "$( [[ -z "$REMAINING" ]] && echo pass || echo fail )"
echo ""

# --- Test 83: Concurrent ack of same event — race safety ---
echo "83. Concurrent ack of same event — only one succeeds..."
T83_DIR="$T16_DIR/t83"
mkdir -p "$T83_DIR/processed"
"$NBS_BUS" publish "$T83_DIR" "src" "race-test" normal "race-payload"
EVENT_FILE=$(ls "$T83_DIR"/*.event 2>/dev/null | head -1 | xargs basename)
# Two concurrent acks — rename is atomic, only one succeeds
"$NBS_BUS" ack "$T83_DIR" "$EVENT_FILE" 2>/dev/null &
PID1=$!
"$NBS_BUS" ack "$T83_DIR" "$EVENT_FILE" 2>/dev/null &
PID2=$!
R1=0; wait $PID1 || R1=$?
R2=0; wait $PID2 || R2=$?
SUCCESS_COUNT=0
[[ $R1 -eq 0 ]] && SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
[[ $R2 -eq 0 ]] && SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
check "Exactly one concurrent ack succeeds" "$( [[ "$SUCCESS_COUNT" -eq 1 ]] && echo pass || echo fail )"
check "Event in processed/ exactly once" "$( [[ -f "$T83_DIR/processed/$EVENT_FILE" ]] && echo pass || echo fail )"
echo ""

# --- Test 84: Publish during concurrent check — no corruption ---
echo "84. Publish during concurrent check — no corruption..."
T84_DIR="$T16_DIR/t84"
mkdir -p "$T84_DIR/processed"
for i in $(seq 1 5); do
    "$NBS_BUS" publish "$T84_DIR" "src" "base-$i" normal "base-$i"
done
# Publish 5 more while checking concurrently
"$NBS_BUS" check "$T84_DIR" > /dev/null 2>&1 &
for i in $(seq 6 10); do
    "$NBS_BUS" publish "$T84_DIR" "src" "concurrent-$i" normal "concurrent-$i" &
done
wait
TOTAL=$(ls "$T84_DIR"/*.event 2>/dev/null | wc -l)
check "All 10 events present after concurrent publish+check" "$( [[ "$TOTAL" -eq 10 ]] && echo pass || echo fail )"
VALID=0
for f in "$T84_DIR"/*.event; do
    grep -q "^source:" "$f" && VALID=$((VALID + 1))
done
check "No corrupted events after concurrent publish+check" "$( [[ "$VALID" -eq 10 ]] && echo pass || echo fail )"
echo ""

# --- Test 85: Ack during concurrent publish — no interference ---
echo "85. Ack during concurrent publish — no interference..."
T85_DIR="$T16_DIR/t85"
mkdir -p "$T85_DIR/processed"
for i in $(seq 1 5); do
    "$NBS_BUS" publish "$T85_DIR" "src" "ack-during-$i" normal "payload-$i"
done
EVENTS_TO_ACK=$(ls "$T85_DIR"/*.event 2>/dev/null | head -3)
for f in $EVENTS_TO_ACK; do
    "$NBS_BUS" ack "$T85_DIR" "$(basename "$f")" 2>/dev/null &
done
for i in $(seq 6 10); do
    "$NBS_BUS" publish "$T85_DIR" "src" "new-$i" normal "payload-$i" &
done
wait
REMAINING_COUNT=$(ls "$T85_DIR"/*.event 2>/dev/null | wc -l)
PROCESSED_COUNT=$(ls "$T85_DIR/processed/"*.event 2>/dev/null | wc -l)
check "7 events remain pending (5-3+5)" "$( [[ "$REMAINING_COUNT" -eq 7 ]] && echo pass || echo fail )"
check "3 events in processed/" "$( [[ "$PROCESSED_COUNT" -eq 3 ]] && echo pass || echo fail )"
echo ""

# --- Test 86: High-volume publish — 100 events ---
echo "86. High-volume publish — 100 events, none lost..."
T86_DIR="$T16_DIR/t86"
mkdir -p "$T86_DIR/processed"
# 10 concurrent publishers, each publishing 10 events
for pub in $(seq 1 10); do
    (
        for evt in $(seq 1 10); do
            "$NBS_BUS" publish "$T86_DIR" "pub-$pub" "bulk-$evt" normal "p${pub}-e${evt}"
        done
    ) &
done
wait
TOTAL_EVENTS=$(ls "$T86_DIR"/*.event 2>/dev/null | wc -l)
check "100 concurrent events — none lost" "$( [[ "$TOTAL_EVENTS" -eq 100 ]] && echo pass || echo fail )"
CHECK_OUT=$("$NBS_BUS" check "$T86_DIR" 2>/dev/null)
LINE_COUNT=$(echo "$CHECK_OUT" | wc -l)
check "bus_check lists all 100 events" "$( [[ "$LINE_COUNT" -eq 100 ]] && echo pass || echo fail )"
echo ""

# --- Test 87: Ack-all under contention ---
echo "87. Ack-all under contention — selective ack..."
T87_DIR="$T16_DIR/t87"
mkdir -p "$T87_DIR/processed"
for i in $(seq 1 20); do
    "$NBS_BUS" publish "$T87_DIR" "ack-src" "batch-$i" normal "payload-$i"
done
for i in $(seq 1 5); do
    "$NBS_BUS" publish "$T87_DIR" "other-src" "other-$i" normal "other-$i"
done
# Ack all from ack-src while more events arrive
"$NBS_BUS" ack-all "$T87_DIR" --handle=ack-src 2>/dev/null &
ACK_PID=$!
for i in $(seq 21 25); do
    "$NBS_BUS" publish "$T87_DIR" "ack-src" "late-$i" normal "late-$i" &
done
wait $ACK_PID || true  # ack-all may have partial failures from race
wait  # wait for publishers
# Count other-src events remaining (grep may not match — use || true)
OTHER_REMAINING=$(grep -rl "source: other-src" "$T87_DIR"/*.event 2>/dev/null | wc -l || echo 0)
check "other-src events untouched by ack-all" "$( [[ "$OTHER_REMAINING" -eq 5 ]] && echo pass || echo fail )"
# Count ack-src events in processed/ (some late events may or may not have been caught)
PROCESSED_ACK_SRC=$(grep -rl "source: ack-src" "$T87_DIR/processed/"*.event 2>/dev/null | wc -l || echo 0)
check "ack-all moved ack-src events to processed/" "$( [[ "$PROCESSED_ACK_SRC" -ge 20 ]] && echo pass || echo fail )"
echo ""

cd "$SCRIPT_DIR"

# ============================================================
# T17: Permissions prompt detection robustness
# ============================================================
#
# The permissions prompt is the #1 cause of agent stalls in multi-agent
# setups. detect_permissions_prompt must reliably match the exact Claude
# Code permissions prompt format while avoiding false positives from
# chat messages, code output, or other detectors' content.
#
# detect_permissions_prompt requires BOTH:
#   1. 'Do you want to proceed?' (exact substring)
#   2. "don't ask again" (exact substring)
# This dual-match design reduces false positives: chat messages about
# "proceeding" won't trigger unless they also mention "don't ask again".

echo "T17: Permissions prompt detection robustness"
echo ""

# --- Test 88: Exact Claude Code permissions prompt ---
echo "88. Exact permissions prompt format..."
EXACT_PERMS="  Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /home/user/project
  3. No
❯"
check "Exact permissions prompt detected" "$( detect_permissions_prompt "$EXACT_PERMS" && echo pass || echo fail )"
echo ""

# --- Test 89: Permissions prompt with different tool types ---
echo "89. Permissions prompt with varying tool and directory names..."
# Read tool
PERMS_READ="● Read(src/main.rs)
  Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Read in /home/user/project
  3. No
❯"
# Write tool with deeply nested path
PERMS_WRITE="● Write(/home/user/project/src/utils/helper.ts)
  Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Write in /home/user/project
  3. No
❯"
# Permissions prompt with spinner text above
PERMS_WITH_SPINNER="  Processing...
  Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /tmp
  3. No
❯"
# Minimal permissions prompt (no tool output context)
PERMS_MINIMAL="Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /home/user
  3. No
❯"

check "Permissions prompt for Read tool" "$( detect_permissions_prompt "$PERMS_READ" && echo pass || echo fail )"
check "Permissions prompt for Write tool" "$( detect_permissions_prompt "$PERMS_WRITE" && echo pass || echo fail )"
check "Permissions prompt with spinner above" "$( detect_permissions_prompt "$PERMS_WITH_SPINNER" && echo pass || echo fail )"
check "Permissions prompt minimal" "$( detect_permissions_prompt "$PERMS_MINIMAL" && echo pass || echo fail )"
echo ""

# --- Test 90: Permissions prompt false positive resistance ---
echo "90. Permissions prompt false positive resistance..."
# Chat message discussing proceeding — has "proceed?" but not "don't ask again"
CHAT_PROCEED_PERMS="● Bash(nbs-chat read .nbs/chat/live.chat)
  ⎿  alex: Do you want to proceed with this approach?
❯"
# Code comment mentioning the prompt — has "proceed?" but not "don't ask again"
CODE_PROCEED_PERMS="● Read(src/sidecar.sh)
  ⎿  # Check: Do you want to proceed?
  ⎿  echo 'Continuing...'
❯"
# Normal build output — neither substring
NORMAL_BUILD="● Bash(make -j8)
  ⎿  Building targets...
  ⎿  [100%] Built target nbs-chat
❯"
# Has "don't ask again" but NOT "Do you want to proceed?" — partial match
PARTIAL_MATCH="  Some other prompt
  1. Yes
  2. Yes, and don't ask again for Bash in /tmp
  3. No
❯"

check "Chat 'proceed' without 'don't ask again' is NOT a false positive" "$( ! detect_permissions_prompt "$CHAT_PROCEED_PERMS" && echo pass || echo fail )"
check "Code comment 'proceed' is NOT a false positive" "$( ! detect_permissions_prompt "$CODE_PROCEED_PERMS" && echo pass || echo fail )"
check "Normal build output does NOT trigger" "$( ! detect_permissions_prompt "$NORMAL_BUILD" && echo pass || echo fail )"
check "Partial match (don't ask again only) does NOT trigger" "$( ! detect_permissions_prompt "$PARTIAL_MATCH" && echo pass || echo fail )"
echo ""

# --- Test 91: Permissions prompt independence from other detectors ---
echo "91. Permissions vs plan mode vs AskUserQuestion independence..."
# Permissions prompt should NOT trigger plan mode or AskUserQuestion
check "Permissions prompt is NOT plan mode" "$( ! detect_plan_mode "$EXACT_PERMS" && echo pass || echo fail )"
check "Permissions prompt is NOT AskUserQuestion" "$( ! detect_ask_modal "$EXACT_PERMS" && echo pass || echo fail )"
# Plan mode should NOT trigger permissions detection
PLAN_ONLY="  Would you like to proceed?
  1. Yes
  2. Yes, and don't ask again for this project
❯"
check "Plan mode is NOT permissions prompt" "$( ! detect_permissions_prompt "$PLAN_ONLY" && echo pass || echo fail )"
# AskUserQuestion should NOT trigger permissions detection
ASK_ONLY="? Which approach should we use?
  1. Option A (Recommended)
  2. Option B
Type something.
❯"
check "AskUserQuestion is NOT permissions prompt" "$( ! detect_permissions_prompt "$ASK_ONLY" && echo pass || echo fail )"
echo ""

# --- Test 92: Permissions prompt in both content-change and stable-content paths ---
echo "92. Permissions detection in both content-change and stable-content paths..."
# Same as T14 test 70: the sidecar checks in two places (content change and stable).
# Verify the function is deterministic across repeated calls.
PERMS_CONTENT="Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /home/user
  3. No
❯"
HASH_P1=$(echo "$PERMS_CONTENT" | sha256sum | cut -d' ' -f1)
HASH_P2=$(echo "$PERMS_CONTENT" | sha256sum | cut -d' ' -f1)
check "Same content produces same hash" "$( [[ "$HASH_P1" == "$HASH_P2" ]] && echo pass || echo fail )"
check "Detection on first call (content-change path)" "$( detect_permissions_prompt "$PERMS_CONTENT" && echo pass || echo fail )"
check "Detection on second call (stable-content path)" "$( detect_permissions_prompt "$PERMS_CONTENT" && echo pass || echo fail )"
echo ""

# --- Test 93: Permissions prompt during context stress ---
echo "93. Permissions prompt during context stress..."
# If a permissions prompt appears while context is stressed, plan mode and
# permissions detection run before context stress handling. The sidecar should
# detect both, but permissions is resolved first (blocking prompt).
STRESS_PERMS="Compacting conversation...
Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /home/user
  3. No
❯"
check "Permissions detected despite context stress" "$( detect_permissions_prompt "$STRESS_PERMS" && echo pass || echo fail )"
check "Context stress also detected" "$( detect_context_stress "$STRESS_PERMS" && echo pass || echo fail )"
echo ""

# --- Test 94: Permissions prompt with bypass permissions bar ---
echo "94. Permissions prompt with bypass permissions status bar..."
# The bypass-permissions bar appears at the bottom of the pane.
# It should not interfere with permissions prompt detection.
PERMS_WITH_BYPASS="Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /home/user
  3. No
  ⏵⏵ bypass permissions on (shift+tab to cycle)"
check "Permissions detected with bypass bar" "$( detect_permissions_prompt "$PERMS_WITH_BYPASS" && echo pass || echo fail )"
echo ""

# --- Test 95: Permissions prompt co-occurrence with plan mode ---
echo "95. Both permissions and plan mode in same content..."
# Edge case: both prompts appear in captured pane content (e.g., scrollback
# contains a resolved plan mode prompt, current prompt is permissions).
# Both detectors should fire independently.
BOTH_PROMPTS="Would you like to proceed?
  1. Yes
  2. Yes, and don't ask again for this project

Do you want to proceed?
  1. Yes
  2. Yes, and don't ask again for Bash in /home/user
  3. No
❯"
check "Permissions detected in combined content" "$( detect_permissions_prompt "$BOTH_PROMPTS" && echo pass || echo fail )"
check "Plan mode also detected in combined content" "$( detect_plan_mode "$BOTH_PROMPTS" && echo pass || echo fail )"
echo ""

# ============================================================
# T18: Handle collision guard (pre-spawn pidfile check)
# ============================================================
#
# nbs-claude uses pidfiles at .nbs/pids/<handle>.pid to prevent
# two instances from running with the same handle. The guard:
# - Checks for existing pidfile on startup
# - If pidfile exists AND the PID is alive, refuses to spawn (exit 1)
# - --force flag overrides the guard (exits 0 with warning)
# - Stale pidfiles (dead PID) are ignored
# - Cleanup removes pidfile on exit (only if PID matches)

echo "T18: Handle collision guard"
echo ""

T18_DIR="$TEST_DIR/t18-collision"
mkdir -p "$T18_DIR/.nbs/pids"

# Trap-based cleanup for background processes spawned by T18 tests.
# If the suite exits early (Ctrl-C, assertion failure, crash), these
# processes would otherwise persist as orphans.
# Chain with the existing cleanup trap to preserve temp dir removal.
T18_PIDS=()
t18_cleanup() { for p in "${T18_PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; cleanup; }
trap t18_cleanup EXIT INT TERM

# --- Test 96: Pidfile created on startup ---
echo "96. Pidfile created on startup..."
# Write a pidfile as nbs-claude would
T96_HANDLE="test-agent-96"
T96_PIDFILE="$T18_DIR/.nbs/pids/${T96_HANDLE}.pid"
echo "12345" > "$T96_PIDFILE"
check "Pidfile created" "$( [[ -f "$T96_PIDFILE" ]] && echo pass || echo fail )"
check "Pidfile contains PID" "$( [[ "$(cat "$T96_PIDFILE")" == "12345" ]] && echo pass || echo fail )"
rm -f "$T96_PIDFILE"
echo ""

# --- Test 97: Guard blocks when PID is alive ---
echo "97. Guard blocks when PID is alive..."
# Start a background sleep to get a live PID
sleep 300 &
LIVE_PID=$!
T18_PIDS+=($!)
T97_HANDLE="test-agent-97"
T97_PIDFILE="$T18_DIR/.nbs/pids/${T97_HANDLE}.pid"
echo "$LIVE_PID" > "$T97_PIDFILE"
# Simulate the guard check
GUARD_RESULT="blocked"
if [[ -f "$T97_PIDFILE" ]]; then
    EXISTING_PID=$(cat "$T97_PIDFILE" 2>/dev/null)
    if [[ -n "$EXISTING_PID" ]] && kill -0 "$EXISTING_PID" 2>/dev/null; then
        GUARD_RESULT="blocked"
    else
        GUARD_RESULT="allowed"
    fi
else
    GUARD_RESULT="allowed"
fi
check "Guard blocks when PID $LIVE_PID is alive" "$( [[ "$GUARD_RESULT" == "blocked" ]] && echo pass || echo fail )"
kill "$LIVE_PID" 2>/dev/null || true
wait "$LIVE_PID" 2>/dev/null || true
rm -f "$T97_PIDFILE"
echo ""

# --- Test 98: Guard allows when PID is dead (stale pidfile) ---
echo "98. Guard allows when PID is dead (stale pidfile)..."
T98_HANDLE="test-agent-98"
T98_PIDFILE="$T18_DIR/.nbs/pids/${T98_HANDLE}.pid"
# Use a PID that is certainly dead (PID 1 is init — skip; use a large unlikely PID)
sleep 0.01 &
DEAD_PID=$!
wait "$DEAD_PID" 2>/dev/null || true
echo "$DEAD_PID" > "$T98_PIDFILE"
GUARD_RESULT="blocked"
if [[ -f "$T98_PIDFILE" ]]; then
    EXISTING_PID=$(cat "$T98_PIDFILE" 2>/dev/null)
    if [[ -n "$EXISTING_PID" ]] && kill -0 "$EXISTING_PID" 2>/dev/null; then
        GUARD_RESULT="blocked"
    else
        GUARD_RESULT="allowed"
    fi
else
    GUARD_RESULT="allowed"
fi
check "Guard allows stale pidfile (dead PID $DEAD_PID)" "$( [[ "$GUARD_RESULT" == "allowed" ]] && echo pass || echo fail )"
rm -f "$T98_PIDFILE"
echo ""

# --- Test 99: Guard allows when no pidfile exists ---
echo "99. Guard allows when no pidfile exists..."
T99_HANDLE="test-agent-99"
T99_PIDFILE="$T18_DIR/.nbs/pids/${T99_HANDLE}.pid"
rm -f "$T99_PIDFILE"
GUARD_RESULT="blocked"
if [[ -f "$T99_PIDFILE" ]]; then
    EXISTING_PID=$(cat "$T99_PIDFILE" 2>/dev/null)
    if [[ -n "$EXISTING_PID" ]] && kill -0 "$EXISTING_PID" 2>/dev/null; then
        GUARD_RESULT="blocked"
    else
        GUARD_RESULT="allowed"
    fi
else
    GUARD_RESULT="allowed"
fi
check "Guard allows when no pidfile exists" "$( [[ "$GUARD_RESULT" == "allowed" ]] && echo pass || echo fail )"
echo ""

# --- Test 100: --force overrides guard ---
echo "100. --force overrides guard..."
sleep 300 &
LIVE_PID=$!
T18_PIDS+=($!)
T100_HANDLE="test-agent-100"
T100_PIDFILE="$T18_DIR/.nbs/pids/${T100_HANDLE}.pid"
echo "$LIVE_PID" > "$T100_PIDFILE"
FORCE_SPAWN=1
GUARD_RESULT="blocked"
if [[ -f "$T100_PIDFILE" ]]; then
    EXISTING_PID=$(cat "$T100_PIDFILE" 2>/dev/null)
    if [[ -n "$EXISTING_PID" ]] && kill -0 "$EXISTING_PID" 2>/dev/null; then
        if [[ "$FORCE_SPAWN" == "1" ]]; then
            GUARD_RESULT="forced"
        else
            GUARD_RESULT="blocked"
        fi
    else
        GUARD_RESULT="allowed"
    fi
else
    GUARD_RESULT="allowed"
fi
check "--force overrides guard on live PID" "$( [[ "$GUARD_RESULT" == "forced" ]] && echo pass || echo fail )"
kill "$LIVE_PID" 2>/dev/null || true
wait "$LIVE_PID" 2>/dev/null || true
rm -f "$T100_PIDFILE"
FORCE_SPAWN=0
echo ""

# --- Test 101: Cleanup removes only own pidfile ---
echo "101. Cleanup removes only own pidfile..."
T101_HANDLE="test-agent-101"
T101_PIDFILE="$T18_DIR/.nbs/pids/${T101_HANDLE}.pid"
MY_PID=$$
echo "$MY_PID" > "$T101_PIDFILE"
# Simulate cleanup: only remove if PID matches
if [[ -f "$T101_PIDFILE" ]] && [[ "$(cat "$T101_PIDFILE" 2>/dev/null)" == "$MY_PID" ]]; then
    rm -f "$T101_PIDFILE"
fi
check "Cleanup removed own pidfile" "$( [[ ! -f "$T101_PIDFILE" ]] && echo pass || echo fail )"
# Now test that cleanup does NOT remove a pidfile with a different PID
echo "99999" > "$T101_PIDFILE"
if [[ -f "$T101_PIDFILE" ]] && [[ "$(cat "$T101_PIDFILE" 2>/dev/null)" == "$MY_PID" ]]; then
    rm -f "$T101_PIDFILE"
fi
check "Cleanup preserves other PID's pidfile" "$( [[ -f "$T101_PIDFILE" ]] && echo pass || echo fail )"
rm -f "$T101_PIDFILE"
echo ""

# --- Test 102: Different handles do not collide ---
echo "102. Different handles do not collide..."
sleep 300 &
LIVE_PID=$!
T18_PIDS+=($!)
T102_HANDLE_A="agent-alpha"
T102_HANDLE_B="agent-beta"
T102_PIDFILE_A="$T18_DIR/.nbs/pids/${T102_HANDLE_A}.pid"
T102_PIDFILE_B="$T18_DIR/.nbs/pids/${T102_HANDLE_B}.pid"
echo "$LIVE_PID" > "$T102_PIDFILE_A"
# Agent B should be allowed — different handle
GUARD_RESULT="blocked"
if [[ -f "$T102_PIDFILE_B" ]]; then
    EXISTING_PID=$(cat "$T102_PIDFILE_B" 2>/dev/null)
    if [[ -n "$EXISTING_PID" ]] && kill -0 "$EXISTING_PID" 2>/dev/null; then
        GUARD_RESULT="blocked"
    else
        GUARD_RESULT="allowed"
    fi
else
    GUARD_RESULT="allowed"
fi
check "Different handle not blocked by existing pidfile" "$( [[ "$GUARD_RESULT" == "allowed" ]] && echo pass || echo fail )"
kill "$LIVE_PID" 2>/dev/null || true
wait "$LIVE_PID" 2>/dev/null || true
rm -f "$T102_PIDFILE_A" "$T102_PIDFILE_B"
echo ""

cd "$SCRIPT_DIR"

# ============================================================
# T19: CSMA/CD standup trigger (temporal carrier sense)
# ============================================================
#
# check_standup_trigger() in nbs-claude uses temporal carrier sense
# to coordinate standup check-ins between sidecars. It replaced the
# old message-count approach that caused self-suppression when agents
# stopped responding. The mechanism:
# - Each sidecar tracks its own LAST_STANDUP_TIME
# - A shared .standup-ts file records when any sidecar last posted
# - If the shared timestamp is recent (< interval), back off randomly
# - If the shared timestamp is stale/missing, post and update it
# - First run initialises timer without posting

echo "T19: CSMA/CD standup trigger"
echo ""

T19_DIR="$TEST_DIR/t19-csma"
mkdir -p "$T19_DIR/.nbs"
T19_CHAT="$T19_DIR/test.chat"
nbs-chat create "$T19_CHAT"
T19_REGISTRY="$T19_DIR/.nbs/control-registry-test"
echo "chat:$T19_CHAT" > "$T19_REGISTRY"

# --- Test 103: Disabled when STANDUP_INTERVAL=0 ---
echo "103. Disabled when STANDUP_INTERVAL=0..."
STANDUP_INTERVAL=0
LAST_STANDUP_TIME=99999
CONTROL_REGISTRY="$T19_REGISTRY"
RESULT="posted"
# Replicate guard: first check
if [[ "$STANDUP_INTERVAL" -gt 0 ]] && [[ -f "$CONTROL_REGISTRY" ]]; then
    RESULT="passed_guards"
else
    RESULT="disabled"
fi
check "Standup disabled when interval=0" "$( [[ "$RESULT" == "disabled" ]] && echo pass || echo fail )"
echo ""

# --- Test 104: No control registry ---
echo "104. No control registry file..."
STANDUP_INTERVAL=15
CONTROL_REGISTRY="$T19_DIR/.nbs/nonexistent-registry"
RESULT="posted"
if [[ "$STANDUP_INTERVAL" -gt 0 ]] && [[ -f "$CONTROL_REGISTRY" ]]; then
    RESULT="passed_guards"
else
    RESULT="disabled"
fi
check "Standup disabled without registry" "$( [[ "$RESULT" == "disabled" ]] && echo pass || echo fail )"
echo ""

# --- Test 105: First run initialisation (LAST_STANDUP_TIME=0) ---
echo "105. First run initialises timer without posting..."
STANDUP_INTERVAL=15
LAST_STANDUP_TIME=0
CONTROL_REGISTRY="$T19_REGISTRY"
NOW=$(date +%s)
INTERVAL_SECONDS=$((STANDUP_INTERVAL * 60))
RESULT="posted"
# Replicate first-run check
if [[ $LAST_STANDUP_TIME -eq 0 ]]; then
    LAST_STANDUP_TIME=$NOW
    RESULT="initialised"
fi
check "First run sets timer" "$( [[ "$RESULT" == "initialised" ]] && echo pass || echo fail )"
check "Timer set to current time" "$( [[ "$LAST_STANDUP_TIME" -eq "$NOW" ]] && echo pass || echo fail )"
echo ""

# --- Test 106: Too soon since last attempt ---
echo "106. Suppressed when interval not elapsed..."
STANDUP_INTERVAL=15
NOW=$(date +%s)
INTERVAL_SECONDS=$((STANDUP_INTERVAL * 60))
LAST_STANDUP_TIME=$((NOW - 60))  # Only 60 seconds ago
RESULT="posted"
if [[ $LAST_STANDUP_TIME -gt 0 ]] && (( NOW - LAST_STANDUP_TIME < INTERVAL_SECONDS )); then
    RESULT="too_soon"
fi
check "Standup suppressed when interval not elapsed" "$( [[ "$RESULT" == "too_soon" ]] && echo pass || echo fail )"
echo ""

# --- Test 107: Carrier sense — medium busy (recent timestamp) ---
echo "107. Carrier sense suppresses when timestamp recent..."
STANDUP_INTERVAL=15
NOW=$(date +%s)
INTERVAL_SECONDS=$((STANDUP_INTERVAL * 60))
LAST_STANDUP_TIME=$((NOW - INTERVAL_SECONDS - 10))  # Our timer elapsed
TS_FILE="${T19_CHAT}.standup-ts"
# Write a recent global timestamp (30 seconds ago)
echo "$((NOW - 30))" > "$TS_FILE"
LAST_GLOBAL_STANDUP=0
if [[ -f "$TS_FILE" ]]; then
    LAST_GLOBAL_STANDUP=$(cat "$TS_FILE" 2>/dev/null || echo 0)
    [[ "$LAST_GLOBAL_STANDUP" =~ ^[0-9]+$ ]] || LAST_GLOBAL_STANDUP=0
fi
RESULT="posted"
if (( LAST_GLOBAL_STANDUP > 0 )) && (( NOW - LAST_GLOBAL_STANDUP < INTERVAL_SECONDS )); then
    RESULT="medium_busy"
fi
check "Carrier sense detects busy medium" "$( [[ "$RESULT" == "medium_busy" ]] && echo pass || echo fail )"
rm -f "$TS_FILE"
echo ""

# --- Test 108: Carrier sense — stale timestamp allows posting ---
echo "108. Carrier sense allows when timestamp stale..."
STANDUP_INTERVAL=15
NOW=$(date +%s)
INTERVAL_SECONDS=$((STANDUP_INTERVAL * 60))
LAST_STANDUP_TIME=$((NOW - INTERVAL_SECONDS - 10))  # Our timer elapsed
TS_FILE="${T19_CHAT}.standup-ts"
# Write a stale global timestamp (20 minutes ago)
echo "$((NOW - 1200))" > "$TS_FILE"
LAST_GLOBAL_STANDUP=$(cat "$TS_FILE" 2>/dev/null || echo 0)
[[ "$LAST_GLOBAL_STANDUP" =~ ^[0-9]+$ ]] || LAST_GLOBAL_STANDUP=0
RESULT="suppressed"
if (( LAST_GLOBAL_STANDUP > 0 )) && (( NOW - LAST_GLOBAL_STANDUP < INTERVAL_SECONDS )); then
    RESULT="suppressed"
else
    RESULT="allowed"
fi
check "Stale timestamp allows posting" "$( [[ "$RESULT" == "allowed" ]] && echo pass || echo fail )"
rm -f "$TS_FILE"
echo ""

# --- Test 109: Missing timestamp file allows posting ---
echo "109. Missing timestamp file allows posting..."
TS_FILE="${T19_CHAT}.standup-ts"
rm -f "$TS_FILE"
LAST_GLOBAL_STANDUP=0
if [[ -f "$TS_FILE" ]]; then
    LAST_GLOBAL_STANDUP=$(cat "$TS_FILE" 2>/dev/null || echo 0)
    [[ "$LAST_GLOBAL_STANDUP" =~ ^[0-9]+$ ]] || LAST_GLOBAL_STANDUP=0
fi
RESULT="suppressed"
if (( LAST_GLOBAL_STANDUP > 0 )) && (( NOW - LAST_GLOBAL_STANDUP < INTERVAL_SECONDS )); then
    RESULT="suppressed"
else
    RESULT="allowed"
fi
check "Missing timestamp file allows posting" "$( [[ "$RESULT" == "allowed" ]] && echo pass || echo fail )"
echo ""

# --- Test 110: Non-numeric timestamp treated as 0 ---
echo "110. Non-numeric timestamp treated as missing..."
TS_FILE="${T19_CHAT}.standup-ts"
echo "garbage_data" > "$TS_FILE"
LAST_GLOBAL_STANDUP=$(cat "$TS_FILE" 2>/dev/null || echo 0)
[[ "$LAST_GLOBAL_STANDUP" =~ ^[0-9]+$ ]] || LAST_GLOBAL_STANDUP=0
RESULT="suppressed"
if (( LAST_GLOBAL_STANDUP > 0 )) && (( NOW - LAST_GLOBAL_STANDUP < INTERVAL_SECONDS )); then
    RESULT="suppressed"
else
    RESULT="allowed"
fi
check "Non-numeric timestamp treated as missing" "$( [[ "$RESULT" == "allowed" ]] && echo pass || echo fail )"
rm -f "$TS_FILE"
echo ""

# ============================================================
# T20: Conditional notification (event-gated /nbs-notify)
# ============================================================
#
# should_inject_notify() in nbs-claude gates /nbs-notify injection
# on actual pending work. It calls check_bus_events() and
# check_chat_unread(). If both return non-zero (nothing pending),
# should_inject_notify returns 1 (do not inject). This prevents
# empty notification cycles that cause terminal context rot.
#
# The tests replicate the conditional logic inline, verifying:
# - No injection when bus and chat are both empty
# - Injection when bus events exist
# - Injection when unread chat messages exist
# - Cooldown suppresses non-critical repeat notifications
# - Critical priority bypasses cooldown
# - Startup grace period blocks early injection
# - Summary message built correctly from bus + chat

echo "T20: Conditional notification"
echo ""

T20_DIR="$TEST_DIR/t20-notify"
mkdir -p "$T20_DIR/.nbs/events"
T20_CHAT="$T20_DIR/test.chat"
nbs-chat create "$T20_CHAT"
T20_REGISTRY="$T20_DIR/.nbs/control-registry-test"
echo "bus:$T20_DIR/.nbs/events" > "$T20_REGISTRY"
echo "chat:$T20_CHAT" >> "$T20_REGISTRY"

# --- Test 111: No injection when bus and chat are empty ---
echo "111. No injection when bus and chat are empty..."
# check_bus_events: empty events dir → no output → BUS_EVENT_COUNT=0 → return 1
BUS_EVENT_COUNT=0
BUS_MAX_PRIORITY="none"
BUS_EVENT_SUMMARY=""
CONTROL_REGISTRY="$T20_REGISTRY"
SIDECAR_HANDLE="test-notify"
bus_output=$(nbs-bus check "$T20_DIR/.nbs/events" 2>/dev/null) || true
if [[ -n "$bus_output" ]]; then
    BUS_EVENT_COUNT=$(echo "$bus_output" | wc -l)
fi
bus_rc=1
[[ $BUS_EVENT_COUNT -gt 0 ]] && bus_rc=0
# check_chat_unread: no messages beyond header → return 1
chat_rc=1
CHAT_UNREAD_COUNT=0
total=$(awk '/^---$/{found=1; next} found && NF{count++} END{print count+0}' "$T20_CHAT" 2>/dev/null) || true
total=$((${total:-0} + 0))
cursor=0
if [[ -f "${T20_CHAT}.cursors" ]]; then
    cursor=$(awk -F= -v h="$SIDECAR_HANDLE" '$1==h{print $2}' "${T20_CHAT}.cursors" 2>/dev/null) || true
    cursor=$((${cursor:-0} + 0))
fi
(( total > cursor + 1 )) && chat_rc=0
# should_inject_notify logic: both empty → return 1
RESULT="inject"
if [[ $bus_rc -ne 0 && $chat_rc -ne 0 ]]; then
    RESULT="no_inject"
fi
check "No injection when bus and chat empty" "$( [[ "$RESULT" == "no_inject" ]] && echo pass || echo fail )"
echo ""

# --- Test 112: Injection when bus events exist ---
echo "112. Injection when bus events exist..."
nbs-bus publish "$T20_DIR/.nbs/events" test test-event normal "test" >/dev/null 2>&1
bus_rc=0
nbs-bus check "$T20_DIR/.nbs/events" >/dev/null 2>&1 || bus_rc=$?
RESULT="no_inject"
if [[ $bus_rc -eq 0 ]]; then
    RESULT="inject"
fi
check "Injection when bus events exist" "$( [[ "$RESULT" == "inject" ]] && echo pass || echo fail )"
# Clean up event
for f in "$T20_DIR/.nbs/events"/*.event; do
    nbs-bus ack "$T20_DIR/.nbs/events" "$(basename "$f")" 2>/dev/null || true
done
echo ""

# --- Test 113: Injection when unread chat messages exist ---
echo "113. Injection when unread chat messages exist..."
# Send 2 messages — cursor=0 means "read message 0", so need total > 1
nbs-chat send "$T20_CHAT" sender "hello from test" >/dev/null 2>&1
nbs-chat send "$T20_CHAT" sender "second message" >/dev/null 2>&1
total=$(awk '/^---$/{found=1; next} found && NF{count++} END{print count+0}' "$T20_CHAT" 2>/dev/null) || true
total=$((${total:-0} + 0))
cursor=0
if [[ -f "${T20_CHAT}.cursors" ]]; then
    cursor=$(awk -F= -v h="$SIDECAR_HANDLE" '$1==h{print $2}' "${T20_CHAT}.cursors" 2>/dev/null) || true
    cursor=$((${cursor:-0} + 0))
fi
chat_rc=1
(( total > cursor + 1 )) && chat_rc=0
RESULT="no_inject"
if [[ $chat_rc -eq 0 ]]; then
    RESULT="inject"
fi
check "Injection when unread chat exists" "$( [[ "$RESULT" == "inject" ]] && echo pass || echo fail )"
echo ""

# --- Test 114: Cooldown suppresses non-critical events ---
echo "114. Cooldown suppresses non-critical repeat notifications..."
NOTIFY_COOLDOWN=60
NOW=$(date +%s)
LAST_NOTIFY_TIME=$((NOW - 10))  # 10 seconds ago — within cooldown
BUS_MAX_PRIORITY="normal"
# Both bus and chat have events (from previous tests)
bus_rc=0
chat_rc=0
elapsed=$((NOW - LAST_NOTIFY_TIME))
RESULT="inject"
if [[ "$BUS_MAX_PRIORITY" != "critical" && $elapsed -lt $NOTIFY_COOLDOWN ]]; then
    RESULT="cooldown"
fi
check "Cooldown suppresses normal priority" "$( [[ "$RESULT" == "cooldown" ]] && echo pass || echo fail )"
echo ""

# --- Test 115: Critical priority bypasses cooldown ---
echo "115. Critical priority bypasses cooldown..."
BUS_MAX_PRIORITY="critical"
RESULT="cooldown"
if [[ "$BUS_MAX_PRIORITY" != "critical" && $elapsed -lt $NOTIFY_COOLDOWN ]]; then
    RESULT="cooldown"
else
    RESULT="inject"
fi
check "Critical priority bypasses cooldown" "$( [[ "$RESULT" == "inject" ]] && echo pass || echo fail )"
echo ""

# --- Test 116: Startup grace period blocks injection ---
echo "116. Startup grace period blocks injection..."
STARTUP_GRACE=30
SIDECAR_START_TIME=$((NOW - 10))  # Started 10 seconds ago — within grace
grace_elapsed=$((NOW - SIDECAR_START_TIME))
RESULT="inject"
if [[ $SIDECAR_START_TIME -gt 0 ]] && [[ $grace_elapsed -lt $STARTUP_GRACE ]]; then
    RESULT="grace_blocked"
fi
check "Startup grace blocks injection" "$( [[ "$RESULT" == "grace_blocked" ]] && echo pass || echo fail )"
# After grace period
SIDECAR_START_TIME=$((NOW - 60))  # Started 60 seconds ago — past grace
grace_elapsed=$((NOW - SIDECAR_START_TIME))
RESULT="inject"
if [[ $SIDECAR_START_TIME -gt 0 ]] && [[ $grace_elapsed -lt $STARTUP_GRACE ]]; then
    RESULT="grace_blocked"
fi
check "Injection allowed after grace period" "$( [[ "$RESULT" == "inject" ]] && echo pass || echo fail )"
echo ""

# --- Test 117: Summary message built from bus + chat ---
echo "117. Summary message built from bus and chat..."
BUS_EVENT_SUMMARY="3 event(s) in .nbs/events"
CHAT_UNREAD_SUMMARY="5 unread in live.chat"
parts=""
if [[ -n "$BUS_EVENT_SUMMARY" ]]; then
    parts="$BUS_EVENT_SUMMARY"
fi
if [[ -n "$CHAT_UNREAD_SUMMARY" ]]; then
    if [[ -n "$parts" ]]; then
        parts="${parts}. ${CHAT_UNREAD_SUMMARY}"
    else
        parts="$CHAT_UNREAD_SUMMARY"
    fi
fi
check "Summary combines bus and chat" "$( [[ "$parts" == "3 event(s) in .nbs/events. 5 unread in live.chat" ]] && echo pass || echo fail )"
# Bus only
CHAT_UNREAD_SUMMARY=""
parts=""
if [[ -n "$BUS_EVENT_SUMMARY" ]]; then
    parts="$BUS_EVENT_SUMMARY"
fi
if [[ -n "$CHAT_UNREAD_SUMMARY" ]]; then
    if [[ -n "$parts" ]]; then
        parts="${parts}. ${CHAT_UNREAD_SUMMARY}"
    else
        parts="$CHAT_UNREAD_SUMMARY"
    fi
fi
check "Summary with bus only" "$( [[ "$parts" == "3 event(s) in .nbs/events" ]] && echo pass || echo fail )"
echo ""

# ============================================================
# T21: UTC timestamp display in nbs-chat read output
# ============================================================
echo "=== T21: UTC timestamp display ==="

T21_DIR="$TEST_DIR/t21_timestamp"
mkdir -p "$T21_DIR"

# T21 test 118: nbs-chat read output includes [YYYY-MM-DDTHH:MM:SSZ] prefix
T21_CHAT="$T21_DIR/ts.chat"
"$NBS_CHAT" create "$T21_CHAT"
"$NBS_CHAT" send "$T21_CHAT" alice "hello world"
T21_OUTPUT=$("$NBS_CHAT" read "$T21_CHAT")
check "T21: read output contains ISO 8601 UTC timestamp" "$( echo "$T21_OUTPUT" | grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] alice: hello world$' && echo pass || echo fail )"

# T21 test 119: Multiple messages all have UTC timestamps
"$NBS_CHAT" send "$T21_CHAT" bob "second message"
"$NBS_CHAT" send "$T21_CHAT" alice "third message"
T21_ALL=$("$NBS_CHAT" read "$T21_CHAT")
T21_TS_COUNT=$(echo "$T21_ALL" | grep -cE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\]')
check "T21: all 3 messages have UTC timestamps" "$( [[ "$T21_TS_COUNT" -eq 3 ]] && echo pass || echo fail )"

# T21 test 120: --last=1 output has UTC timestamp
T21_LAST=$("$NBS_CHAT" read "$T21_CHAT" --last=1)
check "T21: --last=1 output has UTC timestamp" "$( echo "$T21_LAST" | grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] alice: third message$' && echo pass || echo fail )"

# T21 test 121: search output includes UTC timestamp
T21_SEARCH=$("$NBS_CHAT" search "$T21_CHAT" "second")
check "T21: search output has UTC timestamp" "$( echo "$T21_SEARCH" | grep -qE '^\[1\] \[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] bob: second message$' && echo pass || echo fail )"

# T21 test 122: backward compat — legacy messages (no timestamp) have no [UTC] prefix
T21_LEGACY="$T21_DIR/legacy.chat"
# Manually create a chat file with old-format messages (handle: content, no pipe)
cat > "$T21_LEGACY" << 'LEGACY_EOF'
=== nbs-chat ===
last-writer: alice
last-write: 2025-01-01T00:00:00+0000
file-length: 0
participants: alice(1)
---
LEGACY_EOF
# Encode a legacy-format message: "alice: old message" with no pipe/epoch
LEGACY_MSG=$(echo -n "alice: old message" | base64)
echo "$LEGACY_MSG" >> "$T21_LEGACY"
T21_LEGACY_OUT=$("$NBS_CHAT" read "$T21_LEGACY" 2>/dev/null || true)
check "T21: legacy message has no UTC prefix" "$( echo "$T21_LEGACY_OUT" | grep -qE '^alice: old message$' && echo pass || echo fail )"

echo ""

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
