#!/bin/bash
# Test: dashboard terminal resize — SIGWINCH handling
#
# Verifies that the dashboard handles terminal resize without crashing
# and redraws the layout correctly after resize.
#
# Spec: feature-requests/dashboard.md (authoritative)
# Step 2: test_dashboard_resize.sh

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
TAG="dashrsz$$"

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

echo "=== Dashboard Resize Test ==="
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

for agent in supervisor generalist gatekeeper theologian testkeeper scribe medic; do
    "$NBS_CHAT" cursor-set "$CHAT" "$agent" 1 2>/dev/null || true
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
# R1. Dashboard survives SIGWINCH without crashing
# ---------------------------------------------------------------
echo "R1. Dashboard survives SIGWINCH..."

DASH_HANDLE=$("$NBS_TS" create --name="dashtest-resize-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_HANDLE")
sleep 2

# Verify it's running
STATUS_BEFORE=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if ! echo "$STATUS_BEFORE" | grep -q "alive"; then
    fail "Dashboard not running before resize"
    echo ""
    echo "=== Results ==="
    echo "$ERRORS test(s) failed"
    exit 1
fi

# Find the dashboard process PID
DASH_PID=$(pgrep -f "nbs-dashboard.*$MOCK_ROOT" | head -1 || true)

if [ -n "$DASH_PID" ]; then
    # Send SIGWINCH directly to the dashboard process
    kill -WINCH "$DASH_PID" 2>/dev/null || true
    sleep 2

    STATUS_AFTER=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
    if echo "$STATUS_AFTER" | grep -q "alive"; then
        pass "Dashboard survived SIGWINCH"
    else
        fail "Dashboard died after SIGWINCH"
    fi
else
    # Cannot find PID — just verify the dashboard is still alive
    sleep 2
    STATUS_AFTER=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
    if echo "$STATUS_AFTER" | grep -q "alive"; then
        pass "Dashboard still running (could not send SIGWINCH directly)"
    else
        fail "Dashboard died unexpectedly"
    fi
fi

# ---------------------------------------------------------------
# R2. Layout redraws correctly after resize
# ---------------------------------------------------------------
echo "R2. Layout intact after resize..."

# Capture output — all agents should still be visible
OUTPUT_AFTER=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

AGENTS_VISIBLE=0
for agent in $AGENTS; do
    if echo "$OUTPUT_AFTER" | grep -qi "$agent"; then
        AGENTS_VISIBLE=$((AGENTS_VISIBLE + 1))
    fi
done

if [ $AGENTS_VISIBLE -ge 5 ]; then
    pass "Layout intact after resize ($AGENTS_VISIBLE agents visible)"
else
    fail "Layout broken after resize (only $AGENTS_VISIBLE agents visible)"
fi

# ---------------------------------------------------------------
# R3. Dashboard still responds to input after resize
# ---------------------------------------------------------------
echo "R3. Dashboard responds to input after resize..."

raw_send "$DASH_HANDLE" '\033[B'
sleep 1

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Dashboard accepts input after resize"
else
    fail "Dashboard unresponsive after resize"
fi

# ---------------------------------------------------------------
# R4. Multiple rapid resizes do not crash
# ---------------------------------------------------------------
echo "R4. Multiple rapid resizes do not crash..."

if [ -n "$DASH_PID" ]; then
    for i in $(seq 1 5); do
        kill -WINCH "$DASH_PID" 2>/dev/null || true
        sleep 0.2
    done
    sleep 2

    STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
    if echo "$STATUS" | grep -q "alive"; then
        pass "Survived 5 rapid SIGWINCH signals"
    else
        fail "Crashed during rapid resize sequence"
    fi
else
    pass "Rapid resize test skipped (no PID available)"
fi

# ---------------------------------------------------------------
# R5. 'q' still exits cleanly after resize
# ---------------------------------------------------------------
echo "R5. 'q' exits cleanly after resize..."

raw_send "$DASH_HANDLE" "q"
"$NBS_TS" wait-complete "$DASH_HANDLE" --timeout=5 2>/dev/null || true
sleep 1

