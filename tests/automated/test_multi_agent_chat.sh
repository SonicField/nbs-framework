#!/bin/bash
# Test: Multi-agent chat integration
#
# Exercises the exact failure modes observed in production:
# 8 agents sharing a chat file, concurrent writes, poll races,
# high-frequency messaging, read-under-write, filter correctness.
#
# These tests go beyond the lifecycle tests (test 10: 50 concurrent sends)
# by testing the multi-agent coordination PATTERNS, not just raw concurrency.
#
# Deterministic tests:
#   1.  8 concurrent writers — simulates 8-agent team
#   2.  Poll-then-send — agent waits, another posts
#   3.  High-frequency burst — 100 messages from 4 senders
#   4.  Read-under-write — continuous reader + continuous writer
#   5.  Participants accuracy after multi-agent session
#   6.  --since filter with interleaved senders
#   7.  --unread filter with multiple agents
#   8.  Large message round-trip (10KB mixed content)
#   9.  Rapid create-send-read cycle (agent restart pattern)
#   10. Header integrity after stress (file-length correct)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
    # Kill any lingering background processes from this test
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null || true
    wait 2>/dev/null || true
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

echo "=== Multi-Agent Chat Integration Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# --- Test 1: 8 concurrent writers (simulates 8-agent team) ---
echo "1. 8 concurrent writers..."
CHAT="$TEST_DIR/test1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

