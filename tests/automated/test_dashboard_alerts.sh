#!/bin/bash
# Test: dashboard alert colouring — dead agents, missing sidecars, cursor lag,
#       long silence
#
# Verifies that the dashboard uses correct colour highlighting for alert
# conditions per the authoritative spec (feature-requests/dashboard.md):
#   - Dead agent: red in Status column
#   - Missing sidecar: red in Sidecar column
#   - Cursor behind >10: yellow in Cursor column
#   - Cursor behind >50: red in Cursor column
#   - No post for >15 minutes: yellow in Last Post column
#   - No post for >30 minutes: red in Last Post column
#
# NBS palette colours: NBS_STYLE_ERROR=fg:196, NBS_STYLE_WARNING=fg:226

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
TAG="dashalert$$"

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

echo "=== Dashboard Alert Colouring Test ==="
echo ""

# ---------------------------------------------------------------
# Mock infrastructure — deliberately create alert conditions
# ---------------------------------------------------------------

MOCK_ROOT="$TMPDIR/project"
MOCK_NBS="$MOCK_ROOT/.nbs"
mkdir -p "$MOCK_NBS/chat" "$MOCK_NBS/events"
echo "$TAG" > "$MOCK_NBS/project-id"

CHAT="$MOCK_NBS/chat/team.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null

# Send 60 messages from supervisor to create cursor-behind conditions.
# No messages from medic or scribe — they will have "long silence".
for i in $(seq 1 60); do
    "$NBS_CHAT" send "$CHAT" supervisor "Message $i"
done

# Cursor file:
#   supervisor=60 → 0 behind (OK)
#   generalist=49 → 11 behind (>10 → yellow)
#   gatekeeper=5  → 55 behind (>50 → red, also dead agent)
#   theologian=48 → 12 behind (>10 → yellow, also missing sidecar)
#   testkeeper=60 → 0 behind (OK)
#   scribe=60     → 0 behind (OK, but never posted → silence alert)
#   medic=0       → 60 behind (>50 → red, never posted → silence alert)
cat > "$MOCK_NBS/chat/team.chat.cursors" << EOF
# Read cursors — last-read message index per handle
supervisor=60
generalist=49
gatekeeper=5
theologian=48
testkeeper=60
scribe=60
medic=0
EOF

AGENTS="supervisor generalist gatekeeper theologian testkeeper scribe medic"

# Create sessions for most agents — but NOT gatekeeper (simulates dead agent)
LIVE_AGENTS="supervisor generalist theologian testkeeper scribe medic"
for agent in $LIVE_AGENTS; do
    H=$("$NBS_TS" create --name="dashtest-${agent}-${TAG}" "sleep 3600" | tr -d '[:space:]')
    HANDLES+=("$H")
done

# Create sidecars for most agents — but NOT theologian (simulates missing sidecar)
SIDECAR_AGENTS="supervisor generalist gatekeeper testkeeper scribe medic"
for agent in $SIDECAR_AGENTS; do
    (exec -a "dashtest-sidecar --handle=$agent --root=$MOCK_ROOT" sleep 3600) &
    SIDECAR_PIDS+=($!)
done

sleep 2

# ---------------------------------------------------------------
# Launch dashboard and capture output (with ANSI codes preserved)
# ---------------------------------------------------------------

DASH_HANDLE=$("$NBS_TS" create --name="dashtest-alerts-${TAG}" \
    "$DASHBOARD $MOCK_ROOT" | tr -d '[:space:]')
HANDLES+=("$DASH_HANDLE")
sleep 3

RAW=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --no-strip --width=120 --height=30)

OUTPUT=$("$NBS_TS" read "$DASH_HANDLE" 2>&1 | \
    "$NBS_TS_RENDER" --width=120 --height=30)

# ---------------------------------------------------------------
# A1. Dead agent shows red in Status column
# ---------------------------------------------------------------
echo "A1. Dead agent shows red in Status column..."

GATEKEEPER_LINE=$(echo "$RAW" | grep -i "gatekeeper" || true)

# Check for red SGR: \033[31m (basic), \033[91m (bright), \033[38;5;196m (256-colour),
# \033[41m (bg red basic), \033[48;5;196m (bg red 256-colour)
if echo "$GATEKEEPER_LINE" | grep -qE '\[31m|\[91m|\[38;5;196m|\[41m|\[48;5;196m'; then
    pass "Dead agent (gatekeeper) has red colouring"
else
    if [ -n "$GATEKEEPER_LINE" ]; then
        fail "Dead agent (gatekeeper) visible but no red colouring detected"
        echo "   Line: $(echo "$GATEKEEPER_LINE" | cat -v | head -c 200)"
    else
        fail "Gatekeeper row not found"
    fi
fi

# ---------------------------------------------------------------
# A2. Missing sidecar shows red in Sidecar column
# ---------------------------------------------------------------
echo "A2. Missing sidecar shows red..."

THEOLOGIAN_LINE=$(echo "$RAW" | grep -i "theologian" || true)

