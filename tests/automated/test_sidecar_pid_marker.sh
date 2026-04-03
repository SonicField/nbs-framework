#!/bin/bash
# test_sidecar_pid_marker.sh — PID marker test for duplicate sidecar detection
#
# Scenario #5 from cursor-desync-mitigations.md:
#   "The sidecar should write a PID marker alongside the cursor.
#    Before advancing, check that the PID marker matches the current
#    sidecar's PID. If it doesn't, another sidecar is active —
#    log a warning and exit."
#
# Tests:
#   1. Sidecar writes PID marker file on startup
#   2. Second sidecar with same handle detects PID mismatch and exits
#   3. PID marker file contains the sidecar's actual PID
#   4. After first sidecar dies, second sidecar can start (stale PID)
#
# This test MUST FAIL on pre-harden codebase (PID marker not implemented).
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
    TEST_DIR=$(mktemp -d /tmp/nbs-pid-marker-test-XXXXXX)
    mkdir -p "$TEST_DIR/.nbs/chat" "$TEST_DIR/.nbs/events/processed" \
             "$TEST_DIR/.nbs/pids" "$TEST_DIR/.nbs/sessions"

    # Create chat file
    "$NBS_CHAT" create "$TEST_DIR/.nbs/chat/live.chat" 2>/dev/null
    "$NBS_CHAT" send "$TEST_DIR/.nbs/chat/live.chat" setup "test message" 2>/dev/null

    # Create control registry for the test handle
    echo "chat:$TEST_DIR/.nbs/chat/live.chat" > "$TEST_DIR/.nbs/control-registry-testagent"
    echo "bus:$TEST_DIR/.nbs/events" >> "$TEST_DIR/.nbs/control-registry-testagent"

    # Create bus config
    cat > "$TEST_DIR/.nbs/events/config.yaml" <<'YAML'
dedup-window: 300
ack-timeout: 120
YAML

    # Create a mock claude binary that prints a prompt
    cat > "$TEST_DIR/claude" <<'MOCK'
#!/bin/bash
echo ""
while true; do
    echo -n "❯ "
    if ! read -r input; then break; fi
    sleep 1
done
MOCK
    chmod +x "$TEST_DIR/claude"
}

cleanup() {
    # Kill any sidecars we started
    for pid in "${PIDS_TO_KILL[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    # Belt and braces
    pkill -f "nbs-sidecar.*$TEST_DIR" 2>/dev/null || true
    # Kill any nbs-ts sessions from test
    "$NBS_TS" list 2>/dev/null | while IFS=$'\t' read -r handle status name cmd; do
        [[ -n "$handle" ]] || continue
        if echo "$cmd" | grep -qF "$TEST_DIR" 2>/dev/null; then
            "$NBS_TS" kill "$handle" 2>/dev/null || true
        fi
    done
    [[ -n "$TEST_DIR" ]] && rm -rf "$TEST_DIR"
}
trap cleanup EXIT

# Start an nbs-ts session with a long-running process for the sidecar to attach to
start_ts_session() {
    local name="$1"
    local handle
    # Use a bash loop that ignores HUP and stays alive.
    # The -- separator is consumed by nbs-ts, not by bash.
    handle=$("$NBS_TS" create --name="$name" -- /bin/bash -c 'trap "" HUP; while true; do sleep 1; done' 2>/dev/null)
    if [[ -n "$handle" ]]; then
        # Wait for session to stabilise
        sleep 1
    fi
    echo "$handle"
}

# Start a sidecar in background, return its PID
start_sidecar() {
    local handle="$1"
    local ts_session="$2"
    local logfile="$3"

    "$NBS_SIDECAR" \
        --handle="$handle" \
        --root="$TEST_DIR" \
        --session="$ts_session" \
        --log="$logfile" \
        >"$logfile.stdout" 2>"$logfile.stderr" &
    local pid=$!
    PIDS_TO_KILL+=("$pid")
    echo "$pid"
}

