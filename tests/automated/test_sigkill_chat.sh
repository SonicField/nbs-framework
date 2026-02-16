#!/bin/bash
# Test: Concurrent chat under SIGKILL (Phase 2d / T15)
#
# Verifies chat file integrity when agents are killed mid-write.
# SIGKILL gives no cleanup opportunity, so integrity depends
# entirely on flock serialisation and atomic write semantics.
#
# Tests:
#   73. 4 writers, SIGKILL 2 mid-write
#   74. Cursor isolation after SIGKILL
#   75. Restarted agent reads full history
#   76. 3 rounds of SIGKILL + restart
#   77. No stale flock after SIGKILL
#   78. Header file-length after partial write
#   79. Concurrent read during SIGKILL

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
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

# Verify binary exists
if [[ ! -x "$NBS_CHAT" ]]; then
    echo "ERROR: Binary not found: $NBS_CHAT"
    echo "Run: make -C src/nbs-chat install"
    exit 1
fi

echo "=== T15: Concurrent Chat Under SIGKILL ==="
echo "Test dir: $TEST_DIR"
echo ""

# Helper: verify chat file integrity
# Returns 0 if file is consistent, 1 if corrupt
verify_chat_integrity() {
    local chat_file="$1"

    # 1. File must exist and be non-empty
    [[ -f "$chat_file" && -s "$chat_file" ]] || return 1

    # 2. Header file-length must match actual file size
    local header_length actual_length
    header_length=$(grep '^file-length:' "$chat_file" | sed 's/^file-length:[[:space:]]*//')
    actual_length=$(wc -c < "$chat_file")
    [[ "$header_length" -eq "$actual_length" ]] || return 1

    # 3. Every base64 line after --- must decode successfully
    local raw_messages corrupt=0
    raw_messages=$(sed -n '/^---$/,$ p' "$chat_file" | tail -n +2)
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        if ! echo "$line" | base64 -d >/dev/null 2>&1; then
            corrupt=$((corrupt + 1))
        fi
    done <<< "$raw_messages"
    [[ "$corrupt" -eq 0 ]] || return 1

    # 4. Every decoded message must match "handle: content" format
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        local decoded
        decoded=$(echo "$line" | base64 -d 2>/dev/null) || return 1
        if ! echo "$decoded" | grep -qE '^[a-zA-Z0-9_-]+: '; then
            return 1
        fi
    done <<< "$raw_messages"

    return 0
}

# --- Test 73: 4 writers, kill 2 mid-write with SIGKILL ---
echo "73. 4 writers, SIGKILL 2 mid-write..."
CHAT="$TEST_DIR/sigkill.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Each writer sends 50 messages with padding to extend write window
WRITERS=("agent-a" "agent-b" "agent-c" "agent-d")
PIDS=()
for writer in "${WRITERS[@]}"; do
    (
        for i in $(seq 1 50); do
            "$NBS_CHAT" send "$CHAT" "$writer" "Message $i from $writer — payload padding to increase write duration"
        done
    ) &
    PIDS+=($!)
done

# Wait briefly to let writes begin, then SIGKILL two writers
sleep 0.2
kill -9 "${PIDS[1]}" 2>/dev/null || true  # agent-b
kill -9 "${PIDS[2]}" 2>/dev/null || true  # agent-c

# Wait for survivors to finish, then reap killed processes
wait "${PIDS[0]}" 2>/dev/null || true
wait "${PIDS[3]}" 2>/dev/null || true
wait "${PIDS[1]}" 2>/dev/null || true
wait "${PIDS[2]}" 2>/dev/null || true

# Verify file integrity
check "Chat file integrity after SIGKILL" "$( verify_chat_integrity "$CHAT" && echo pass || echo fail )"

# Verify surviving agents wrote all their messages
OUTPUT=$("$NBS_CHAT" read "$CHAT")
AGENT_A_COUNT=$(echo "$OUTPUT" | grep -c '^agent-a:' || true)
AGENT_D_COUNT=$(echo "$OUTPUT" | grep -c '^agent-d:' || true)
check "Surviving agent-a wrote all 50" "$( [[ "$AGENT_A_COUNT" -eq 50 ]] && echo pass || echo fail )"
check "Surviving agent-d wrote all 50" "$( [[ "$AGENT_D_COUNT" -eq 50 ]] && echo pass || echo fail )"

