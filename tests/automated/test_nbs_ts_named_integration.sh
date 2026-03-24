#!/bin/bash
# Test: nbs-ts named session integration — nbs-claude naming + restart scoping
#
# Tests I1-I2 from nbs-ts-named-sessions-plan.md (Phase 2)
# Falsification: I1 fails if nbs-claude does not pass --name; I2 fails if
# restart kills sessions outside its team.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"

ERRORS=0
TMPDIR=""
AGENT_PID=""
EXTRA_HANDLES=()

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

cleanup() {
    # Kill agent process if running
    if [[ -n "$AGENT_PID" ]]; then
        kill "$AGENT_PID" 2>/dev/null || true
        wait "$AGENT_PID" 2>/dev/null || true
    fi
    # Kill nbs-ts session from metadata
    if [[ -n "$TMPDIR" && -d "$TMPDIR" ]]; then
        if [[ -f "$TMPDIR/.nbs/sessions/supervisor.json" ]]; then
            local h
            h=$(grep -o '"nbs_ts_handle": "[^"]*"' "$TMPDIR/.nbs/sessions/supervisor.json" 2>/dev/null \
                | cut -d'"' -f4)
            [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
        fi
        pkill -f "nbs-sidecar.*--handle=supervisor.*$TMPDIR" 2>/dev/null || true
        rm -rf "$TMPDIR"
    fi
    # Clean up extra handles from I2
    for h in "${EXTRA_HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

# Prerequisites
if [[ ! -x "$NBS_TS" ]]; then echo "SKIP: nbs-ts not found"; exit 0; fi
if [[ ! -x "$NBS_CHAT" ]]; then echo "SKIP: nbs-chat not found"; exit 0; fi
if ! command -v claude >/dev/null 2>&1; then echo "SKIP: claude not found"; exit 0; fi

echo "=== nbs-ts Named Session Integration Test ==="
echo ""

# ================================================================
# I1: nbs-claude creates named session
# ================================================================
echo "I1. nbs-claude creates named session..."

# Create a minimal project with a chat file
TMPDIR=$(mktemp -d /tmp/nbs-named-int.XXXXXX)
mkdir -p "$TMPDIR/.nbs/chat" "$TMPDIR/.nbs/pids" "$TMPDIR/.nbs/sessions" \
         "$TMPDIR/.nbs/events/processed" "$TMPDIR/.nbs/workers" \
         "$TMPDIR/.nbs/scribe" "$TMPDIR/.nbs/bin"

# Symlink tools
for tool in nbs-chat nbs-claude nbs-ts nbs-sidecar nbs-bus nbs-workers \
            nbs-spawn-worker nbs-chat-terminal-restart.sh nbs-claude-build-args; do
    [[ -e "$PROJECT_ROOT/bin/$tool" ]] && \
        ln -sf "$PROJECT_ROOT/bin/$tool" "$TMPDIR/.nbs/bin/$tool"
done

# Create chat file named "test.chat"
"$NBS_CHAT" create "$TMPDIR/.nbs/chat/test.chat" 2>/dev/null
for h in scribe gatekeeper testkeeper supervisor generalist theologian; do
    touch "$TMPDIR/.nbs/control-registry-${h}"
done

# Spawn nbs-claude with NBS_HANDLE=supervisor, NBS_TRANSPORT=ts
(
    cd "$TMPDIR"
    NBS_HANDLE=supervisor \
    NBS_TRANSPORT=ts \
    NBS_INITIAL_PROMPT="Say hello" \
    NBS_FORCE_SPAWN=1 \
    exec .nbs/bin/nbs-claude --dangerously-skip-permissions
) >/dev/null 2>&1 &
AGENT_PID=$!

# Wait for session metadata to appear
for i in $(seq 1 15); do
    [[ -f "$TMPDIR/.nbs/sessions/supervisor.json" ]] && break
    sleep 1
done

if [[ ! -f "$TMPDIR/.nbs/sessions/supervisor.json" ]]; then
    fail "nbs-claude did not write session metadata"
else
    # Read the nbs-ts handle from metadata
    TS_HANDLE=$(grep -o '"nbs_ts_handle": "[^"]*"' "$TMPDIR/.nbs/sessions/supervisor.json" \
        | cut -d'"' -f4)

    if [[ -z "$TS_HANDLE" ]]; then
        fail "Session metadata missing nbs_ts_handle"
    else
        # Check that nbs-ts list shows a session named nbs-supervisor-test
        LIST_LINE=$("$NBS_TS" list 2>/dev/null | grep "^$TS_HANDLE" || true)
        if echo "$LIST_LINE" | grep -q "nbs-supervisor-test"; then
            pass "Session $TS_HANDLE named 'nbs-supervisor-test' in list"
        else
            fail "Session $TS_HANDLE does not have expected name 'nbs-supervisor-test'"
            echo "   List line: $LIST_LINE"
        fi

        # Also verify via nbs-ts find
        FOUND=$("$NBS_TS" find nbs-supervisor-test 2>/dev/null) && RC=$? || RC=$?
        FOUND=$(echo "$FOUND" | tr -d '[:space:]')
        if [[ $RC -eq 0 ]] && [[ "$FOUND" == "$TS_HANDLE" ]]; then
            pass "nbs-ts find nbs-supervisor-test returns correct handle"
        else
            fail "nbs-ts find nbs-supervisor-test: rc=$RC, output='$FOUND', expected '$TS_HANDLE'"
        fi
    fi
fi

# Kill the ts session FIRST — this causes nbs-ts attach to return,
# allowing the nbs-claude wrapper to exit cleanly.
if [[ -n "${TS_HANDLE:-}" ]]; then
    "$NBS_TS" kill "$TS_HANDLE" 2>/dev/null || true
fi
# Kill sidecar
if [[ -n "$TMPDIR" ]]; then
    pkill -f "nbs-sidecar.*--handle=supervisor.*$TMPDIR" 2>/dev/null || true
fi
# Now kill the nbs-claude wrapper and wait
if [[ -n "$AGENT_PID" ]]; then
    kill "$AGENT_PID" 2>/dev/null || true
    # Use timeout on wait to prevent hanging
    for _w in $(seq 1 10); do
        kill -0 "$AGENT_PID" 2>/dev/null || break
        sleep 0.5
    done
    kill -9 "$AGENT_PID" 2>/dev/null || true
    wait "$AGENT_PID" 2>/dev/null || true
    AGENT_PID=""
fi
sleep 1

# ================================================================
# I2: Restart kills only its team
# ================================================================
echo ""
echo "I2. Restart kills only its team..."

# Create two named sessions: one for "poem" team, one for "other" team
H_POEM=$("$NBS_TS" create --name=nbs-fake-poem bash 2>&1 | tr -d '[:space:]')
EXTRA_HANDLES+=("$H_POEM")
H_OTHER=$("$NBS_TS" create --name=nbs-fake-other bash 2>&1 | tr -d '[:space:]')
EXTRA_HANDLES+=("$H_OTHER")
sleep 0.5

# Verify both are alive
LIST_BEFORE=$("$NBS_TS" list 2>/dev/null)
if echo "$LIST_BEFORE" | grep -q "${H_POEM}.*alive.*nbs-fake-poem" && \
   echo "$LIST_BEFORE" | grep -q "${H_OTHER}.*alive.*nbs-fake-other"; then
    pass "Both sessions alive before simulated restart"
else
    fail "Sessions not alive before restart"
    echo "   List: $LIST_BEFORE"
fi

# Simulate what the restart script does: use nbs-ts list --name=poem to find
# poem sessions, then kill only those
while IFS=$'\t' read -r handle status name cmd; do
    [[ -n "$handle" ]] || continue
    "$NBS_TS" kill "$handle" 2>/dev/null || true
done < <("$NBS_TS" list --name=poem 2>/dev/null || true)

sleep 0.5

# Verify: poem session should be dead, other should be alive
LIST_AFTER=$("$NBS_TS" list 2>/dev/null)
POEM_STATUS=$("$NBS_TS" status "$H_POEM" 2>/dev/null || echo "dead/gone")
OTHER_STATUS=$("$NBS_TS" status "$H_OTHER" 2>/dev/null || echo "dead/gone")

if echo "$POEM_STATUS" | grep -q "dead\|exited"; then
    pass "Poem session killed"
else
    fail "Poem session still alive after targeted kill: $POEM_STATUS"
fi

if echo "$OTHER_STATUS" | grep -q "alive"; then
    pass "Other session still alive (not killed)"
else
    fail "Other session was killed (should have survived): $OTHER_STATUS"
fi

# Also verify --name filter is correct
FILTERED=$("$NBS_TS" list --name=poem 2>/dev/null || true)
if ! echo "$FILTERED" | grep -q "$H_OTHER"; then
    pass "list --name=poem does not show 'other' session"
else
    fail "list --name=poem incorrectly includes 'other' session"
fi

# ================================================================
# I3: Spawn worker creates named session
# ================================================================
echo ""
echo "I3. Spawn worker creates named session..."

SPAWN_WORKER="$PROJECT_ROOT/bin/nbs-spawn-worker"
if [[ ! -x "$SPAWN_WORKER" ]]; then
    echo "   SKIP: nbs-spawn-worker not found"
else
    # Create a temp project with minimal .nbs structure
    I3_TMPDIR=$(mktemp -d /tmp/nbs-named-i3.XXXXXX)
    mkdir -p "$I3_TMPDIR/.nbs/chat" "$I3_TMPDIR/.nbs/pids" \
             "$I3_TMPDIR/.nbs/workers" "$I3_TMPDIR/.nbs/bin" \
             "$I3_TMPDIR/.nbs/events/processed" "$I3_TMPDIR/.nbs/sessions"

    # Symlink tools
    for tool in nbs-claude nbs-ts nbs-sidecar nbs-bus nbs-workers \
                nbs-spawn-worker nbs-claude-build-args; do
        [[ -e "$PROJECT_ROOT/bin/$tool" ]] && \
            ln -sf "$PROJECT_ROOT/bin/$tool" "$I3_TMPDIR/.nbs/bin/$tool"
    done

    # Create minimal chat file
    "$PROJECT_ROOT/bin/nbs-chat" create "$I3_TMPDIR/.nbs/chat/test.chat" 2>/dev/null || true

    # Create a minimal skill file
    I3_SKILL=$(mktemp /tmp/nbs-i3-skill.XXXXXX.md)
    echo "# Test Skill" > "$I3_SKILL"
    echo "Say hello and set State: completed immediately." >> "$I3_SKILL"

    # Spawn the worker
    WORKER_OUT=$("$SPAWN_WORKER" testworker "$I3_TMPDIR" "$I3_SKILL" \
        "Echo done and set State: completed" 2>&1) || true

    # Wait for session to appear
    sleep 3

    # Verify nbs-ts list shows a session with name containing "worker"
    LIST_I3=$("$NBS_TS" list 2>/dev/null || true)
    if echo "$LIST_I3" | grep -q "nbs-testworker-worker"; then
        pass "nbs-spawn-worker created session with name containing 'nbs-testworker-worker'"

        # Also verify via nbs-ts find (partial name won't work, but list confirms presence)
        WORKER_NAME=$(echo "$LIST_I3" | grep -o 'nbs-testworker-worker-[a-f0-9]*' | head -1)
        if [[ -n "$WORKER_NAME" ]]; then
            FOUND_HANDLE=$("$NBS_TS" find "$WORKER_NAME" 2>/dev/null) && RC=$? || RC=$?
            FOUND_HANDLE=$(echo "$FOUND_HANDLE" | tr -d '[:space:]')
            if [[ $RC -eq 0 ]] && [[ -n "$FOUND_HANDLE" ]]; then
                pass "nbs-ts find $WORKER_NAME returned handle: $FOUND_HANDLE"
                EXTRA_HANDLES+=("$FOUND_HANDLE")
            else
                fail "nbs-ts find $WORKER_NAME failed (rc=$RC)"
            fi
        fi
    else
        fail "No session with name containing 'nbs-testworker-worker' in nbs-ts list"
        echo "   Worker output: $WORKER_OUT"
        echo "   List output:"
        echo "$LIST_I3" | sed 's/^/      /'
    fi

    # Clean up I3
    rm -f "$I3_SKILL"
    # Kill any worker sessions we found
    if [[ -n "${FOUND_HANDLE:-}" ]]; then
        "$NBS_TS" kill "$FOUND_HANDLE" 2>/dev/null || true
    fi
    rm -rf "$I3_TMPDIR"
fi

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All named session integration tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
