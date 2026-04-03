#!/bin/bash
# test_sidecar_notify_gaps.sh — Tests for sidecar notification gaps (Root Cause B)
#
# Scenarios from cursor-desync-mitigations.md:
#   Scenario #3: Cooldown suppresses burst notifications
#   Scenario #6: Sidecar restart without cursor reset
#
# Tests:
#   1. Burst test: 10 messages in 2 seconds → agent eventually sees all 10
#   2. Restart test: sidecar restarts at cursor == msg_count → startup notification
#   3. Post-cooldown re-check: after cooldown expires, unreads trigger notification
#
# These tests MUST FAIL on pre-harden codebase (post-cooldown re-check not implemented).
#
# Requires: nbs-ts, nbs-chat, nbs-sidecar

set -uo pipefail

source "$(dirname "$0")/test_helpers.sh"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NBS_SIDECAR="$PROJECT_ROOT/bin/nbs-sidecar"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "   PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "   FAIL: $1"; }
skip_test() { SKIP=$((SKIP + 1)); echo "   SKIP: $1 ($2)"; }

assert_eq() {
    local label="$1" got="$2" expected="$3"
    if [[ "$got" == "$expected" ]]; then pass "$label"
    else fail "$label (got '$got', expected '$expected')"; fi
}

assert_ge() {
    local label="$1" got="$2" limit="$3"
    if [[ "$got" -ge "$limit" ]]; then pass "$label"
    else fail "$label (got $got, expected >= $limit)"; fi
}

assert_gt() {
    local label="$1" got="$2" limit="$3"
    if [[ "$got" -gt "$limit" ]]; then pass "$label"
    else fail "$label (got $got, expected > $limit)"; fi
}

# Verify required binaries
for bin in "$NBS_SIDECAR" "$NBS_TS" "$NBS_CHAT"; do
    if [[ ! -x "$bin" ]]; then
        echo "SKIP: $(basename "$bin") not found — run 'make install' first" >&2
        exit 0
    fi
done

TEST_DIR=""
PIDS_TO_KILL=()

setup() {
    TEST_DIR=$(mktemp -d /tmp/nbs-notify-gap-test-XXXXXX)
    mkdir -p "$TEST_DIR/.nbs/chat" "$TEST_DIR/.nbs/events/processed" \
             "$TEST_DIR/.nbs/pids" "$TEST_DIR/.nbs/sessions"

    # Create chat file
    "$NBS_CHAT" create "$TEST_DIR/.nbs/chat/live.chat" 2>/dev/null

    # Create control registry
    echo "chat:$TEST_DIR/.nbs/chat/live.chat" > "$TEST_DIR/.nbs/control-registry-testagent"
    echo "bus:$TEST_DIR/.nbs/events" >> "$TEST_DIR/.nbs/control-registry-testagent"

    # Bus config
    cat > "$TEST_DIR/.nbs/events/config.yaml" <<'YAML'
dedup-window: 300
ack-timeout: 120
YAML
}

cleanup() {
    for pid in "${PIDS_TO_KILL[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    pkill -f "nbs-sidecar.*$TEST_DIR" 2>/dev/null || true
    # Kill nbs-ts sessions from test
    if [[ -x "$NBS_TS" ]]; then
        "$NBS_TS" list 2>/dev/null | while IFS=$'\t' read -r handle status name cmd; do
            [[ -n "$handle" ]] || continue
            if [[ -n "${TEST_DIR:-}" ]] && echo "$cmd" | grep -qF "$TEST_DIR" 2>/dev/null; then
                "$NBS_TS" kill "$handle" 2>/dev/null || true
            fi
        done
    fi
    [[ -n "$TEST_DIR" ]] && rm -rf "$TEST_DIR"
}
trap cleanup EXIT

start_ts_session() {
    local name="$1"
    local handle
    # Mock session outputs ❯ prompt so sidecar can detect prompt and inject.
    handle=$("$NBS_TS" create --name="$name" -- /bin/bash -c 'trap "" HUP; echo ""; while true; do printf "❯ "; sleep 3; done' 2>/dev/null)
    if [[ -n "$handle" ]]; then sleep 2; fi
    echo "$handle"
}