# Killed agents may have written 0..50 messages — whatever landed must be valid
AGENT_B_COUNT=$(echo "$OUTPUT" | grep -c '^agent-b:' || true)
AGENT_C_COUNT=$(echo "$OUTPUT" | grep -c '^agent-c:' || true)
check "Killed agent-b has 0-50 valid messages" "$( [[ "$AGENT_B_COUNT" -ge 0 && "$AGENT_B_COUNT" -le 50 ]] && echo pass || echo fail )"
check "Killed agent-c has 0-50 valid messages" "$( [[ "$AGENT_C_COUNT" -ge 0 && "$AGENT_C_COUNT" -le 50 ]] && echo pass || echo fail )"
echo ""

# --- Test 74: Cursor isolation after SIGKILL ---
echo "74. Surviving agent cursors unaffected by SIGKILL..."
# --since for surviving agent must work correctly
SINCE_A=$("$NBS_CHAT" read "$CHAT" --since=agent-a)
SINCE_A_RC=$?
check "--since=agent-a works after SIGKILL" "$( [[ "$SINCE_A_RC" -eq 0 ]] && echo pass || echo fail )"

# --unread for a fresh agent should return all messages
UNREAD_FRESH=$("$NBS_CHAT" read "$CHAT" --unread=fresh-observer)
UNREAD_FRESH_COUNT=$(echo "$UNREAD_FRESH" | grep -c '.' || true)
TOTAL_MSGS=$((AGENT_A_COUNT + AGENT_B_COUNT + AGENT_C_COUNT + AGENT_D_COUNT))
check "Fresh observer sees all $TOTAL_MSGS messages" "$( [[ "$UNREAD_FRESH_COUNT" -eq "$TOTAL_MSGS" ]] && echo pass || echo fail )"
echo ""

# --- Test 75: Restarted agent reads full history after SIGKILL ---
echo "75. Restarted agent reads full history..."
RESTART_OUTPUT=$("$NBS_CHAT" read "$CHAT")
RESTART_COUNT=$(echo "$RESTART_OUTPUT" | grep -c '.' || true)
check "Restarted agent reads all $TOTAL_MSGS messages" "$( [[ "$RESTART_COUNT" -eq "$TOTAL_MSGS" ]] && echo pass || echo fail )"

# Restarted agent posts — should work normally
"$NBS_CHAT" send "$CHAT" "agent-b" "Back online after SIGKILL"
RESTART_RC=$?
check "Restarted agent-b can post" "$( [[ "$RESTART_RC" -eq 0 ]] && echo pass || echo fail )"

# Verify integrity still holds after restart + new message
check "Integrity preserved after restarted agent posts" "$( verify_chat_integrity "$CHAT" && echo pass || echo fail )"
echo ""

# --- Test 76: Repeated SIGKILL cycles ---
echo "76. 3 rounds of SIGKILL + restart..."
CHAT2="$TEST_DIR/repeated_sigkill.chat"
"$NBS_CHAT" create "$CHAT2" >/dev/null

CYCLE_OK=true
for round in 1 2 3; do
    (
        for i in $(seq 1 20); do
            "$NBS_CHAT" send "$CHAT2" "survivor" "Round $round msg $i"
        done
    ) &
    SURVIVOR_PID=$!

    (
        for i in $(seq 1 20); do
            "$NBS_CHAT" send "$CHAT2" "victim" "Round $round msg $i"
        done
    ) &
    VICTIM_PID=$!

    sleep 0.1
    kill -9 "$VICTIM_PID" 2>/dev/null || true

    wait "$SURVIVOR_PID" 2>/dev/null || true
    wait "$VICTIM_PID" 2>/dev/null || true

    if ! verify_chat_integrity "$CHAT2"; then
        CYCLE_OK=false
        break
    fi
done
check "Integrity holds after 3 SIGKILL cycles" "$( $CYCLE_OK && echo pass || echo fail )"

# All survivor messages must be present (20 per round = 60 total)
OUTPUT2=$("$NBS_CHAT" read "$CHAT2")
SURVIVOR_COUNT=$(echo "$OUTPUT2" | grep -c '^survivor:' || true)
check "All 60 survivor messages present" "$( [[ "$SURVIVOR_COUNT" -eq 60 ]] && echo pass || echo fail )"

# Per-round monotonic ordering for survivor
ROUND_ORDER_OK=true
for round in 1 2 3; do
    PREV=0
    while IFS= read -r line; do
        NUM=$(echo "$line" | sed -n "s/.*Round $round msg \\([0-9]*\\).*/\\1/p")
        if [[ -n "$NUM" && "$NUM" -le "$PREV" ]]; then
            ROUND_ORDER_OK=false
            break 2
        fi
        PREV=$NUM
    done < <(echo "$OUTPUT2" | grep "^survivor: Round $round")
