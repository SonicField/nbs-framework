#!/bin/bash
# Test: restart script skill injection reaches claude
#
# Spawns a single agent the same way the restart script does and
# verifies: (1) only one nbs-ts session is created (no double-wrap),
# (2) the session runs claude (not tail -f), (3) the sidecar started
# with --initial-prompt so the skill will be injected.
#
# Catches the double-nbs-ts-session bug where nbs-ts create wrapped
# nbs-claude which created its own nbs-ts session internally.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_CHAT="$PROJECT_ROOT/bin/nbs-chat"

ERRORS=0
TMPDIR=""
AGENT_PID=""

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

cleanup() {
    # Kill agent and its children
    if [[ -n "$AGENT_PID" ]]; then
        kill "$AGENT_PID" 2>/dev/null || true
        wait "$AGENT_PID" 2>/dev/null || true
    fi
    if [[ -n "$TMPDIR" && -d "$TMPDIR" ]]; then
        # Kill nbs-ts session via metadata
        if [[ -f "$TMPDIR/.nbs/sessions/supervisor.json" ]]; then
            local h
            h=$(grep -o '"nbs_ts_handle": "[^"]*"' "$TMPDIR/.nbs/sessions/supervisor.json" 2>/dev/null \
                | cut -d'"' -f4)
            [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
        fi
        pkill -f "nbs-sidecar.*--handle=supervisor.*$TMPDIR" 2>/dev/null || true
        rm -rf "$TMPDIR"
    fi
}
trap cleanup EXIT

# Prerequisites
if [[ ! -x "$NBS_TS" ]]; then echo "SKIP: nbs-ts not found"; exit 0; fi
if [[ ! -x "$NBS_CHAT" ]]; then echo "SKIP: nbs-chat not found"; exit 0; fi
if ! command -v claude >/dev/null 2>&1; then echo "SKIP: claude not found"; exit 0; fi

echo "=== Restart Skill Injection Test ==="
echo ""

# Create a minimal project
TMPDIR=$(mktemp -d /tmp/nbs-restart-test.XXXXXX)
mkdir -p "$TMPDIR/.nbs/chat" "$TMPDIR/.nbs/pids" "$TMPDIR/.nbs/sessions" \
         "$TMPDIR/.nbs/events/processed" "$TMPDIR/.nbs/workers" \
         "$TMPDIR/.nbs/scribe" "$TMPDIR/.nbs/bin"

# Symlink tools
for tool in nbs-chat nbs-claude nbs-ts nbs-sidecar nbs-bus nbs-workers \
            nbs-spawn-worker nbs-chat-terminal-restart.sh nbs-claude-build-args; do
    [[ -e "$PROJECT_ROOT/bin/$tool" ]] && \
        ln -sf "$PROJECT_ROOT/bin/$tool" "$TMPDIR/.nbs/bin/$tool"
done

# Create chat file and registries
"$NBS_CHAT" create "$TMPDIR/.nbs/chat/test.chat" 2>/dev/null
for h in scribe gatekeeper testkeeper supervisor generalist theologian; do
    touch "$TMPDIR/.nbs/control-registry-${h}"
done

# Spawn one agent exactly as the restart script does (post-fix)
echo "SI1. Spawn agent without double-wrapping..."
(
    cd "$TMPDIR"
    NBS_HANDLE=supervisor \
    NBS_TRANSPORT=ts \
    NBS_INITIAL_PROMPT="/nbs-supervisor" \
    NBS_FORCE_SPAWN=1 \
    exec .nbs/bin/nbs-claude --dangerously-skip-permissions
) >/dev/null 2>&1 &
AGENT_PID=$!

# Wait for nbs-claude to create the session and write metadata
for i in $(seq 1 10); do
    [[ -f "$TMPDIR/.nbs/sessions/supervisor.json" ]] && break
    sleep 1
done

if [[ ! -f "$TMPDIR/.nbs/sessions/supervisor.json" ]]; then
    fail "nbs-claude did not write session metadata"
    echo ""
    echo "=== Result ==="
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi

# Read the inner nbs-ts handle from session metadata
TS_HANDLE=$(grep -o '"nbs_ts_handle": "[^"]*"' "$TMPDIR/.nbs/sessions/supervisor.json" \
    | cut -d'"' -f4)

if [[ -z "$TS_HANDLE" ]]; then
    fail "Session metadata missing nbs_ts_handle"
else
    pass "Session metadata has nbs_ts_handle: $TS_HANDLE"
fi

# SI1: Check only ONE session was created for this agent
# Count sessions whose command contains "claude" and matches our session ID
SESSION_ID=$(grep -o '"session_id": "[^"]*"' "$TMPDIR/.nbs/sessions/supervisor.json" \
    | cut -d'"' -f4)
SESSION_COUNT=$("$NBS_TS" list 2>/dev/null | grep -c "$SESSION_ID" || true)
SESSION_COUNT=$(echo "$SESSION_COUNT" | tr -d '[:space:]')
if [[ "$SESSION_COUNT" -eq 1 ]]; then
    pass "Single nbs-ts session (no double-wrapping)"
elif [[ "$SESSION_COUNT" -gt 1 ]]; then
    fail "Multiple sessions ($SESSION_COUNT) for same agent — double-wrapping"
else
    fail "Session $SESSION_ID not found in nbs-ts list"
fi

# SI2: Session runs claude, not tail
echo "SI2. Session runs claude (not tail)..."
if [[ -n "$TS_HANDLE" ]]; then
    SESSION_CMD=$("$NBS_TS" list 2>/dev/null | grep "^$TS_HANDLE" | cut -f4)
    if echo "$SESSION_CMD" | grep -q "claude"; then
        pass "Session command is claude"
    elif echo "$SESSION_CMD" | grep -q "tail"; then
        fail "Session command is tail -f (double-wrapping bug)"
    else
        fail "Unexpected session command: $SESSION_CMD"
    fi
else
    fail "No handle to check"
fi

# SI3: Sidecar started with --initial-prompt
echo "SI3. Sidecar has --initial-prompt..."
sleep 3
SIDECAR_LINE=$(ps aux | grep "nbs-sidecar.*--handle=supervisor" | grep -v grep | head -1)
if [[ -z "$SIDECAR_LINE" ]]; then
    fail "No sidecar process found for supervisor"
elif echo "$SIDECAR_LINE" | grep -q "initial-prompt"; then
    pass "Sidecar has --initial-prompt flag"
else
    fail "Sidecar running but missing --initial-prompt"
fi

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All skill injection tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