AGENT_NAMES=("theologian" "generalist" "testkeeper" "pythia" "scribe" "gatekeeper" "claude" "arm-remote")
MSGS_PER_AGENT=10
TOTAL_EXPECTED=$(( ${#AGENT_NAMES[@]} * MSGS_PER_AGENT ))

PIDS=()
for agent in "${AGENT_NAMES[@]}"; do
    (
        for i in $(seq 1 $MSGS_PER_AGENT); do
            "$NBS_CHAT" send "$CHAT" "$agent" "Agent $agent reporting message $i"
        done
    ) &
    PIDS+=($!)
done

SEND_FAILURES=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid" 2>/dev/null; then
        SEND_FAILURES=$((SEND_FAILURES + 1))
    fi
done

check "No send failures from 8 agents" "$( [[ "$SEND_FAILURES" -eq 0 ]] && echo pass || echo fail )"

OUTPUT=$("$NBS_CHAT" read "$CHAT")
MSG_COUNT=$(echo "$OUTPUT" | grep -c '.' || true)
check "$TOTAL_EXPECTED messages present" "$( [[ "$MSG_COUNT" -eq "$TOTAL_EXPECTED" ]] && echo pass || echo fail )"

# Verify each agent's messages are all present
for agent in "${AGENT_NAMES[@]}"; do
    AGENT_COUNT=$(echo "$OUTPUT" | grep -c "^${agent}:" || true)
    check "$agent has $MSGS_PER_AGENT messages" "$( [[ "$AGENT_COUNT" -eq "$MSGS_PER_AGENT" ]] && echo pass || echo fail )"
done

# Verify per-sender ordering is monotonic (message N appears before message N+1)
ORDER_OK=true
for agent in "${AGENT_NAMES[@]}"; do
    PREV=0
    while IFS= read -r line; do
        NUM=$(echo "$line" | sed -n 's/.*message \([0-9]*\).*/\1/p')
        if [[ -n "$NUM" && "$NUM" -le "$PREV" ]]; then
            ORDER_OK=false
            break
        fi
        PREV=$NUM
    done < <(echo "$OUTPUT" | grep "^${agent}:")
done
check "Per-sender ordering is monotonic" "$( $ORDER_OK && echo pass || echo fail )"

# Header integrity after concurrent writes
HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$CHAT")
check "Header file-length correct after 8-agent stress" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"

echo ""

# --- Test 2: Poll-then-send (agent waits, another posts) ---
echo "2. Poll-then-send..."
CHAT="$TEST_DIR/test2.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Agent A polls, agent B sends after a delay
(
    sleep 2
    "$NBS_CHAT" send "$CHAT" "theologian" "Task complete, reporting back"
) &
BG_PID=$!

set +e
POLL_OUTPUT=$("$NBS_CHAT" poll "$CHAT" "generalist" --timeout=10 2>/dev/null)
POLL_RC=$?
set -e
wait "$BG_PID" 2>/dev/null || true

check "Poll exits 0" "$( [[ "$POLL_RC" -eq 0 ]] && echo pass || echo fail )"
check "Poll receives theologian's message" "$( echo "$POLL_OUTPUT" | grep -qF 'theologian: Task complete' && echo pass || echo fail )"

echo ""

# --- Test 3: High-frequency burst — 100 messages from 4 senders ---
echo "3. High-frequency burst (100 messages, 4 senders)..."
CHAT="$TEST_DIR/test3.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

SENDERS=("worker-a" "worker-b" "worker-c" "worker-d")
BURST_TOTAL=100
PER_SENDER=$((BURST_TOTAL / ${#SENDERS[@]}))

PIDS=()
for sender in "${SENDERS[@]}"; do
    (
        for i in $(seq 1 $PER_SENDER); do
            "$NBS_CHAT" send "$CHAT" "$sender" "Burst $i from $sender"
        done
    ) &
    PIDS+=($!)
done

BURST_FAILURES=0
for pid in "${PIDS[@]}"; do
    if ! wait "$pid" 2>/dev/null; then
        BURST_FAILURES=$((BURST_FAILURES + 1))
    fi
done

check "No burst send failures" "$( [[ "$BURST_FAILURES" -eq 0 ]] && echo pass || echo fail )"

OUTPUT=$("$NBS_CHAT" read "$CHAT")
BURST_COUNT=$(echo "$OUTPUT" | grep -c '.' || true)
check "$BURST_TOTAL messages after burst" "$( [[ "$BURST_COUNT" -eq "$BURST_TOTAL" ]] && echo pass || echo fail )"

# Each sender contributed equally
for sender in "${SENDERS[@]}"; do
    S_COUNT=$(echo "$OUTPUT" | grep -c "^${sender}:" || true)
    check "$sender sent $PER_SENDER" "$( [[ "$S_COUNT" -eq "$PER_SENDER" ]] && echo pass || echo fail )"
done

# Per-sender monotonic ordering
BURST_ORDER_OK=true
for sender in "${SENDERS[@]}"; do
    PREV=0
    while IFS= read -r line; do
        NUM=$(echo "$line" | sed -n 's/.*Burst \([0-9]*\).*/\1/p')
        if [[ -n "$NUM" && "$NUM" -le "$PREV" ]]; then
            BURST_ORDER_OK=false
            break
        fi
        PREV=$NUM
    done < <(echo "$OUTPUT" | grep "^${sender}:")
done
check "Per-sender burst ordering monotonic" "$( $BURST_ORDER_OK && echo pass || echo fail )"

echo ""

# --- Test 4: Read-under-write ---
echo "4. Read-under-write (concurrent reader + writer)..."
CHAT="$TEST_DIR/test4.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Writer sends 50 messages
(
    for i in $(seq 1 50); do
        "$NBS_CHAT" send "$CHAT" "writer" "Write $i"
    done
) &
WRITER_PID=$!

# Reader reads continuously while writer is active
READ_ERRORS=0
for _ in $(seq 1 20); do
    set +e
    READ_OUT=$("$NBS_CHAT" read "$CHAT" 2>&1)
    READ_RC=$?
    set -e
    if [[ "$READ_RC" -ne 0 ]]; then
        READ_ERRORS=$((READ_ERRORS + 1))
    fi
    # Check no partial/corrupt lines (every line must match "handle: message" format)
    if [[ -n "$READ_OUT" ]]; then
        BAD_LINES=$(echo "$READ_OUT" | grep -cvE '^[a-zA-Z0-9_-]+: ' || true)
        if [[ "$BAD_LINES" -gt 0 ]]; then
            READ_ERRORS=$((READ_ERRORS + 1))
        fi
    fi
done

wait "$WRITER_PID" 2>/dev/null || true
check "No read errors during concurrent write" "$( [[ "$READ_ERRORS" -eq 0 ]] && echo pass || echo fail )"

# Final read should show all 50
FINAL=$("$NBS_CHAT" read "$CHAT")
FINAL_COUNT=$(echo "$FINAL" | grep -c '.' || true)
check "All 50 messages present after read-under-write" "$( [[ "$FINAL_COUNT" -eq 50 ]] && echo pass || echo fail )"

echo ""

# --- Test 5: Participants accuracy after multi-agent session ---
echo "5. Participants accuracy..."
CHAT="$TEST_DIR/test5.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

"$NBS_CHAT" send "$CHAT" "theologian" "msg1" >/dev/null
"$NBS_CHAT" send "$CHAT" "theologian" "msg2" >/dev/null
"$NBS_CHAT" send "$CHAT" "theologian" "msg3" >/dev/null
"$NBS_CHAT" send "$CHAT" "generalist" "msg1" >/dev/null
"$NBS_CHAT" send "$CHAT" "generalist" "msg2" >/dev/null
"$NBS_CHAT" send "$CHAT" "claude" "msg1" >/dev/null
"$NBS_CHAT" send "$CHAT" "pythia" "msg1" >/dev/null
"$NBS_CHAT" send "$CHAT" "pythia" "msg2" >/dev/null
"$NBS_CHAT" send "$CHAT" "pythia" "msg3" >/dev/null
"$NBS_CHAT" send "$CHAT" "pythia" "msg4" >/dev/null

PARTS=$("$NBS_CHAT" participants "$CHAT")
check "theologian: 3 messages" "$( echo "$PARTS" | grep 'theologian' | grep -q '3 messages' && echo pass || echo fail )"
check "generalist: 2 messages" "$( echo "$PARTS" | grep 'generalist' | grep -q '2 messages' && echo pass || echo fail )"
check "claude: 1 messages" "$( echo "$PARTS" | grep 'claude' | grep -q '1 messages' && echo pass || echo fail )"
check "pythia: 4 messages" "$( echo "$PARTS" | grep 'pythia' | grep -q '4 messages' && echo pass || echo fail )"
check "4 unique participants" "$( echo "$PARTS" | grep -c 'messages' | awk '{print ($1 == 4) ? "pass" : "fail"}' )"

echo ""

# --- Test 6: --since filter with interleaved senders ---
echo "6. --since filter with interleaved senders..."
CHAT="$TEST_DIR/test6.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

"$NBS_CHAT" send "$CHAT" "theologian" "Diagnosis complete" >/dev/null
"$NBS_CHAT" send "$CHAT" "generalist" "Starting phase 1" >/dev/null
"$NBS_CHAT" send "$CHAT" "claude" "Wave 3 in progress" >/dev/null
"$NBS_CHAT" send "$CHAT" "theologian" "Startup fix landed" >/dev/null
"$NBS_CHAT" send "$CHAT" "alex" "Good work everyone" >/dev/null
"$NBS_CHAT" send "$CHAT" "pythia" "Checkpoint ready" >/dev/null

# --since=generalist should return everything after generalist's last post
SINCE_OUT=$("$NBS_CHAT" read "$CHAT" --since=generalist)
SINCE_COUNT=$(echo "$SINCE_OUT" | grep -c '.' || true)
check "--since=generalist returns 4 messages" "$( [[ "$SINCE_COUNT" -eq 4 ]] && echo pass || echo fail )"
check "First is claude" "$( echo "$SINCE_OUT" | head -1 | grep -qF 'claude:' && echo pass || echo fail )"
check "Last is pythia" "$( echo "$SINCE_OUT" | tail -1 | grep -qF 'pythia:' && echo pass || echo fail )"

# --since for handle that posted last — should return nothing
SINCE_LAST=$("$NBS_CHAT" read "$CHAT" --since=pythia)
check "--since last poster returns nothing" "$( [[ -z "$SINCE_LAST" ]] && echo pass || echo fail )"

# --since for handle not in chat — should return all messages
SINCE_NEW=$("$NBS_CHAT" read "$CHAT" --since=newagent)
SINCE_NEW_COUNT=$(echo "$SINCE_NEW" | grep -c '.' || true)
check "--since unknown handle returns all 6" "$( [[ "$SINCE_NEW_COUNT" -eq 6 ]] && echo pass || echo fail )"

echo ""

# --- Test 7: --unread filter with multiple agents ---
echo "7. --unread filter with multiple agents..."
CHAT="$TEST_DIR/test7.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

"$NBS_CHAT" send "$CHAT" "theologian" "First batch" >/dev/null
"$NBS_CHAT" send "$CHAT" "claude" "Also first batch" >/dev/null

# Agent A reads — sees 2
UNREAD_A1=$("$NBS_CHAT" read "$CHAT" --unread=generalist)
check "Agent A first read: 2 messages" "$( echo "$UNREAD_A1" | wc -l | awk '{print ($1 == 2) ? "pass" : "fail"}' )"

# Agent B reads — also sees 2 (independent cursor)
UNREAD_B1=$("$NBS_CHAT" read "$CHAT" --unread=testkeeper)
check "Agent B first read: 2 messages" "$( echo "$UNREAD_B1" | wc -l | awk '{print ($1 == 2) ? "pass" : "fail"}' )"

# New messages arrive
"$NBS_CHAT" send "$CHAT" "pythia" "Second batch" >/dev/null

# Agent A reads — sees 1 new
UNREAD_A2=$("$NBS_CHAT" read "$CHAT" --unread=generalist)
check "Agent A second read: 1 new message" "$( echo "$UNREAD_A2" | wc -l | awk '{print ($1 == 1) ? "pass" : "fail"}' )"
check "Agent A sees pythia's message" "$( echo "$UNREAD_A2" | grep -qF 'pythia: Second batch' && echo pass || echo fail )"

# Agent B reads — also sees 1 new (independent cursor)
UNREAD_B2=$("$NBS_CHAT" read "$CHAT" --unread=testkeeper)
check "Agent B second read: 1 new message" "$( echo "$UNREAD_B2" | wc -l | awk '{print ($1 == 1) ? "pass" : "fail"}' )"

# Agent A reads again — nothing new
UNREAD_A3=$("$NBS_CHAT" read "$CHAT" --unread=generalist)
check "Agent A third read: nothing new" "$( [[ -z "$UNREAD_A3" ]] && echo pass || echo fail )"

echo ""

# --- Test 8: Large message round-trip (10KB mixed content) ---
echo "8. Large message round-trip (10KB)..."
CHAT="$TEST_DIR/test8.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Generate 10KB message with mixed content: printable ASCII, unicode, newlines, special chars
LARGE_MSG=""
for i in $(seq 1 100); do
    LARGE_MSG+="Line $i: The quick brown fox jumps — über the «lazy» dog. Special: \$PATH=/usr/bin:~/local;echo 'done'"
    LARGE_MSG+=$'\n'
done

"$NBS_CHAT" send "$CHAT" "claude" "$LARGE_MSG"
OUTPUT=$("$NBS_CHAT" read "$CHAT")

# Verify content survived round-trip
check "Large message contains line 1" "$( echo "$OUTPUT" | grep -qF 'Line 1:' && echo pass || echo fail )"
check "Large message contains line 100" "$( echo "$OUTPUT" | grep -qF 'Line 100:' && echo pass || echo fail )"
check "Unicode preserved in large message" "$( echo "$OUTPUT" | grep -qF 'über' && echo pass || echo fail )"
check "Special chars preserved" "$( echo "$OUTPUT" | grep -qF 'PATH=/usr/bin' && echo pass || echo fail )"

# Header integrity after large message
HEADER_LENGTH=$(grep '^file-length:' "$CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$CHAT")
check "Header correct after large message" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"

echo ""

# --- Test 9: Rapid create-send-read cycle (agent restart pattern) ---
echo "9. Rapid create-send-read cycle (restart simulation)..."

# Simulates the pattern: agent restarts, catches up on chat, posts status
RESTART_OK=true
for i in $(seq 1 10); do
    CHAT="$TEST_DIR/test9_$i.chat"
    "$NBS_CHAT" create "$CHAT" >/dev/null

    # Pre-existing messages (from before "restart")
    "$NBS_CHAT" send "$CHAT" "alex" "Task $i assigned" >/dev/null
    "$NBS_CHAT" send "$CHAT" "theologian" "Working on it" >/dev/null

    # "Restarted" agent reads and responds
    CATCHUP=$("$NBS_CHAT" read "$CHAT")
    if ! echo "$CATCHUP" | grep -qF "Task $i assigned"; then
        RESTART_OK=false
        break
    fi

    "$NBS_CHAT" send "$CHAT" "generalist" "Back online, caught up" >/dev/null

    FINAL=$("$NBS_CHAT" read "$CHAT")
    FINAL_COUNT=$(echo "$FINAL" | grep -c '.' || true)
    if [[ "$FINAL_COUNT" -ne 3 ]]; then
        RESTART_OK=false
        break
    fi
done
check "10 restart cycles all succeed" "$( $RESTART_OK && echo pass || echo fail )"

echo ""

# --- Test 10: Header integrity after all stress ---
echo "10. Header integrity after stress..."
# Re-check test 1's chat (the most stressed file)
STRESS_CHAT="$TEST_DIR/test1.chat"
HEADER_LENGTH=$(grep '^file-length:' "$STRESS_CHAT" | sed 's/^file-length:[[:space:]]*//')
ACTUAL_LENGTH=$(wc -c < "$STRESS_CHAT")
check "Stress file header length matches actual" "$( [[ "$HEADER_LENGTH" -eq "$ACTUAL_LENGTH" ]] && echo pass || echo fail )"

# Verify no corrupt base64 in stressed file
RAW_MESSAGES=$(sed -n '/^---$/,$ p' "$STRESS_CHAT" | tail -n +2)
CORRUPT=0
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    if ! echo "$line" | base64 -d >/dev/null 2>&1; then
        CORRUPT=$((CORRUPT + 1))
    fi
done <<< "$RAW_MESSAGES"
check "No corrupt base64 after stress" "$( [[ "$CORRUPT" -eq 0 ]] && echo pass || echo fail )"

# Verify participants header matches actual senders
HEADER_PARTS=$(grep '^participants:' "$STRESS_CHAT" | sed 's/^participants:[[:space:]]*//')
for agent in "${AGENT_NAMES[@]}"; do
    check "Participant $agent in header" "$( echo "$HEADER_PARTS" | grep -qF "$agent" && echo pass || echo fail )"
done

echo ""

# --- Summary ---
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All multi-agent integration tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
