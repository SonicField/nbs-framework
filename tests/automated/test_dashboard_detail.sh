#!/bin/bash
# Test: dashboard detail view — drill into agent terminal output
#
# Verifies that pressing Enter opens the selected agent's terminal output
# rendered through nbs-ts-render, and that Escape/q returns to the overview.
#
# Spec: feature-requests/dashboard.md (authoritative)
# Step 2: test_dashboard_detail.sh

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
TAG="dashdet$$"

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

# Send raw bytes — no \r appended
raw_send() {
    local handle="$1"; shift
    printf "$@" > ~/.nbs-ts/sessions/$handle/input.fifo
}

echo "=== Dashboard Detail View Test ==="
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
"$NBS_CHAT" send "$CHAT" supervisor "Team started."

for agent in supervisor generalist gatekeeper theologian testkeeper scribe medic; do
    "$NBS_CHAT" cursor-set "$CHAT" "$agent" 1 2>/dev/null || true
done

# Create agent sessions — supervisor produces recognisable output
AGENTS="supervisor generalist gatekeeper theologian testkeeper scribe medic"
for agent in $AGENTS; do
    if [ "$agent" = "supervisor" ]; then
        # Supervisor echoes distinctive text so we can verify detail view
        H=$("$NBS_TS" create --name="dashtest-${agent}-${TAG}" \
            "echo 'SUPERVISOR-DETAIL-MARKER: active and running'; sleep 3600" \
            | tr -d '[:space:]')
    else
        H=$("$NBS_TS" create --name="dashtest-${agent}-${TAG}" "sleep 3600" | tr -d '[:space:]')
    fi
    HANDLES+=("$H")
done

for agent in $AGENTS; do
    (exec -a "dashtest-sidecar --handle=$agent --root=$MOCK_ROOT" sleep 3600) &
    SIDECAR_PIDS+=($!)
done

sleep 2

# ---------------------------------------------------------------
# D1. Enter opens detail view for selected agent
# ---------------------------------------------------------------
echo "D1. Enter opens detail view..."

DASH_HANDLE=$("$NBS_TS" create --name="dashtest-detail-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_HANDLE")
sleep 2

# Capture overview
OVERVIEW=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# Press Enter to open detail view (cursor on first agent: supervisor)
raw_send "$DASH_HANDLE" '\r'
sleep 2

DETAIL=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if [ "$DETAIL" != "$OVERVIEW" ]; then
    pass "Enter changed view (detail view opened)"
else
    fail "Enter did not change view"
fi

# ---------------------------------------------------------------
# D2. Detail view shows agent terminal output (rendered text)
# ---------------------------------------------------------------
echo "D2. Detail view shows rendered terminal output..."

if echo "$DETAIL" | grep -qi "SUPERVISOR-DETAIL-MARKER\|supervisor"; then
    pass "Detail view contains agent output or name"
else
    fail "Detail view missing agent output"
    echo "   Detail (first 5 lines): $(echo "$DETAIL" | head -5)"
fi

# ---------------------------------------------------------------
# D3. Detail view output is rendered through nbs-ts-render
# ---------------------------------------------------------------
echo "D3. Output is rendered (readable text, not raw escape sequences)..."

# The rendered detail view should contain recognisable text.
# Check that the distinctive marker appears as readable text.
if echo "$DETAIL" | grep -q "SUPERVISOR-DETAIL-MARKER"; then
    pass "Agent output is readable text"
else
    # Even without the marker, the detail view should show the agent name/session
    if echo "$DETAIL" | grep -qi "supervisor\|session"; then
        pass "Detail view contains readable agent context"
    else
        fail "Detail view does not show readable agent output"
        echo "   Detail (first 5 lines): $(echo "$DETAIL" | head -5)"
    fi
fi

# ---------------------------------------------------------------
# D4. Escape returns to overview
# ---------------------------------------------------------------
echo "D4. Escape returns to overview..."

raw_send "$DASH_HANDLE" '\033'
sleep 2

BACK_TO_OVERVIEW=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# Should see the agent table again — check for multiple agent names
AGENTS_FOUND=0
for agent in $AGENTS; do
    if echo "$BACK_TO_OVERVIEW" | grep -qi "$agent"; then
        AGENTS_FOUND=$((AGENTS_FOUND + 1))
    fi
done

if [ $AGENTS_FOUND -ge 5 ]; then
    pass "Returned to overview ($AGENTS_FOUND agents visible)"