start_sidecar() {
    local handle="$1"
    local ts_session="$2"
    local logfile="$3"
    local extra_env="${4:-}"

    eval $extra_env \
        "$NBS_SIDECAR" \
        --handle="$handle" \
        --root="$TEST_DIR" \
        --session="$ts_session" \
        >"$logfile.stdout" 2>"$logfile.stderr" &
    local pid=$!
    PIDS_TO_KILL+=("$pid")
    echo "$pid"
}

# Count notifications injected into the nbs-ts session.
# Notifications contain "[NBS-CHAT-NOTIFICATION]" in the session output.
count_notifications() {
    local ts_handle="$1"
    local count
    count=$("$NBS_TS" read "$ts_handle" 2>/dev/null | grep -c '\[NBS-CHAT-NOTIFICATION\]' || true)
    echo "${count:-0}"
}

echo "=== test_sidecar_notify_gaps — Root Cause B ==="
echo ""

# ============================================================
# Test 1: Burst test — 10 messages in 2 seconds
#
# Scenario #3: During a burst, the sidecar delivers one notification,
# then cooldown suppresses the rest. After cooldown expires,
# the sidecar MUST re-check and deliver a catch-up notification
# so the agent sees ALL 10 messages.
#
# Setup:
#   - Start sidecar with short cooldown (5s) and no startup grace
#   - Send 1 message, wait for notification
#   - Send 9 more messages rapidly (burst)
#   - Wait for cooldown + bus_check_interval to elapse
#   - Verify agent received ≥2 notifications (initial + catch-up)
#   - Verify cursor caught up to all 10 messages
# ============================================================
echo "--- Scenario #3: Burst notification catch-up ---"
setup

CHAT="$TEST_DIR/.nbs/chat/live.chat"
TS_HANDLE=$(start_ts_session "burst-test")

if [[ -z "$TS_HANDLE" ]]; then
    fail "could not create nbs-ts session"
else
    # Start sidecar with short cooldown and no startup grace
    SC_PID=$(start_sidecar "testagent" "$TS_HANDLE" "$TEST_DIR/sc.log" \
        "NBS_NOTIFY_COOLDOWN=5 NBS_STARTUP_GRACE=2 NBS_BUS_CHECK_INTERVAL=2")
    sleep 2

    if ! kill -0 "$SC_PID" 2>/dev/null; then
        fail "sidecar died on startup"
    else
        # Send first message — should trigger notification
        "$NBS_CHAT" send "$CHAT" alice "Hello team" 2>/dev/null

        # Wait for startup_grace (2s) + bus_check (2s) + margin
        sleep 6

        # Verify first notification delivered
        NOTIFS_AFTER_FIRST=$(count_notifications "$TS_HANDLE")
        assert_ge "first message triggers notification" "$NOTIFS_AFTER_FIRST" 1

        # Burst: send 9 more messages rapidly
        for i in $(seq 2 10); do
            "$NBS_CHAT" send "$CHAT" "agent${i}" "Burst message ${i}" 2>/dev/null
        done

        # Wait for cooldown (5s) + bus_check cycles for catch-up delivery + margin
        # Note: catchup_needed flag bypasses the content-stability gate entirely,
        # so delivery happens on the next bus_check after cooldown expires.
        sleep 18

        # After cooldown expires, sidecar re-checks cursor vs msg_count.
        # With catchup_needed=1, it bypasses the content gate and delivers
        # a catch-up notification for the burst messages.
        NOTIFS_AFTER_BURST=$(count_notifications "$TS_HANDLE")
        assert_ge "catch-up notification after burst (got ${NOTIFS_AFTER_BURST})" \
            "$NOTIFS_AFTER_BURST" 2

        # Verify total message count is correct
        TOTAL_MSGS=$("$NBS_CHAT" count "$CHAT" 2>/dev/null || echo 0)
        assert_eq "total messages = 10" "$TOTAL_MSGS" "10"

        kill "$SC_PID" 2>/dev/null || true
    fi

    "$NBS_TS" kill "$TS_HANDLE" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 2: Sidecar restart — startup notification when caught up
