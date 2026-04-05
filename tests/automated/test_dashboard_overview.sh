#!/bin/bash
# Test: dashboard overview screen — agent table, status columns, navigation
#
# Creates a mock .nbs/ project with fake agent sessions and sidecars,
# launches the dashboard in a PTY via nbs-ts, and verifies the overview
# table renders correctly with all 7 agents displayed.
#
# Spec: feature-requests/dashboard.md (authoritative)
# Step 2: test_dashboard_overview.sh

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
TAG="dashtest$$"

# Environment: tell the dashboard to use test-specific naming patterns
# so mock sessions/sidecars cannot collide with real phoenix processes.
# Decision D-1775314242: all mock names use dashtest- prefix.
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

# Send raw bytes to the dashboard PTY — no \r appended (unlike nbs-ts send)
raw_send() {
    local handle="$1"; shift
    printf "$@" > ~/.nbs-ts/sessions/$handle/input.fifo
}

echo "=== Dashboard Overview Test ==="
echo ""

# ---------------------------------------------------------------
# Mock infrastructure
# ---------------------------------------------------------------

MOCK_ROOT="$TMPDIR/project"
MOCK_NBS="$MOCK_ROOT/.nbs"
mkdir -p "$MOCK_NBS/chat" "$MOCK_NBS/events"
echo "$TAG" > "$MOCK_NBS/project-id"

# Create an archive chat file with old messages — dashboard must skip this
ARCHIVE="$MOCK_NBS/chat/team-20260101-000000-archive.chat"
"$NBS_CHAT" create "$ARCHIVE" >/dev/null
"$NBS_CHAT" send "$ARCHIVE" supervisor "Old archive message from January"

# Create the active chat file — dashboard must select this one
CHAT="$MOCK_NBS/chat/team.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

AGENTS="supervisor generalist gatekeeper theologian testkeeper scribe medic"

# Populate chat — messages from each agent
for agent in $AGENTS; do
    "$NBS_CHAT" send "$CHAT" "$agent" "Message from $agent"
done
for i in $(seq 1 13); do
    "$NBS_CHAT" send "$CHAT" supervisor "Status update $i"
done
# Total: 20 messages

# Set cursors — all agents at 18 (2 behind)
for agent in $AGENTS; do
    "$NBS_CHAT" cursor-set "$CHAT" "$agent" 18 2>/dev/null || true
done

# Create fake nbs-ts sessions — dashtest- prefix per D-1775314242
for agent in $AGENTS; do
    H=$("$NBS_TS" create --name="dashtest-${agent}-${TAG}" "sleep 3600" | tr -d '[:space:]')
    HANDLES+=("$H")
done

# Create fake sidecar processes — dashtest-sidecar, NOT nbs-sidecar
# This ensures ghost processes (from crashed cleanup) cannot match real
# phoenix sidecar pgrep patterns.
for agent in $AGENTS; do
    (exec -a "dashtest-sidecar --handle=$agent --root=$MOCK_ROOT" sleep 3600) &
    SIDECAR_PIDS+=($!)
done

sleep 1

# ---------------------------------------------------------------
# O0. Archive skip: dashboard reads active chat, not archive
# ---------------------------------------------------------------
echo "O0. Dashboard skips archive chat files..."

# The mock directory has both team-*-archive.chat and team.chat.
# The dashboard must select team.chat (20 messages), not the archive (1 message).
# Verify by checking message count — should be 20, not 1.
DASH_ARCHIVE=$("$NBS_TS" create --name="dashtest-archive-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_ARCHIVE")
sleep 3