echo "=== test_sidecar_pid_marker — duplicate sidecar detection ==="
echo "  Test dir: (temp)"
echo ""

# ============================================================
# Test 1: Sidecar writes PID marker file on startup
#
# After starting a sidecar, a PID marker file should exist at
# .nbs/pids/sidecar-<handle>.pid containing the sidecar's PID.
# ============================================================
echo "--- Test 1: Sidecar writes PID marker on startup ---"
setup

TS_HANDLE=$(start_ts_session "pid-marker-t1")
if [[ -z "$TS_HANDLE" ]]; then
    fail "could not create nbs-ts session"
else
    SIDECAR_PID=$(start_sidecar "testagent" "$TS_HANDLE" "$TEST_DIR/sidecar1.log")

    # Give sidecar time to start and write PID marker
    sleep 3

    PID_FILE="$TEST_DIR/.nbs/pids/sidecar-testagent.pid"
    if [[ -f "$PID_FILE" ]]; then
        MARKER_PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
        if [[ "$MARKER_PID" == "$SIDECAR_PID" ]]; then
            pass "PID marker written with correct PID ($SIDECAR_PID)"
        else
            fail "PID marker has wrong PID (marker=$MARKER_PID, actual=$SIDECAR_PID)"
        fi
    else
        fail "PID marker file not created at $PID_FILE"
    fi

    kill "$SIDECAR_PID" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 2: Second sidecar with same handle detects PID mismatch
#          and exits
#
# Start sidecar A → it writes PID marker.
# Start sidecar B with same handle → it reads PID marker,
# sees mismatch (A's PID != B's PID), and exits non-zero.
# ============================================================
echo "--- Test 2: Duplicate sidecar exits on PID mismatch ---"
setup

TS_HANDLE_A=$(start_ts_session "pid-marker-t2a")
TS_HANDLE_B=$(start_ts_session "pid-marker-t2b")

if [[ -z "$TS_HANDLE_A" || -z "$TS_HANDLE_B" ]]; then
    fail "could not create nbs-ts sessions"
else
    # Start first sidecar
    SIDECAR_A_PID=$(start_sidecar "testagent" "$TS_HANDLE_A" "$TEST_DIR/sidecar-a.log")
    sleep 3

    # Verify first sidecar is running
    if kill -0 "$SIDECAR_A_PID" 2>/dev/null; then
        pass "sidecar A is running (PID $SIDECAR_A_PID)"
    else
        fail "sidecar A died unexpectedly"
    fi

    # Start second sidecar with SAME handle (different session)
    SIDECAR_B_PID=$(start_sidecar "testagent" "$TS_HANDLE_B" "$TEST_DIR/sidecar-b.log")

    # Give sidecar B time to detect mismatch and exit
    sleep 5

    # Sidecar B should have exited (PID mismatch detection)
    if kill -0 "$SIDECAR_B_PID" 2>/dev/null; then
        fail "sidecar B is still running — should have exited on PID mismatch"
        kill "$SIDECAR_B_PID" 2>/dev/null || true
    else
        # Verify it exited, not crashed
        wait "$SIDECAR_B_PID" 2>/dev/null
        B_EXIT=$?
        if [[ $B_EXIT -ne 0 ]]; then
            pass "sidecar B exited (code $B_EXIT) — duplicate detected"
        else
            fail "sidecar B exited 0 — should exit non-zero on PID mismatch"
        fi
    fi

    # Sidecar A should still be running (it was first)
    if kill -0 "$SIDECAR_A_PID" 2>/dev/null; then
        pass "sidecar A still running after B exited"
    else
        fail "sidecar A died — only the duplicate should exit"
    fi

    # Check sidecar B's log for the warning message (check stderr, debug log, and stdout)
    FOUND_WARNING=0
    for logfile in "$TEST_DIR/sidecar-b.log.stderr" "$TEST_DIR/sidecar-b.log.stdout" \
                   /tmp/nbs-sidecar-main-debug-${SIDECAR_B_PID}.log; do
        if [[ -f "$logfile" ]] && grep -qi 'duplicate\|pid.*mismatch\|already running\|another sidecar' \
                "$logfile" 2>/dev/null; then
            FOUND_WARNING=1
            break
        fi
    done
    if [[ $FOUND_WARNING -eq 1 ]]; then
        pass "sidecar B logged duplicate detection warning"
    else
        fail "sidecar B log has no duplicate detection message"
    fi

    kill "$SIDECAR_A_PID" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE_A" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE_B" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 3: After first sidecar dies, second can start (stale PID)