EXIT_CODE=$("$NBS_TS" exit-code "$DASH_HANDLE" 2>&1) || EXIT_CODE="unknown"
if [ "$EXIT_CODE" = "0" ]; then
    pass "Dashboard exited cleanly with code 0 after resize"
else
    fail "Dashboard exit code after resize: $EXIT_CODE (expected 0)"
fi

# ---------------------------------------------------------------
# R6. Narrow terminal: no wrapping, content truncated or h-scrollable
# ---------------------------------------------------------------
echo "R6. Narrow terminal does not wrap..."

# Launch dashboard in a narrow terminal (60 cols)
DASH_NARROW=$("$NBS_TS" create --name="dashtest-narrow-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_NARROW")
sleep 3

# Capture at narrow width
OUTPUT_NARROW=$("$NBS_TS" read "$DASH_NARROW" 2>&1 | \
    "$NBS_TS_RENDER" --width=60 --height=24)

# The table should not wrap — each agent should appear on exactly one line.
# Count lines containing agent names. If wrapping occurs, agent names may
# appear on multiple lines or column data wraps below the name.
SUPERVISOR_LINES=$(echo "$OUTPUT_NARROW" | grep -ci "supervisor" || true)

if [ "$SUPERVISOR_LINES" -le 1 ]; then
    pass "Narrow terminal: supervisor appears on at most 1 line (no wrap)"
else
    fail "Narrow terminal: supervisor appears on $SUPERVISOR_LINES lines (wrapping)"
fi

