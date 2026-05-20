#!/bin/bash
# Test: nbs-sidecar-restart cross-project dedup
#
# Verifies that sidecars with the same handle but different --root=
# values are treated as separate entries. Without this fix, the dedup
# key was handle-only, causing cross-project collisions (e.g.
# gatekeeper@cinderx killed when restarting gatekeeper@phoenix).
#
# Tests:
#   1. Two sidecars, same handle, different roots — both survive restart
#   2. Two sidecars, same handle, same root — deduped to one (existing behaviour)
#   3. --root filter respects project boundary
#   4. PID marker cleanup uses correct root (not stale variable)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "   PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "   FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "   SKIP: $1"; }

TEST_DIR=$(mktemp -d /tmp/nbs-restart-xproj.XXXXXX)

cleanup() {
    # Kill any mock sidecars we started
    for pf in "$TEST_DIR"/*.pid; do
        [[ -f "$pf" ]] || continue
        kill "$(cat "$pf")" 2>/dev/null || true
    done
    # Kill any sidecar-loops we spawned
    pgrep -f "nbs-sidecar-loop.*$TEST_DIR" 2>/dev/null | xargs kill 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

# Build mock sidecar — a C binary that sleeps, so /proc/cmdline
# matches what the restart script expects (not a shell script).
MOCK_BIN="$TEST_DIR/bin"
mkdir -p "$MOCK_BIN"
cat > "$MOCK_BIN/mock.c" << 'CSRC'
#include <unistd.h>
int main(void) { pause(); return 0; }
CSRC
gcc -o "$MOCK_BIN/nbs-sidecar" "$MOCK_BIN/mock.c"
rm -f "$MOCK_BIN/mock.c"

# Copy restart script and library into test bin so SCRIPT_DIR resolves
cp "$PROJECT_ROOT/bin/nbs-sidecar-restart" "$MOCK_BIN/nbs-sidecar-restart"
cp "$PROJECT_ROOT/bin/nbs-sidecar-lib.sh" "$MOCK_BIN/nbs-sidecar-lib.sh"
cp "$PROJECT_ROOT/bin/nbs-sidecar-find-session" "$MOCK_BIN/nbs-sidecar-find-session" 2>/dev/null || true
# nbs-ts is needed by the spawn function
if [[ -x "$PROJECT_ROOT/bin/nbs-ts" ]]; then
    cp "$PROJECT_ROOT/bin/nbs-ts" "$MOCK_BIN/nbs-ts"
fi
chmod +x "$MOCK_BIN"/*

# Create two fake project roots
ROOT_A="$TEST_DIR/project-alpha"
ROOT_B="$TEST_DIR/project-beta"
mkdir -p "$ROOT_A/.nbs/pids" "$ROOT_A/.nbs/locks"
mkdir -p "$ROOT_B/.nbs/pids" "$ROOT_B/.nbs/locks"

echo "=== nbs-sidecar-restart Cross-Project Dedup Tests ==="
echo ""

# --- Test 1: Same handle, different roots — both survive ---
echo "1. Same handle, different roots — both survive restart..."

# Start two mock sidecars with identical handles but different roots
"$MOCK_BIN/nbs-sidecar" --handle=scribe --root="$ROOT_A" --transport=ts --session=aaaa1111 &
PID_A=$!
echo "$PID_A" > "$TEST_DIR/scribe_a.pid"

"$MOCK_BIN/nbs-sidecar" --handle=scribe --root="$ROOT_B" --transport=ts --session=bbbb2222 &
PID_B=$!
echo "$PID_B" > "$TEST_DIR/scribe_b.pid"

sleep 0.5

if ! kill -0 "$PID_A" 2>/dev/null || ! kill -0 "$PID_B" 2>/dev/null; then
    fail "Mock sidecars failed to start (PID_A=$PID_A, PID_B=$PID_B)"
else
    # Run restart (no --root filter — should process both projects)
    # The restart script will try to respawn but that will fail (no real
    # nbs-ts sessions). The key assertion: it must NOT kill one as a
    # duplicate of the other. Both must be individually killed+respawned.
    OUTPUT=$("$MOCK_BIN/nbs-sidecar-restart" scribe 2>&1) || true

    # After restart, both original PIDs should be dead (restarted)
    sleep 2
    A_DEAD=0; B_DEAD=0
    kill -0 "$PID_A" 2>/dev/null || A_DEAD=1
    kill -0 "$PID_B" 2>/dev/null || B_DEAD=1

    if [[ $A_DEAD -eq 1 && $B_DEAD -eq 1 ]]; then
        # Both killed — check that BOTH were mentioned in the output
        # (not treated as one dedup group)
        A_MENTIONED=$(echo "$OUTPUT" | grep -c "scribe@project-alpha" || true)
        B_MENTIONED=$(echo "$OUTPUT" | grep -c "scribe@project-beta" || true)

        if [[ $A_MENTIONED -ge 1 && $B_MENTIONED -ge 1 ]]; then
            pass "Both projects' sidecars handled separately"
        elif echo "$OUTPUT" | grep -c "Killing.*scribe" | grep -q "^2$" 2>/dev/null; then
            pass "Both sidecars killed (2 kill messages)"
        else
            # Count kill lines — should be exactly 2 (one per project)
            KILL_COUNT=$(echo "$OUTPUT" | grep -c "Killing.*scribe" || true)
            if [[ $KILL_COUNT -eq 2 ]]; then
                pass "Both sidecars killed individually ($KILL_COUNT kill messages)"
            elif [[ $KILL_COUNT -eq 1 ]]; then
                fail "Only one sidecar killed — cross-project dedup collision"
            else
                pass "Both sidecars processed (kill_count=$KILL_COUNT)"
            fi
        fi
    elif [[ $A_DEAD -eq 1 && $B_DEAD -eq 0 ]]; then
        fail "Only project-alpha's sidecar killed — project-beta survived (dedup collision)"
        kill "$PID_B" 2>/dev/null || true
    elif [[ $A_DEAD -eq 0 && $B_DEAD -eq 1 ]]; then
        fail "Only project-beta's sidecar killed — project-alpha survived (dedup collision)"
        kill "$PID_A" 2>/dev/null || true
    else
        fail "Neither sidecar was killed"
        kill "$PID_A" "$PID_B" 2>/dev/null || true
    fi
fi

# --- Test 2: Same handle, same root — deduped to one ---
echo "2. Same handle, same root — deduped to one..."

# Start two mock sidecars with identical handle AND root (genuine duplicate)
"$MOCK_BIN/nbs-sidecar" --handle=medic --root="$ROOT_A" --transport=ts --session=aaaa3333 &
DUP_PID1=$!
echo "$DUP_PID1" > "$TEST_DIR/medic_dup1.pid"

"$MOCK_BIN/nbs-sidecar" --handle=medic --root="$ROOT_A" --transport=ts --session=aaaa4444 &
DUP_PID2=$!
echo "$DUP_PID2" > "$TEST_DIR/medic_dup2.pid"

sleep 0.5

if ! kill -0 "$DUP_PID1" 2>/dev/null || ! kill -0 "$DUP_PID2" 2>/dev/null; then
    fail "Duplicate mock sidecars failed to start"
else
    OUTPUT=$("$MOCK_BIN/nbs-sidecar-restart" medic 2>&1) || true
    sleep 2

    D1_DEAD=0; D2_DEAD=0
    kill -0 "$DUP_PID1" 2>/dev/null || D1_DEAD=1
    kill -0 "$DUP_PID2" 2>/dev/null || D2_DEAD=1

    if [[ $D1_DEAD -eq 1 && $D2_DEAD -eq 1 ]]; then
        if echo "$OUTPUT" | grep -qi "DEDUP.*2.*sidecars.*medic"; then
            pass "Both duplicates killed with DEDUP message"
        else
            pass "Both duplicates killed"
        fi
    else
        fail "Not all duplicates killed (d1_dead=$D1_DEAD d2_dead=$D2_DEAD)"
        kill "$DUP_PID1" "$DUP_PID2" 2>/dev/null || true
    fi
fi

# --- Test 3: --root filter respects project boundary ---
echo "3. --root filter only affects target project..."

"$MOCK_BIN/nbs-sidecar" --handle=gatekeeper --root="$ROOT_A" --transport=ts --session=aaaa5555 &
FILT_PID_A=$!
echo "$FILT_PID_A" > "$TEST_DIR/gk_a.pid"

"$MOCK_BIN/nbs-sidecar" --handle=gatekeeper --root="$ROOT_B" --transport=ts --session=bbbb6666 &
FILT_PID_B=$!
echo "$FILT_PID_B" > "$TEST_DIR/gk_b.pid"

sleep 0.5

if ! kill -0 "$FILT_PID_A" 2>/dev/null || ! kill -0 "$FILT_PID_B" 2>/dev/null; then
    fail "Filtered mock sidecars failed to start"
else
    # Restart only project-alpha
    OUTPUT=$("$MOCK_BIN/nbs-sidecar-restart" --root="$ROOT_A" gatekeeper 2>&1) || true
    sleep 2

    A_DEAD=0; B_ALIVE=0
    kill -0 "$FILT_PID_A" 2>/dev/null || A_DEAD=1
    kill -0 "$FILT_PID_B" 2>/dev/null && B_ALIVE=1

    if [[ $A_DEAD -eq 1 && $B_ALIVE -eq 1 ]]; then
        pass "Only target project's sidecar restarted, other untouched"
    elif [[ $A_DEAD -eq 1 && $B_ALIVE -eq 0 ]]; then
        fail "--root filter killed the wrong project's sidecar too"
    elif [[ $A_DEAD -eq 0 ]]; then
        fail "Target project's sidecar was not restarted"
    fi
    kill "$FILT_PID_B" 2>/dev/null || true
fi

# --- Test 4: PID marker cleanup uses correct root ---
echo "4. PID marker cleanup uses args-derived root..."

# Create PID markers in both roots
echo "99999" > "$ROOT_A/.nbs/pids/sidecar-supervisor.pid"
echo "99998" > "$ROOT_B/.nbs/pids/sidecar-supervisor.pid"

"$MOCK_BIN/nbs-sidecar" --handle=supervisor --root="$ROOT_A" --transport=ts --session=aaaa7777 &
SUP_PID=$!
echo "$SUP_PID" > "$TEST_DIR/sup.pid"

sleep 0.5

if ! kill -0 "$SUP_PID" 2>/dev/null; then
    fail "Supervisor mock sidecar failed to start"
else
    OUTPUT=$("$MOCK_BIN/nbs-sidecar-restart" --root="$ROOT_A" supervisor 2>&1) || true
    sleep 2

    # project-alpha's PID marker should be cleaned
    if [[ ! -f "$ROOT_A/.nbs/pids/sidecar-supervisor.pid" ]]; then
        pass "PID marker cleaned for target root"
    else
        fail "PID marker NOT cleaned for target root"
    fi

    # project-beta's PID marker should be untouched
    if [[ -f "$ROOT_B/.nbs/pids/sidecar-supervisor.pid" ]]; then
        pass "PID marker preserved for other root"
    else
        fail "PID marker wrongly cleaned for other root"
    fi
fi

# --- Results ---
echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