#
# Start sidecar A → writes PID marker → kill A.
# Start sidecar B with same handle → reads PID marker,
# sees A's PID is dead (stale), overwrites with own PID, runs.
# ============================================================
echo "--- Test 3: Stale PID marker allows new sidecar ---"
setup

TS_HANDLE_A=$(start_ts_session "pid-marker-t3a")
TS_HANDLE_B=$(start_ts_session "pid-marker-t3b")

if [[ -z "$TS_HANDLE_A" || -z "$TS_HANDLE_B" ]]; then
    fail "could not create nbs-ts sessions"
else
    # Start and kill first sidecar
    SIDECAR_A_PID=$(start_sidecar "testagent" "$TS_HANDLE_A" "$TEST_DIR/sidecar-a.log")
    sleep 3
    kill "$SIDECAR_A_PID" 2>/dev/null || true
    wait "$SIDECAR_A_PID" 2>/dev/null || true
    sleep 1

    # PID marker should exist but be stale
    PID_FILE="$TEST_DIR/.nbs/pids/sidecar-testagent.pid"
    if [[ -f "$PID_FILE" ]]; then
        pass "PID marker file exists after sidecar A died"
    else
        fail "PID marker file missing — sidecar A didn't write it"
    fi

    # Start second sidecar — should detect stale PID and start normally
    SIDECAR_B_PID=$(start_sidecar "testagent" "$TS_HANDLE_B" "$TEST_DIR/sidecar-b.log")
    sleep 3

    if kill -0 "$SIDECAR_B_PID" 2>/dev/null; then
        pass "sidecar B started despite stale PID marker"

        # Verify PID marker was updated to B's PID
        if [[ -f "$PID_FILE" ]]; then
            MARKER_PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
            if [[ "$MARKER_PID" == "$SIDECAR_B_PID" ]]; then
                pass "PID marker updated to sidecar B's PID ($SIDECAR_B_PID)"
            else
                fail "PID marker not updated (marker=$MARKER_PID, expected=$SIDECAR_B_PID)"
            fi
        fi
    else
        fail "sidecar B failed to start — stale PID blocked it"
    fi

    kill "$SIDECAR_B_PID" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE_A" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE_B" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 4: Handle mismatch — different handle's PID is not a conflict
#
# Sidecar for handle "foo" writes PID marker. We manually write
# foo's PID into bar's PID marker file. Sidecar for handle "bar"
# starts — it should detect that the PID belongs to a different
# handle (via /proc/pid/cmdline) and treat it as stale, NOT as
# a conflict. Sidecar B should start and take ownership.
#
# This tests the PID recycling mitigation: kill(pid,0) says alive,
# but cmdline shows it's a different handle → not our conflict.
# ============================================================
echo "--- Test 4: Handle mismatch — different handle is not a conflict ---"
setup

# We need sidecar A running with handle "otheragent"
echo "chat:$TEST_DIR/.nbs/chat/live.chat" > "$TEST_DIR/.nbs/control-registry-otheragent"
echo "bus:$TEST_DIR/.nbs/events" >> "$TEST_DIR/.nbs/control-registry-otheragent"

TS_HANDLE_A=$(start_ts_session "pid-marker-t4a")
TS_HANDLE_B=$(start_ts_session "pid-marker-t4b")

if [[ -z "$TS_HANDLE_A" || -z "$TS_HANDLE_B" ]]; then
    fail "could not create nbs-ts sessions"