#
# Scenario #6: Old sidecar dies. New sidecar starts. Cursor equals
# msg_count (agent has read everything). The new sidecar MUST still
# deliver a startup notification so the agent reads recent context
# and knows she has a new sidecar.
#
# Setup:
#   - Create chat with 5 messages
#   - Set cursor to msg_count-1 (caught up)
#   - Start sidecar with no startup grace
#   - Verify sidecar delivers startup notification within 10s
# ============================================================
echo "--- Scenario #6: Startup notification when caught up ---"
setup

CHAT="$TEST_DIR/.nbs/chat/live.chat"

# Create messages
for i in $(seq 1 5); do
    "$NBS_CHAT" send "$CHAT" "agent${i}" "History message ${i}" 2>/dev/null
done

# Set cursor to caught-up position
MSG_COUNT=$("$NBS_CHAT" count "$CHAT" 2>/dev/null || echo 0)
CURSOR_VAL=$((MSG_COUNT - 1))
"$NBS_CHAT" cursor-set "$CHAT" testagent "$CURSOR_VAL" 2>/dev/null

TS_HANDLE=$(start_ts_session "restart-test")

if [[ -z "$TS_HANDLE" ]]; then
    fail "could not create nbs-ts session"
else
    # Start sidecar — should deliver startup notification even though caught up
    SC_PID=$(start_sidecar "testagent" "$TS_HANDLE" "$TEST_DIR/sc.log" \
        "NBS_NOTIFY_COOLDOWN=5 NBS_STARTUP_GRACE=2 NBS_BUS_CHECK_INTERVAL=2")

    # Wait for startup notification
    sleep 8

    if ! kill -0 "$SC_PID" 2>/dev/null; then
        fail "sidecar died on startup"
    else
        NOTIFS=$(count_notifications "$TS_HANDLE")
        assert_ge "startup notification delivered when caught up (got ${NOTIFS})" \
            "$NOTIFS" 1

        # Check that the notification mentions startup or context
        SESSION_OUTPUT=$("$NBS_TS" read "$TS_HANDLE" 2>/dev/null || true)
        if echo "$SESSION_OUTPUT" | grep -qi 'NBS-CHAT-NOTIFICATION\|startup\|unread'; then
            pass "startup notification contains expected content"
        else
            fail "no startup notification found in session output"
        fi

        kill "$SC_PID" 2>/dev/null || true
    fi

    "$NBS_TS" kill "$TS_HANDLE" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 3: Post-cooldown re-check delivers catch-up
#
# Direct test of the mitigation: after cooldown expires,
# the sidecar must check cursor vs msg_count and notify
# if the agent is behind — even without new messages arriving.
#
# Setup:
#   - Start sidecar
#   - Send 5 messages rapidly (triggers 1 notification + cooldown)
#   - Do NOT send any more messages
#   - Wait for cooldown to expire
#   - Verify sidecar delivered a second notification (catch-up)
#     without any new messages arriving
# ============================================================
echo "--- Scenario #3: Post-cooldown re-check without new messages ---"
setup

CHAT="$TEST_DIR/.nbs/chat/live.chat"
TS_HANDLE=$(start_ts_session "recheck-test")

if [[ -z "$TS_HANDLE" ]]; then
    fail "could not create nbs-ts session"
else
    SC_PID=$(start_sidecar "testagent" "$TS_HANDLE" "$TEST_DIR/sc.log" \
        "NBS_NOTIFY_COOLDOWN=5 NBS_STARTUP_GRACE=2 NBS_BUS_CHECK_INTERVAL=2")
    sleep 2

    if ! kill -0 "$SC_PID" 2>/dev/null; then
        fail "sidecar died on startup"
    else
        # Send 5 messages rapidly
        for i in $(seq 1 5); do
            "$NBS_CHAT" send "$CHAT" "agent${i}" "Rapid message ${i}" 2>/dev/null
        done

        # Wait for startup_grace (2s) + bus_check (2s) + margin
        sleep 6
        NOTIFS_FIRST=$(count_notifications "$TS_HANDLE")
        assert_ge "first notification delivered" "$NOTIFS_FIRST" 1

        # Do NOT send any more messages.
        # Wait for cooldown (5s) + bus_check cycles for catch-up delivery + margin
        # Note: catchup_needed flag bypasses the content-stability gate entirely,
        # so delivery happens on the next bus_check after cooldown expires.
        sleep 18

        # Sidecar should have re-checked after cooldown and found
        # the agent is still behind (cursor not advanced because
        # we're not reading the messages in the mock session).
        NOTIFS_RECHECK=$(count_notifications "$TS_HANDLE")
        assert_ge "post-cooldown re-check notification (got ${NOTIFS_RECHECK})" \
            "$NOTIFS_RECHECK" 2

        kill "$SC_PID" 2>/dev/null || true
    fi

    "$NBS_TS" kill "$TS_HANDLE" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 4: Cursor ownership falsifier — --unread advance must not