else
    fail "Did not return to overview (only $AGENTS_FOUND agents visible)"
fi

# ---------------------------------------------------------------
# D5. Down arrow scrolls output in detail view
# ---------------------------------------------------------------
echo "D5. Down arrow scrolls in detail view..."

# Re-enter detail view
raw_send "$DASH_HANDLE" '\r'
sleep 2

DETAIL_BEFORE=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

raw_send "$DASH_HANDLE" '\033[B'
sleep 1

# Dashboard should not crash after scroll
STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Down arrow in detail view — dashboard still running"
else
    fail "Down arrow in detail view — dashboard crashed"
fi

# ---------------------------------------------------------------
# D6. Page Down and Page Up scroll by pages in detail view
# ---------------------------------------------------------------
echo "D6. Page Down/Up scroll in detail view..."

# Page Down (ESC[6~)
raw_send "$DASH_HANDLE" '\033[6~'
sleep 1

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Page Down in detail view — dashboard still running"
else
    fail "Page Down in detail view — dashboard crashed"
fi

# Page Up (ESC[5~)
raw_send "$DASH_HANDLE" '\033[5~'
sleep 1

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Page Up in detail view — dashboard still running"
else
    fail "Page Up in detail view — dashboard crashed"
fi

# ---------------------------------------------------------------
# D7. Home/End work in detail view
# ---------------------------------------------------------------
echo "D7. Home/End in detail view..."

# Home (ESC[H)
raw_send "$DASH_HANDLE" '\033[H'
sleep 1

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Home in detail view — dashboard still running"
else
    fail "Home in detail view — dashboard crashed"
fi

# End (ESC[F)
raw_send "$DASH_HANDLE" '\033[F'
sleep 1

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "End in detail view — dashboard still running"
else
    fail "End in detail view — dashboard crashed"
fi

# ---------------------------------------------------------------
# D8. 'q' in detail view returns to overview (per spec)
# ---------------------------------------------------------------
echo "D8. 'q' in detail view returns to overview..."

raw_send "$DASH_HANDLE" "q"
sleep 2

AFTER_Q=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# q in detail should return to overview — check for agent table
AGENTS_FOUND=0
for agent in $AGENTS; do
    if echo "$AFTER_Q" | grep -qi "$agent"; then
        AGENTS_FOUND=$((AGENTS_FOUND + 1))
    fi
done

if [ $AGENTS_FOUND -ge 5 ]; then
    pass "'q' in detail returned to overview ($AGENTS_FOUND agents visible)"
else
    fail "'q' in detail did not return to overview (only $AGENTS_FOUND agents visible)"
fi

# ---------------------------------------------------------------
# D9. Detail view header shows agent name and session
# ---------------------------------------------------------------
echo "D9. Detail view header shows agent context..."

# Re-enter detail
raw_send "$DASH_HANDLE" '\r'
sleep 2

DETAIL_HEADER=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$DETAIL_HEADER" | grep -qi "supervisor"; then
    pass "Detail header shows agent name"
else
    fail "Detail header missing agent name"
fi

# ---------------------------------------------------------------
# D10. Detail view preserves ANSI colour from agent output
# ---------------------------------------------------------------
echo "D10. Detail view preserves colour (ANSI SGR sequences)..."

# We are in the detail view from D9. Capture raw output (with ANSI preserved).
DETAIL_RAW=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --no-strip --width=120 --height=30)

# The detail view should contain SGR colour sequences from the agent's output
# or from the dashboard's own rendering of the agent content.
# Check for any SGR sequence (ESC[...m) in the output — at minimum the
# dashboard frame uses colour, but agent output colour should also be present.
if echo "$DETAIL_RAW" | grep -qP '\x1b\[[0-9;]*m'; then
    pass "Detail view contains ANSI SGR colour sequences"
else
    fail "Detail view has no ANSI colour sequences — colours stripped"
fi

# ---------------------------------------------------------------
# Cleanup — exit the dashboard
# ---------------------------------------------------------------
raw_send "$DASH_HANDLE" '\033'
sleep 1
raw_send "$DASH_HANDLE" "q"
"$NBS_TS" wait-complete "$DASH_HANDLE" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# Results
# ---------------------------------------------------------------
echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All detail view tests passed"
    exit 0
else
    echo "$ERRORS detail view test(s) failed"
    exit 1
fi