else
    # Start sidecar A with handle "otheragent"
    SIDECAR_A_PID=$(start_sidecar "otheragent" "$TS_HANDLE_A" "$TEST_DIR/sidecar-a.log")
    sleep 3

    if ! kill -0 "$SIDECAR_A_PID" 2>/dev/null; then
        fail "sidecar A (otheragent) died on startup"
    else
        pass "sidecar A (otheragent) running (PID $SIDECAR_A_PID)"

        # Write A's PID into testagent's PID marker file (simulates PID reuse)
        echo "$SIDECAR_A_PID" > "$TEST_DIR/.nbs/pids/sidecar-testagent.pid"

        # Start sidecar B with handle "testagent" — should NOT conflict
        # because A's cmdline has --handle=otheragent, not --handle=testagent
        SIDECAR_B_PID=$(start_sidecar "testagent" "$TS_HANDLE_B" "$TEST_DIR/sidecar-b.log")
        sleep 5

        if kill -0 "$SIDECAR_B_PID" 2>/dev/null; then
            pass "sidecar B (testagent) started — handle mismatch treated as stale"

            # Verify PID marker updated to B's PID
            PID_FILE="$TEST_DIR/.nbs/pids/sidecar-testagent.pid"
            if [[ -f "$PID_FILE" ]]; then
                MARKER_PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
                if [[ "$MARKER_PID" == "$SIDECAR_B_PID" ]]; then
                    pass "PID marker updated to sidecar B's PID"
                else
                    fail "PID marker not updated (marker=$MARKER_PID, expected=$SIDECAR_B_PID)"
                fi
            fi
        else
            fail "sidecar B (testagent) refused to start — handle mismatch incorrectly treated as conflict"
        fi
    fi

    kill "$SIDECAR_A_PID" 2>/dev/null || true
    kill "$SIDECAR_B_PID" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE_A" 2>/dev/null || true
    "$NBS_TS" kill "$TS_HANDLE_B" 2>/dev/null || true
fi

cleanup
PIDS_TO_KILL=()
echo ""

# ============================================================
# Test 5: Dead process with bogus PID — treat as stale
#
# When a PID marker contains a PID that is dead (kill returns
# ESRCH), the sidecar should treat it as stale and take ownership.
#
# Simulate by writing a bogus PID (99999999) to the marker file.
# This PID is dead, so kill(pid,0) returns ESRCH → sidecar takes
# the "Process is dead (stale PID)" branch and starts normally.
#
# NOTE: This does NOT exercise the "alive but cmdline unreadable"
# branch (sidecar.c:746-750), which requires kernel-level
# conditions (hidepid, security modules, or TOCTOU race) that
# cannot be reliably simulated in userspace without root.
# That path is defensive and handles the same way (treat as stale).
# ============================================================
echo "--- Test 5: Dead process (bogus PID) — treat as stale ---"
setup

TS_HANDLE=$(start_ts_session "pid-marker-t5")

if [[ -z "$TS_HANDLE" ]]; then
    fail "could not create nbs-ts session"
else
    # Write a bogus PID to the marker file
    PID_FILE="$TEST_DIR/.nbs/pids/sidecar-testagent.pid"
    echo "99999999" > "$PID_FILE"

    # Start sidecar — should treat bogus PID as stale and start
    SIDECAR_PID=$(start_sidecar "testagent" "$TS_HANDLE" "$TEST_DIR/sidecar.log")
    sleep 3

    if kill -0 "$SIDECAR_PID" 2>/dev/null; then
        pass "sidecar started despite bogus dead PID in marker"

        # Verify PID marker updated
        MARKER_PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
        if [[ "$MARKER_PID" == "$SIDECAR_PID" ]]; then
            pass "PID marker updated to sidecar's PID"
        else
            fail "PID marker not updated (marker=$MARKER_PID, expected=$SIDECAR_PID)"
        fi
    else
        fail "sidecar refused to start — dead PID treated as conflict"
    fi

    kill "$SIDECAR_PID" 2>/dev/null || true
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