OUTPUT_ARCHIVE=$("$NBS_TS" read "$DASH_ARCHIVE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$OUTPUT_ARCHIVE" | grep -q "Messages.*20\|Messages: 20"; then
    pass "Dashboard reads active chat (20 messages), not archive"
elif echo "$OUTPUT_ARCHIVE" | grep -q "Messages.*1\|Messages: 1"; then
    fail "Dashboard read the archive file (1 message) instead of active chat"
else
    # Accept any count > 1 as evidence the active file was selected
    if echo "$OUTPUT_ARCHIVE" | grep -qE "Messages.*[0-9]"; then
        MSG_COUNT=$(echo "$OUTPUT_ARCHIVE" | grep -oE "Messages.*[0-9]+" | grep -oE "[0-9]+$")
        if [ "${MSG_COUNT:-0}" -gt 1 ]; then
            pass "Dashboard reads active chat ($MSG_COUNT messages)"
        else
            fail "Dashboard may have read archive (Messages: $MSG_COUNT)"
        fi
    else
        fail "Messages count not found in output"
    fi
fi

raw_send "$DASH_ARCHIVE" "q"
"$NBS_TS" wait-complete "$DASH_ARCHIVE" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# O1. Dashboard launches and displays overview
# ---------------------------------------------------------------
echo "O1. Dashboard launches and displays overview table..."

DASH_HANDLE=$("$NBS_TS" create --name="dashtest-overview-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_HANDLE")
sleep 2

OUTPUT=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$OUTPUT" | grep -qi "dashboard"; then
    pass "Dashboard title visible"
else
    fail "Dashboard title not visible"
    echo "   Output (first 3 lines): $(echo "$OUTPUT" | head -3)"
fi

# ---------------------------------------------------------------
# O2. All 7 agents displayed
# ---------------------------------------------------------------
echo "O2. All 7 agents displayed..."

ALL_FOUND=true
for agent in $AGENTS; do
    if ! echo "$OUTPUT" | grep -qi "$agent"; then
        fail "Agent '$agent' not visible"
        ALL_FOUND=false
    fi
done
if [ "$ALL_FOUND" = true ]; then
    pass "All 7 agent names present"
fi

# ---------------------------------------------------------------
# O3. Status column shows alive for running agents
# ---------------------------------------------------------------
echo "O3. Status column shows alive for running agents..."

if echo "$OUTPUT" | grep -qi "alive"; then
    pass "Status 'alive' visible"
else
    fail "Status 'alive' not visible"
fi

# ---------------------------------------------------------------
# O4. Sidecar column shows OK for agents with sidecars
# ---------------------------------------------------------------
echo "O4. Sidecar column shows OK..."

if echo "$OUTPUT" | grep -q "OK"; then
    pass "Sidecar 'OK' visible"
else
    fail "Sidecar 'OK' not visible"
fi

# ---------------------------------------------------------------
# O5. Cursor column shows cursor/behind format
# ---------------------------------------------------------------
echo "O5. Cursor column shows cursor values..."

# Expect N/N format (e.g. "18/2" for cursor=18, behind=2)
if echo "$OUTPUT" | grep -qE "[0-9]+/[0-9]+"; then
    pass "Cursor column shows cursor/behind format"
else
    fail "Cursor column missing N/N values"
fi

# ---------------------------------------------------------------
# O5b. Last Post column shows time deltas, not just hyphens
# ---------------------------------------------------------------
echo "O5b. Last Post column shows time deltas..."

# Agents who sent messages should show a time delta (e.g. "30s ago", "1m ago",
# "2m ago", "just now", or a numeric time). Not just "—" for everyone.
if echo "$OUTPUT" | grep -qiE "[0-9]+[sm] ago|[0-9]+s|just now|[0-9]+m"; then
    pass "Last Post column shows time delta values"
else
    fail "Last Post column shows no time deltas (likely all hyphens)"
    # Show the supervisor row as evidence
    echo "   supervisor row: $(echo "$OUTPUT" | grep -i supervisor | head -1)"
fi

# ---------------------------------------------------------------
# O6. Down arrow moves selection cursor
# ---------------------------------------------------------------
echo "O6. Down arrow moves selection cursor..."

OUTPUT_BEFORE=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

raw_send "$DASH_HANDLE" '\033[B'
sleep 1

OUTPUT_AFTER=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if [ "$OUTPUT_AFTER" != "$OUTPUT_BEFORE" ]; then
    pass "Down arrow changed display (cursor moved)"
else
    fail "Down arrow did not change display"
fi

# ---------------------------------------------------------------
# O7. Up arrow moves selection cursor back
# ---------------------------------------------------------------
echo "O7. Up arrow moves selection cursor..."

raw_send "$DASH_HANDLE" '\033[A'
sleep 1

OUTPUT_UP=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if [ "$OUTPUT_UP" != "$OUTPUT_AFTER" ]; then
    pass "Up arrow changed display (cursor moved back)"
else
    fail "Up arrow did not change display"
fi

# ---------------------------------------------------------------
# O8. Home key jumps to first agent
# ---------------------------------------------------------------
echo "O8. Home key jumps to first agent..."

# Move down several times to ensure we're not at the top
raw_send "$DASH_HANDLE" '\033[B'
sleep 0.3
raw_send "$DASH_HANDLE" '\033[B'
sleep 0.3
raw_send "$DASH_HANDLE" '\033[B'
sleep 0.5

# Press Home (ESC[H)
raw_send "$DASH_HANDLE" '\033[H'
sleep 1

OUTPUT_HOME=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# The selection indicator should be on the first agent (supervisor)
if echo "$OUTPUT_HOME" | grep -E "▸|►|>" | head -1 | grep -qi "supervisor"; then
    pass "Home jumped to first agent (supervisor)"
else
    # Fallback: verify supervisor is still visible and position changed
    if [ "$OUTPUT_HOME" != "$OUTPUT_AFTER" ] && echo "$OUTPUT_HOME" | grep -qi "supervisor"; then
        pass "Home key changed position — supervisor visible"
    else
        fail "Home did not jump to first agent"
    fi
fi

# ---------------------------------------------------------------
# O9. End key jumps to last agent
# ---------------------------------------------------------------
echo "O9. End key jumps to last agent..."

# Press End (ESC[F)
raw_send "$DASH_HANDLE" '\033[F'
sleep 1

OUTPUT_END=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

if echo "$OUTPUT_END" | grep -E "▸|►|>" | head -1 | grep -qi "medic"; then
    pass "End jumped to last agent (medic)"
else
    if [ "$OUTPUT_END" != "$OUTPUT_HOME" ] && echo "$OUTPUT_END" | grep -qi "medic"; then
        pass "End key changed position — medic visible"
    else
        fail "End did not jump to last agent"
    fi
fi

# ---------------------------------------------------------------
# O10. 'q' exits cleanly, terminal state restored
# ---------------------------------------------------------------
echo "O10. 'q' exits cleanly..."

raw_send "$DASH_HANDLE" "q"
"$NBS_TS" wait-complete "$DASH_HANDLE" --timeout=5 2>/dev/null || true
sleep 1

EXIT_CODE=$("$NBS_TS" exit-code "$DASH_HANDLE" 2>&1) || EXIT_CODE="unknown"
if [ "$EXIT_CODE" = "0" ]; then
    pass "Dashboard exited cleanly with code 0"
else
    fail "Dashboard exit code: $EXIT_CODE (expected 0)"
fi

STATUS=$("$NBS_TS" status "$DASH_HANDLE" 2>&1)
if echo "$STATUS" | grep -q "dead"; then
    pass "Dashboard process exited"
else
    fail "Dashboard process still running after 'q'"
fi

# ---------------------------------------------------------------
# O11. Escape also exits from overview
# ---------------------------------------------------------------
echo "O11. Escape exits from overview..."

DASH_ESC=$("$NBS_TS" create --name="dashtest-esc-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_ESC")
sleep 2

raw_send "$DASH_ESC" '\033'
"$NBS_TS" wait-complete "$DASH_ESC" --timeout=5 2>/dev/null || true
sleep 1

EXIT_ESC=$("$NBS_TS" exit-code "$DASH_ESC" 2>&1) || EXIT_ESC="unknown"
if [ "$EXIT_ESC" = "0" ]; then
    pass "Escape exited cleanly with code 0"
else
    fail "Escape exit code: $EXIT_ESC (expected 0)"
fi

# ---------------------------------------------------------------
# O12. Title bar shows agent and sidecar counts
# ---------------------------------------------------------------
echo "O12. Title bar shows agent and sidecar counts..."

if echo "$OUTPUT" | grep -qE "7 agents|agents.*7"; then
    pass "Title shows agent count"
else
    fail "Title missing agent count"
    echo "   First line: $(echo "$OUTPUT" | head -1)"
fi

# ---------------------------------------------------------------
# O13. Box-drawing characters present in table
# ---------------------------------------------------------------
echo "O13. Box-drawing characters in table..."

if echo "$OUTPUT" | grep -qE "─|│|╔|╚|╗|╝|═|║|┌|└|┐|┘"; then
    pass "Box-drawing characters present"
else
    fail "Box-drawing characters missing"
fi

# ---------------------------------------------------------------
# O14. Live update: killing an agent session changes display
# ---------------------------------------------------------------
echo "O14. Live update — killed agent reflected on next refresh..."

# Launch a fresh dashboard
DASH_LIVE=$("$NBS_TS" create --name="dashtest-live-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_LIVE")
sleep 3

OUTPUT_LIVE_BEFORE=$("$NBS_TS" read "$DASH_LIVE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# Kill the scribe's nbs-ts session (last-created for scribe)
SCRIBE_SESSION=$("$NBS_TS" find "dashtest-scribe-${TAG}" 2>/dev/null || true)
if [ -n "$SCRIBE_SESSION" ]; then
    "$NBS_TS" kill "$SCRIBE_SESSION" 2>/dev/null || true
fi

# Wait for at least one refresh cycle (2s interval per spec)
sleep 3

OUTPUT_LIVE_AFTER=$("$NBS_TS" read "$DASH_LIVE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# The scribe row should now show "dead" instead of "alive"
SCRIBE_AFTER=$(echo "$OUTPUT_LIVE_AFTER" | grep -i "scribe" || true)
if echo "$SCRIBE_AFTER" | grep -qiE "dead|DOWN|missing"; then
    pass "Live update: scribe shows dead after session killed"
else
    if [ "$OUTPUT_LIVE_AFTER" != "$OUTPUT_LIVE_BEFORE" ]; then
        pass "Live update: display changed after session killed"
    else
        fail "Live update: display did not change after killing scribe session"
    fi
fi

raw_send "$DASH_LIVE" "q"
"$NBS_TS" wait-complete "$DASH_LIVE" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# O15. Differential redraw: refresh does not clear screen
# ---------------------------------------------------------------
echo "O15. Differential redraw — refresh uses selective update, not screen clear..."

# Recreate the killed scribe session so we can kill it again for a state change
H_SCRIBE=$("$NBS_TS" create --name="dashtest-scribe-${TAG}" "sleep 3600" | tr -d '[:space:]')
HANDLES+=("$H_SCRIBE")

# Launch a fresh dashboard
DASH_DIFF=$("$NBS_TS" create --name="dashtest-diff-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_DIFF")
sleep 3

# Record the output.log byte count after initial render
DIFF_LOG="$HOME/.nbs-ts/sessions/$DASH_DIFF/output.log"
OFFSET_BEFORE=$(wc -c < "$DIFF_LOG" 2>/dev/null || echo 0)

# Trigger a state change — kill scribe again
"$NBS_TS" kill "$H_SCRIBE" 2>/dev/null || true

# Wait for one refresh cycle (>2s)
sleep 3

OFFSET_AFTER=$(wc -c < "$DIFF_LOG" 2>/dev/null || echo 0)

# Extract ONLY the bytes written during the refresh cycle
REFRESH_SIZE=$((OFFSET_AFTER - OFFSET_BEFORE))

if [ "$REFRESH_SIZE" -gt 0 ] 2>/dev/null; then
    REFRESH_BYTES=$(tail -c +"$((OFFSET_BEFORE + 1))" "$DIFF_LOG" 2>/dev/null | head -c "$REFRESH_SIZE")

    # A differential redraw should NOT clear the screen (ESC[2J)
    if echo "$REFRESH_BYTES" | grep -qP '\x1b\[2J'; then
        fail "Refresh used screen clear (ESC[2J) — not differential"
    else
        pass "Refresh did not clear screen"
    fi

    # A differential redraw should write fewer bytes than a full screen
    # Full screen at 120x30 = 3600 chars minimum. Differential for one
    # row change should be well under 1000 bytes.
    FULL_SCREEN_SIZE=$((120 * 30))
    if [ "$REFRESH_SIZE" -lt "$FULL_SCREEN_SIZE" ]; then
        pass "Refresh output ($REFRESH_SIZE bytes) smaller than full screen ($FULL_SCREEN_SIZE bytes)"
    else
        fail "Refresh output ($REFRESH_SIZE bytes) >= full screen ($FULL_SCREEN_SIZE bytes) — likely full redraw"
    fi

    # Check for selective cursor positioning (ESC[row;colH with row > 1)
    if echo "$REFRESH_BYTES" | grep -qP '\x1b\[[2-9][0-9]*;[0-9]+H|\x1b\[1[0-9]+;[0-9]+H'; then
        pass "Selective cursor positioning detected (ESC[row;colH)"
    else
        fail "No selective cursor positioning found — likely sequential redraw"
    fi
else
    fail "No new output detected during refresh cycle"
fi

raw_send "$DASH_DIFF" "q"
"$NBS_TS" wait-complete "$DASH_DIFF" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# O16. Activity column shows agent output, not just "Idle"
# ---------------------------------------------------------------
echo "O16. Activity column shows agent output content..."

# Create a fresh agent session that produces distinctive output
ACTIVITY_SESSION=$("$NBS_TS" create --name="dashtest-generalist-${TAG}" \
    "echo 'ACTIVITY-TEST-MARKER: compiling main.c'; sleep 3600" \
    | tr -d '[:space:]')
HANDLES+=("$ACTIVITY_SESSION")
sleep 1

# Launch a fresh dashboard
DASH_ACT=$("$NBS_TS" create --name="dashtest-activity-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_ACT")
sleep 3

OUTPUT_ACT=$("$NBS_TS" read "$DASH_ACT" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# The generalist row's activity column should show output content,
# not just "Idle" or a dash
GENERALIST_LINE=$(echo "$OUTPUT_ACT" | grep -i "generalist" || true)

if echo "$GENERALIST_LINE" | grep -qiE "compil|main\.c|ACTIVITY-TEST|build|running"; then
    pass "Activity column shows agent output content"
else
    if echo "$GENERALIST_LINE" | grep -qi "Idle"; then
        fail "Activity column shows 'Idle' instead of agent output"
        echo "   Line: $GENERALIST_LINE"
    else
        if [ -n "$GENERALIST_LINE" ]; then
            fail "Activity column content unclear"
            echo "   Line: $GENERALIST_LINE"
        else
            fail "Generalist row not found"
        fi
    fi
fi

raw_send "$DASH_ACT" "q"
"$NBS_TS" wait-complete "$DASH_ACT" --timeout=5 2>/dev/null || true

# ---------------------------------------------------------------
# Results
# ---------------------------------------------------------------
echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All overview tests passed"
    exit 0
else
    echo "$ERRORS overview test(s) failed"
    exit 1
fi