done
check "Survivor per-round ordering monotonic" "$( $ROUND_ORDER_OK && echo pass || echo fail )"
echo ""

# --- Test 77: SIGKILL does not leave stale flock ---
echo "77. No stale flock after SIGKILL..."
CHAT3="$TEST_DIR/stale_lock.chat"
"$NBS_CHAT" create "$CHAT3" >/dev/null

(
    for i in $(seq 1 100); do
        "$NBS_CHAT" send "$CHAT3" "doomed" "Message $i"
    done
) &
DOOMED_PID=$!

sleep 0.05
kill -9 "$DOOMED_PID" 2>/dev/null || true
wait "$DOOMED_PID" 2>/dev/null || true

# If flock is stale, this send will hang. Timeout after 5s.
set +e
timeout 5 "$NBS_CHAT" send "$CHAT3" "alive" "Post-kill write" >/dev/null 2>&1
STALE_RC=$?
set -e
check "Post-SIGKILL write succeeds (no stale lock)" "$( [[ "$STALE_RC" -eq 0 ]] && echo pass || echo fail )"

STALE_OUTPUT=$("$NBS_CHAT" read "$CHAT3")
check "Post-SIGKILL message readable" "$( echo "$STALE_OUTPUT" | grep -qF 'alive: Post-kill write' && echo pass || echo fail )"
echo ""

# --- Test 78: Header file-length after partial write ---
echo "78. Header consistency after partial write from SIGKILL..."
CHAT4="$TEST_DIR/partial_write.chat"
"$NBS_CHAT" create "$CHAT4" >/dev/null

# Seed with known messages
for i in $(seq 1 10); do
    "$NBS_CHAT" send "$CHAT4" "seed" "Baseline message $i"
done

# Writer that will be killed mid-stream
(
    for i in $(seq 1 100); do
        "$NBS_CHAT" send "$CHAT4" "partial" "Extra message $i with padding to increase write size"
    done
) &
PARTIAL_PID=$!

sleep 0.05
kill -9 "$PARTIAL_PID" 2>/dev/null || true
wait "$PARTIAL_PID" 2>/dev/null || true

# Verify header matches actual length
POST_HEADER=$(grep '^file-length:' "$CHAT4" | sed 's/^file-length:[[:space:]]*//')
POST_ACTUAL=$(wc -c < "$CHAT4")
check "Header file-length matches actual after SIGKILL" "$( [[ "$POST_HEADER" -eq "$POST_ACTUAL" ]] && echo pass || echo fail )"

check "Full integrity check after partial write" "$( verify_chat_integrity "$CHAT4" && echo pass || echo fail )"

# All seed messages must survive
POST_SEED=$("$NBS_CHAT" read "$CHAT4" | grep -c '^seed:' || true)
check "All 10 seed messages survive SIGKILL" "$( [[ "$POST_SEED" -eq 10 ]] && echo pass || echo fail )"
echo ""

# --- Test 79: Concurrent read during SIGKILL does not crash reader ---
echo "79. Read during SIGKILL does not crash..."
CHAT5="$TEST_DIR/read_during_kill.chat"
"$NBS_CHAT" create "$CHAT5" >/dev/null

for i in $(seq 1 20); do
    "$NBS_CHAT" send "$CHAT5" "setup" "Pre-existing message $i"
done

(
    for i in $(seq 1 100); do
        "$NBS_CHAT" send "$CHAT5" "writer" "Concurrent message $i"
    done
) &
WRITER_PID=$!

# Reader runs concurrently — must never get an error or corrupt output
READ_CRASH=0
for _ in $(seq 1 30); do
    set +e
    READ_OUT=$("$NBS_CHAT" read "$CHAT5" 2>&1)
    READ_RC=$?
    set -e
    if [[ "$READ_RC" -ne 0 ]]; then
        READ_CRASH=$((READ_CRASH + 1))
    fi
    if [[ -n "$READ_OUT" ]]; then
        BAD=$(echo "$READ_OUT" | grep -cvE '^[a-zA-Z0-9_-]+: ' || true)
        if [[ "$BAD" -gt 0 ]]; then
            READ_CRASH=$((READ_CRASH + 1))
        fi
    fi
done

kill -9 "$WRITER_PID" 2>/dev/null || true
wait "$WRITER_PID" 2>/dev/null || true

check "No read errors during concurrent SIGKILL" "$( [[ "$READ_CRASH" -eq 0 ]] && echo pass || echo fail )"
check "Final integrity after read-during-SIGKILL" "$( verify_chat_integrity "$CHAT5" && echo pass || echo fail )"
echo ""

# --- Summary ---
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All SIGKILL chat tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