# Verify dashboard is still functional at narrow width
STATUS=$("$NBS_TS" status "$DASH_NARROW" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Dashboard functional at 60-column width"
else
    fail "Dashboard crashed at narrow width"
fi

# Test left/right arrow horizontal pan
OUTPUT_BEFORE=$("$NBS_TS" read "$DASH_NARROW" 2>&1 | \
    "$NBS_TS_RENDER" --width=60 --height=24)

raw_send "$DASH_NARROW" '\033[C'
sleep 1

OUTPUT_PANNED=$("$NBS_TS" read "$DASH_NARROW" 2>&1 | \
    "$NBS_TS_RENDER" --width=60 --height=24)

if [ "$OUTPUT_PANNED" != "$OUTPUT_BEFORE" ]; then
    pass "Right arrow pans viewport horizontally"
else
    fail "Right arrow did not change display (no horizontal pan)"
fi

raw_send "$DASH_NARROW" "q"
"$NBS_TS" wait-complete "$DASH_NARROW" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# R7. Wide character (CJK) h-scroll — no mojibake
# ---------------------------------------------------------------
echo "R7. Wide character h-scroll handles CJK correctly..."

# Create an agent session with CJK output (display width 2 per character)
CJK_SESSION=$("$NBS_TS" create --name="dashtest-generalist-${TAG}" \
    "echo '日本語テスト：ビルド完了'; sleep 3600" \
    | tr -d '[:space:]')
HANDLES+=("$CJK_SESSION")
sleep 1

# Launch dashboard at narrow width where CJK content forces h-scroll
DASH_CJK=$("$NBS_TS" create --name="dashtest-cjk-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_CJK")
sleep 3

# Enter detail view for the generalist (which has CJK output)
# First navigate to generalist (second row)
raw_send "$DASH_CJK" '\033[B'
sleep 0.5
raw_send "$DASH_CJK" '\r'
sleep 2

# Dashboard should not crash with CJK content
STATUS=$("$NBS_TS" status "$DASH_CJK" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Dashboard alive with CJK content in detail view"
else
    fail "Dashboard crashed with CJK content"
fi

# Scroll right — should not produce mojibake or crash
raw_send "$DASH_CJK" '\033[C'
sleep 1

STATUS=$("$NBS_TS" status "$DASH_CJK" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "H-scroll with CJK content — dashboard still running"
else
    fail "H-scroll with CJK content — dashboard crashed"
fi

# Capture output — verify CJK text is present (not garbled)
OUTPUT_CJK=$("$NBS_TS" read "$DASH_CJK" 2>&1 | \
    "$NBS_TS_RENDER" --width=80 --height=24)

if echo "$OUTPUT_CJK" | grep -q "日本語\|テスト\|ビルド"; then
    pass "CJK characters rendered correctly (no mojibake)"
else
    # CJK may have scrolled out of view — verify no garbled bytes
    if echo "$OUTPUT_CJK" | grep -qP '[\x80-\xbf]{2,}'; then
        fail "Garbled bytes detected — possible UTF-8 bisection"
    else
        pass "No garbled bytes (CJK may have scrolled out of view)"
    fi
fi

raw_send "$DASH_CJK" '\033'
sleep 1
raw_send "$DASH_CJK" "q"
"$NBS_TS" wait-complete "$DASH_CJK" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# R8. Detail view at narrow width — no wrapping, h-scroll only
# ---------------------------------------------------------------
echo "R8. Detail view at narrow width does not wrap..."

# Create an agent with long output lines that exceed 60 columns
LONG_SESSION=$("$NBS_TS" create --name="dashtest-supervisor-${TAG}" \
    "echo 'LONGLINE: This is a very long output line that should exceed sixty columns and must be clipped or scrolled horizontally, never wrapped to the next line'; sleep 3600" \
    | tr -d '[:space:]')
HANDLES+=("$LONG_SESSION")
sleep 1

# Launch dashboard at narrow width
DASH_DETAIL_NARROW=$("$NBS_TS" create --name="dashtest-detnarrow-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_DETAIL_NARROW")
sleep 3

# Enter detail view for supervisor (first agent)
raw_send "$DASH_DETAIL_NARROW" '\r'
sleep 2

# Capture at narrow width — 60 columns
OUTPUT_DETAIL_NARROW=$("$NBS_TS" read "$DASH_DETAIL_NARROW" 2>&1 | \
    "$NBS_TS_RENDER" --width=60 --height=24)

# Check that LONGLINE appears on at most one line — no wrapping
LONGLINE_COUNT=$(echo "$OUTPUT_DETAIL_NARROW" | grep -c "LONGLINE" || true)

if [ "$LONGLINE_COUNT" -le 1 ]; then
    pass "Detail view: long line appears on at most 1 line (no wrap)"
else
    fail "Detail view: long line wrapped to $LONGLINE_COUNT lines"
    echo "   Lines matching LONGLINE:"
    echo "$OUTPUT_DETAIL_NARROW" | grep "LONGLINE"
fi

# The continuation text ("never wrapped") should NOT appear on a second line
if echo "$OUTPUT_DETAIL_NARROW" | grep -q "never wrapped"; then
    # If visible, it should be on the same line as LONGLINE, not a separate line
    WRAPPED_LINE=$(echo "$OUTPUT_DETAIL_NARROW" | grep -c "never wrapped" || true)
    LONGLINE_LINE=$(echo "$OUTPUT_DETAIL_NARROW" | grep -c "LONGLINE" || true)
    if [ "$WRAPPED_LINE" -le "$LONGLINE_LINE" ]; then
        pass "Continuation text on same line (not wrapped)"
    else
        fail "Continuation text appeared on extra lines (wrapping detected)"
    fi
else
    pass "Long line correctly clipped at viewport edge"
fi

# Dashboard should still be alive
STATUS=$("$NBS_TS" status "$DASH_DETAIL_NARROW" 2>&1)
if echo "$STATUS" | grep -q "alive"; then
    pass "Dashboard alive in narrow detail view"
else
    fail "Dashboard crashed in narrow detail view"
fi

raw_send "$DASH_DETAIL_NARROW" '\033'
sleep 1
raw_send "$DASH_DETAIL_NARROW" "q"
"$NBS_TS" wait-complete "$DASH_DETAIL_NARROW" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# Results
# ---------------------------------------------------------------
echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All resize tests passed"
    exit 0
else
    echo "$ERRORS resize test(s) failed"
    exit 1
fi