if echo "$THEOLOGIAN_LINE" | grep -qE '\[31m|\[91m|\[38;5;196m|\[41m|\[48;5;196m'; then
    pass "Missing sidecar (theologian) has red colouring"
else
    if [ -n "$THEOLOGIAN_LINE" ]; then
        fail "Theologian visible but no red colouring for missing sidecar"
        echo "   Line: $(echo "$THEOLOGIAN_LINE" | cat -v | head -c 200)"
    else
        fail "Theologian row not found"
    fi
fi

# ---------------------------------------------------------------
# A3. Cursor behind >10 shows yellow
# ---------------------------------------------------------------
echo "A3. Cursor behind >10 shows yellow..."

# generalist is 11 behind, theologian is 12 behind — both should show yellow
# Check for yellow SGR: \033[33m (basic), \033[93m (bright), \033[38;5;226m (256-colour)
GENERALIST_LINE=$(echo "$RAW" | grep -i "generalist" || true)

if echo "$GENERALIST_LINE" | grep -qE '\[33m|\[93m|\[38;5;226m'; then
    pass "Cursor behind >10 (generalist, 11 behind) has yellow colouring"
else
    if [ -n "$GENERALIST_LINE" ]; then
        fail "Generalist visible but no yellow colouring for cursor >10 behind"
        echo "   Line: $(echo "$GENERALIST_LINE" | cat -v | head -c 200)"
    else
        fail "Generalist row not found"
    fi
fi

# ---------------------------------------------------------------
# A4. Cursor behind >50 shows red
# ---------------------------------------------------------------
echo "A4. Cursor behind >50 shows red..."

# medic is 60 behind, gatekeeper is 55 behind — both should show red in Cursor
MEDIC_LINE=$(echo "$RAW" | grep -i "medic" || true)

if echo "$MEDIC_LINE" | grep -qE '\[31m|\[91m|\[38;5;196m|\[41m|\[48;5;196m'; then
    pass "Cursor behind >50 (medic, 60 behind) has red colouring"
else
    if [ -n "$MEDIC_LINE" ]; then
        fail "Medic visible but no red colouring for cursor >50 behind"
        echo "   Line: $(echo "$MEDIC_LINE" | cat -v | head -c 200)"
    else
        fail "Medic row not found"
    fi
fi

# ---------------------------------------------------------------
# A5. Long silence (>15 min) shows yellow in Last Post
# ---------------------------------------------------------------
echo "A5. Long silence (>15 min) shows yellow in Last Post..."

# medic and scribe never posted — their "Last Post" should be very old or "never".
# This should trigger at least yellow (>15 min) or red (>30 min).
# We check that SOME colour is applied to the row.
# Since we can't control message timestamps, we verify the column structure
# and that agents who never posted have colour indicators.
SCRIBE_LINE=$(echo "$RAW" | grep -i "scribe" || true)

if echo "$SCRIBE_LINE" | grep -qE '\[33m|\[93m|\[38;5;226m|\[31m|\[91m|\[38;5;196m'; then
    pass "Long silence (scribe, never posted) has yellow or red colouring"
else
    if [ -n "$SCRIBE_LINE" ]; then
        # Scribe never posted a message — if dashboard shows a long silence indicator,
        # it should be coloured. If no colour, the dashboard may not implement
        # silence detection yet (or timestamps aren't old enough)
        fail "Scribe visible but no silence warning colour detected"
        echo "   Line: $(echo "$SCRIBE_LINE" | cat -v | head -c 200)"
    else
        fail "Scribe row not found"
    fi
fi

# ---------------------------------------------------------------
# A6. Status text correct for dead agent
# ---------------------------------------------------------------
echo "A6. Status text correct for dead agent..."

GATEKEEPER_TEXT=$(echo "$OUTPUT" | grep -i "gatekeeper" || true)
if echo "$GATEKEEPER_TEXT" | grep -qiE "dead|missing|DOWN"; then
    pass "Dead agent status text correct"
else
    if [ -n "$GATEKEEPER_TEXT" ]; then
        fail "Gatekeeper visible but status text not 'dead'/'missing'"
        echo "   Text: $GATEKEEPER_TEXT"
    else
        fail "Gatekeeper row not found in plain output"
    fi
fi

# ---------------------------------------------------------------
# A7. Sidecar MISSING text for agent without sidecar
# ---------------------------------------------------------------
echo "A7. Sidecar MISSING text correct..."

THEOLOGIAN_TEXT=$(echo "$OUTPUT" | grep -i "theologian" || true)
if echo "$THEOLOGIAN_TEXT" | grep -qi "MISSING"; then
    pass "Missing sidecar text correct for theologian"
else
    if [ -n "$THEOLOGIAN_TEXT" ]; then
        fail "Theologian visible but sidecar not showing MISSING"
        echo "   Text: $THEOLOGIAN_TEXT"
    else
        fail "Theologian row not found in plain output"
    fi
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
    echo "All alert colouring tests passed"
    exit 0
else
    echo "$ERRORS alert colouring test(s) failed"
    exit 1
fi