#          prevent sidecar notification on next message
#
# Root Cause D falsifier (from cursors.md): The dual-writer model
# allows both agent (via --unread) and sidecar to advance cursors.
# After agent advances cursor to N via --unread, a new message N+1
# MUST still trigger a sidecar notification. Agent cursor advancement
# does NOT suppress future notifications.
#
# Setup:
#   - Start sidecar
#   - Send 3 messages, wait for notification
#   - Agent reads --unread (advances cursor to msg_count-1 = N)
#   - Send 1 new message (N+1)
#   - Verify sidecar delivers notification for the new message
# ============================================================
echo "--- Root Cause D: --unread advance does not suppress sidecar notification ---"
setup

CHAT="$TEST_DIR/.nbs/chat/live.chat"
TS_HANDLE=$(start_ts_session "cursor-ownership-test")

if [[ -z "$TS_HANDLE" ]]; then
    fail "could not create nbs-ts session"
else
    # Start sidecar with short cooldown and no startup grace
    SC_PID=$(start_sidecar "testagent" "$TS_HANDLE" "$TEST_DIR/sc.log" \
        "NBS_NOTIFY_COOLDOWN=5 NBS_STARTUP_GRACE=2 NBS_BUS_CHECK_INTERVAL=2")
    sleep 2

    if ! kill -0 "$SC_PID" 2>/dev/null; then
        fail "sidecar died on startup"
    else
        # Send 3 initial messages
        "$NBS_CHAT" send "$CHAT" alice "Setup message 1" 2>/dev/null
        "$NBS_CHAT" send "$CHAT" bob "Setup message 2" 2>/dev/null
        "$NBS_CHAT" send "$CHAT" carol "Setup message 3" 2>/dev/null

        # Wait for startup_grace (2s) + bus_check (2s) + margin
        sleep 6

        NOTIFS_BEFORE=$(count_notifications "$TS_HANDLE")
        assert_ge "initial notification delivered" "$NOTIFS_BEFORE" 1

        # Agent reads --unread, advancing cursor to N (msg_count-1)
        "$NBS_CHAT" read "$CHAT" --unread=testagent >/dev/null 2>&1

        # Verify cursor was advanced
        MSG_COUNT=$("$NBS_CHAT" count "$CHAT" 2>/dev/null || echo 0)
        CURSOR_FILE="${CHAT}.cursors"
        CURSOR_VAL=$(grep "^testagent=" "$CURSOR_FILE" 2>/dev/null | head -1 | cut -d= -f2)
        EXPECTED_CURSOR=$((MSG_COUNT - 1))
        assert_eq "cursor advanced to msg_count-1 after --unread (${CURSOR_VAL})" \
            "$CURSOR_VAL" "$EXPECTED_CURSOR"

        # Wait for cooldown (5s) + bus_check cycle (2s) + margin to fully expire
        sleep 8

        # Send a NEW message (N+1) — sidecar MUST notify
        "$NBS_CHAT" send "$CHAT" dave "New message after --unread advance" 2>/dev/null

        # Wait for bus_check cycles to pick up the new message + deliver
        sleep 10

        NOTIFS_AFTER=$(count_notifications "$TS_HANDLE")
        assert_gt "--unread advance did not suppress notification (before=${NOTIFS_BEFORE}, after=${NOTIFS_AFTER})" \
            "$NOTIFS_AFTER" "$NOTIFS_BEFORE"

        kill "$SC_PID" 2>/dev/null || true
    fi

    "$NBS_TS" kill "$TS_HANDLE" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Summary
# ============================================================

TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped (${TOTAL} total) ==="

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
