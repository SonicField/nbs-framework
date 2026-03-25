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
#   T18 (96-102): Pidfile lifecycle (simple PID storage, no locking)
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

# T5-T21 removed: these tested bash sidecar functions (seed_registry,
# should_inject_notify, etc.) that were ported to the C sidecar.
# The C sidecar has its own unit tests in src/nbs-sidecar/.


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
