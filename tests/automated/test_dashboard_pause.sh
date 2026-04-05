#!/bin/bash
# Test: dashboard pause state — status bar reflects control-pause file
#
# Verifies that the dashboard status bar shows "Paused: yes" when the
# control-pause file exists and "Paused: no" when it does not.
# Also tests message count display in the status bar.
#
# Spec: feature-requests/dashboard.md (authoritative)
# Step 2: test_dashboard_pause.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_TS_RENDER="$PROJECT_ROOT/bin/nbs-ts-render"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"
DASHBOARD="$PROJECT_ROOT/bin/nbs-dashboard"

# Prerequisites — SKIP if testing infrastructure is missing
[ -x "$NBS_TS" ] || { echo "SKIP: nbs-ts not found"; exit 0; }
[ -x "$NBS_TS_RENDER" ] || { echo "SKIP: nbs-ts-render not found"; exit 0; }
[ -x "$NBS_CHAT" ] || { echo "SKIP: nbs-chat not found"; exit 0; }

# Binary under test — FAIL if missing (proves falsifiability)
[ -x "$DASHBOARD" ] || { echo "FAIL: nbs-dashboard not found at $DASHBOARD"; exit 1; }

HANDLES=()
SIDECAR_PIDS=()
ERRORS=0

TMPDIR=$(mktemp -d)
TAG="dashpause$$"

# Environment: test-specific naming per D-1775314242
export NBS_DASHBOARD_SESSION_PREFIX="dashtest"
export NBS_DASHBOARD_SIDECAR_CMD="dashtest-sidecar"

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    for pid in "${SIDECAR_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

raw_send() {
    local handle="$1"; shift
    printf "$@" > ~/.nbs-ts/sessions/$handle/input.fifo
}

echo "=== Dashboard Pause State Test ==="
echo ""

# ---------------------------------------------------------------
# Mock infrastructure
# ---------------------------------------------------------------

MOCK_ROOT="$TMPDIR/project"
MOCK_NBS="$MOCK_ROOT/.nbs"
mkdir -p "$MOCK_NBS/chat" "$MOCK_NBS/events"
echo "$TAG" > "$MOCK_NBS/project-id"

CHAT="$MOCK_NBS/chat/team.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
"$NBS_CHAT" send "$CHAT" supervisor "Init."
"$NBS_CHAT" send "$CHAT" generalist "Ready."
"$NBS_CHAT" send "$CHAT" testkeeper "Tests queued."

for agent in supervisor generalist gatekeeper theologian testkeeper scribe medic; do
    "$NBS_CHAT" cursor-set "$CHAT" "$agent" 3 2>/dev/null || true
done

AGENTS="supervisor generalist gatekeeper theologian testkeeper scribe medic"
for agent in $AGENTS; do
    H=$("$NBS_TS" create --name="dashtest-${agent}-${TAG}" "sleep 3600" | tr -d '[:space:]')
    HANDLES+=("$H")
done

for agent in $AGENTS; do
    (exec -a "dashtest-sidecar --handle=$agent --root=$MOCK_ROOT" sleep 3600) &
    SIDECAR_PIDS+=($!)
done

sleep 1

# ---------------------------------------------------------------
# P1. Status bar shows "Paused: no" when control-pause absent
# ---------------------------------------------------------------
echo "P1. Paused: no when control-pause absent..."

# Ensure no pause file exists
rm -f "$MOCK_NBS/control-pause"

DASH_HANDLE=$("$NBS_TS" create --name="dashtest-pause-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_HANDLE")
sleep 3

OUTPUT=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$OUTPUT" | grep -qiE "Paused.*no|Paused: no"; then
    pass "Status bar shows 'Paused: no'"
else
    if echo "$OUTPUT" | grep -qi "Paused"; then
        fail "Paused text found but not 'no'"
        echo "   $(echo "$OUTPUT" | grep -i "Paused" | head -1)"
    else
        fail "No 'Paused' text in status bar"
        echo "   Last 3 lines: $(echo "$OUTPUT" | tail -3)"
    fi
fi

# ---------------------------------------------------------------
# P2. Status bar shows "Paused: yes" when control-pause exists
# ---------------------------------------------------------------
echo "P2. Paused: yes when control-pause exists..."

# Create the pause file
touch "$MOCK_NBS/control-pause"
sleep 3  # Wait for next refresh cycle (2s interval)

OUTPUT_PAUSED=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$OUTPUT_PAUSED" | grep -qiE "Paused.*yes|Paused: yes"; then
    pass "Status bar shows 'Paused: yes'"
else
    if echo "$OUTPUT_PAUSED" | grep -qi "Paused"; then
        fail "Paused text found but not 'yes'"
        echo "   $(echo "$OUTPUT_PAUSED" | grep -i "Paused" | head -1)"
    else
        fail "No 'Paused' text in status bar after creating control-pause"
    fi
fi

# ---------------------------------------------------------------
# P3. Removing control-pause updates to "Paused: no" on refresh
# ---------------------------------------------------------------
echo "P3. Removing control-pause updates to no..."

rm -f "$MOCK_NBS/control-pause"
sleep 3  # Wait for refresh

OUTPUT_UNPAUSED=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$OUTPUT_UNPAUSED" | grep -qiE "Paused.*no|Paused: no"; then
    pass "Status bar updated to 'Paused: no' after removing file"
else
    fail "Status bar did not update after removing control-pause"
fi

# ---------------------------------------------------------------
# P4. Messages count shown in status bar
# ---------------------------------------------------------------
echo "P4. Messages count in status bar..."

# We sent 3 messages. The status bar should show the count.
if echo "$OUTPUT" | grep -qiE "Messages|Msgs"; then
    pass "Messages count label present in status bar"
else
    fail "Messages count not found in status bar"
    echo "   Last 3 lines: $(echo "$OUTPUT" | tail -3)"
fi

# Verify the actual number appears
if echo "$OUTPUT" | grep -q "3"; then
    pass "Message count value visible"
else
    fail "Message count value '3' not visible"
fi

# ---------------------------------------------------------------
# P5. Dashboard process remains alive through state changes
# ---------------------------------------------------------------
echo "P5. Dashboard alive through pause/unpause cycle..."

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Dashboard still alive after pause/unpause cycle"
else
    fail "Dashboard died during pause/unpause cycle"
fi

# ---------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------
raw_send "$DASH_HANDLE" "q"
"$NBS_TS" wait-complete "$DASH_HANDLE" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# Results
# ---------------------------------------------------------------
echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All pause state tests passed"
    exit 0
else
    echo "$ERRORS pause state test(s) failed"
    exit 1
fi
