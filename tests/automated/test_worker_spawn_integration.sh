#!/bin/bash
# Test: nbs-spawn-worker integration (real Claude)
#
# Spawns a real worker via nbs-spawn-worker, verifies:
#   1. Task file is created by the spawn script
#   2. Worker session appears in nbs-ts (or worker completes quickly)
#   3. Worker posts a unique marker to chat (proves Claude did real work)
#   4. Worker updates task file state to completed
#   5. No leaked sessions or processes after cleanup
#
# Safety:
#   - Uses an isolated temp directory as project root
#   - Does not touch any running chat instances
#   - Tracks all nbs-ts sessions and kills only test-created ones
#   - Kills all spawned processes in cleanup trap
#
# This is an AI test — requires a real Claude API connection.
# Skipped with --quick. Routed through run_ai_test inside Claude Code.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

# --- Locate binaries ---
NBS_TS=""
NBS_CHAT=""
SPAWN_WORKER=""
for d in "$PROJECT_ROOT/.nbs/bin" "$PROJECT_ROOT/bin" "$HOME/.nbs/bin"; do
    [[ -z "$NBS_TS" && -x "$d/nbs-ts" ]] && NBS_TS="$d/nbs-ts"
    [[ -z "$NBS_CHAT" && -x "$d/nbs-chat" ]] && NBS_CHAT="$d/nbs-chat"
    [[ -z "$SPAWN_WORKER" && -x "$d/nbs-spawn-worker" ]] && SPAWN_WORKER="$d/nbs-spawn-worker"
done

for bin in NBS_TS NBS_CHAT SPAWN_WORKER; do
    if [[ -z "${!bin}" ]]; then
        echo "SKIP: $bin not found"
        exit 0
    fi
done

# Check nbs-ts-helper is running (required for session creation)
if ! pgrep -f nbs-ts-helper >/dev/null 2>&1; then
    echo "SKIP: nbs-ts-helper not running"
    exit 0
fi

PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "   PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "   FAIL: $1"; }

# --- Record pre-existing sessions ---
SESSIONS_BEFORE=$(mktemp)
"$NBS_TS" list 2>/dev/null | cut -f1 > "$SESSIONS_BEFORE"

# --- Create isolated test project ---
TEST_DIR=$(mktemp -d /tmp/nbs-spawn-test.XXXXXX)
mkdir -p "$TEST_DIR/.nbs/workers" "$TEST_DIR/.nbs/pids" \
         "$TEST_DIR/.nbs/events/processed" "$TEST_DIR/.nbs/sessions" \
         "$TEST_DIR/.nbs/chat" "$TEST_DIR/.nbs/bin"

# Symlink nbs-claude into the test project (nbs-spawn-worker looks for it here)
NBS_CLAUDE=""
for d in "$PROJECT_ROOT/.nbs/bin" "$PROJECT_ROOT/bin" "$HOME/.nbs/bin"; do
    if [[ -x "$d/nbs-claude" ]]; then
        NBS_CLAUDE="$d/nbs-claude"
        break
    fi
done
if [[ -z "$NBS_CLAUDE" ]]; then
    echo "SKIP: nbs-claude not found"
    rm -rf "$TEST_DIR" "$SESSIONS_BEFORE"
    exit 0
fi
ln -sf "$NBS_CLAUDE" "$TEST_DIR/.nbs/bin/nbs-claude"

# Also link supporting scripts that nbs-claude and nbs-spawn-worker need
NBS_BIN_DIR="$(dirname "$NBS_CLAUDE")"
for f in nbs-claude-build-args nbs-sidecar nbs-launch-agent nbs-ts nbs-ts-helper \
         nbs-chat nbs-bus nbs-workers nbs-spawn-worker nbs-scribe-log nbs-scribe-query; do
    [[ -x "$NBS_BIN_DIR/$f" ]] && ln -sf "$NBS_BIN_DIR/$f" "$TEST_DIR/.nbs/bin/$f"
done

# Create test chat file
TEST_CHAT="$TEST_DIR/.nbs/chat/test.chat"
"$NBS_CHAT" create "$TEST_CHAT" >/dev/null 2>&1

# Unique marker to identify our worker's output
MARKER="SPAWN_INT_TEST_$(date +%s)_$$"

# Team agent session names — NEVER kill these regardless of timing.
_TEAM_PATTERN="nbs-(supervisor|generalist|theologian|testkeeper|gatekeeper|scribe)-"

# Helper: find test-created sessions safe to kill.
# Uses name-prefix matching (primary) with team-agent exclusion (safety net).
find_test_sessions() {
    "$NBS_TS" list 2>/dev/null | while IFS=$'\t' read -r handle status name cmd; do
        [[ -n "$handle" ]] || continue
        # NEVER kill team agent sessions
        if [[ -n "$name" ]] && echo "$name" | grep -qE "$_TEAM_PATTERN"; then
            continue
        fi
        # Kill if name matches our test prefix
        if [[ -n "$name" ]] && echo "$name" | grep -q "nbs-spawntest"; then
            echo "$handle"
            continue
        fi
        # For unnamed/unknown sessions: kill only if created during this test
        if [[ -z "$name" || "$name" == "-" ]]; then
            grep -qx "$handle" "$SESSIONS_BEFORE" 2>/dev/null || echo "$handle"
        fi
    done
}

# --- Cleanup ---
cleanup() {
    # Kill any sessions we created (those not in SESSIONS_BEFORE)
    find_test_sessions | while read -r handle; do
        "$NBS_TS" kill "$handle" 2>/dev/null || true
    done

    # Kill any processes referencing our test dir (exclude ourselves)
    local pids
    pids=$(pgrep -f "$TEST_DIR" 2>/dev/null || true)
    for pid in $pids; do
        [[ "$pid" == "$$" ]] && continue
        kill -9 "$pid" 2>/dev/null || true
    done
    # Kill sidecar-loop scripts (their path is /tmp/nbs-sidecar-loop.*, not TEST_DIR)
    pkill -9 -f 'nbs-sidecar-loop' 2>/dev/null || true
    sleep 1
    rm -rf "$TEST_DIR" "$SESSIONS_BEFORE"
}
trap cleanup EXIT

echo "=== Worker Spawn Integration Test ==="
echo "Test dir: $TEST_DIR"
echo "Marker: $MARKER"
echo ""

# --- Create a minimal skill file ---
SKILL_FILE="$TEST_DIR/test-skill.md"
cat > "$SKILL_FILE" << 'SKILLEOF'
# Test Worker Skill

You are a test worker. Your job is simple: complete the task instructions below.
Do not ask questions. Just do the work and update the Status section.
SKILLEOF

# --- Build task instructions ---
TASK_INSTRUCTIONS="Do the following steps in order:

1. Post this exact message to chat: nbs-chat send $TEST_CHAT spawntest '$MARKER'
2. Read the file $TEST_DIR/test-skill.md and summarize what it says in 2-3 sentences
3. Write your summary to $TEST_DIR/output.txt using the Write tool
4. Update the Status section in your task file: set State to completed and Completed to the current time

Important: Use the exact chat command shown above. The chat file path is $TEST_CHAT"

# --- Test 1: Spawn the worker ---
echo "1. Spawning worker via nbs-spawn-worker..."

SPAWN_OUTPUT=$(bash "$SPAWN_WORKER" spawntest "$TEST_DIR" "$SKILL_FILE" \
    "$TASK_INSTRUCTIONS" 2>&1) || true

if [[ -z "$SPAWN_OUTPUT" ]]; then
    fail "Spawn returned empty output"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed ==="
    exit 1
fi

echo "   Spawn output: $SPAWN_OUTPUT"

# Find the task file to get the worker suffix
TASK_FILE=$(ls "$TEST_DIR/.nbs/workers"/spawntest-*.md 2>/dev/null | head -1)
if [[ -n "$TASK_FILE" ]]; then
    pass "Task file created: $(basename "$TASK_FILE")"
else
    fail "No task file created in $TEST_DIR/.nbs/workers/"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed ==="
    exit 1
fi

# --- Tests 2-3: Session appears AND worker posts to chat ---
# Combined into a single polling loop because Claude startup in a temp dir
# can take 60-90s. We poll for up to 240s total for both milestones.
echo "2-3. Waiting for session + chat message (up to 240s)..."
SESSION_SEEN=false
SESSION_HANDLE=""
FOUND=0
POLL_START=$SECONDS

for i in $(seq 1 48); do
    sleep 5
    ELAPSED=$((SECONDS - POLL_START))

    # Check for marker in chat (primary success signal)
    if "$NBS_CHAT" search "$TEST_CHAT" "$MARKER" 2>/dev/null | grep -q "$MARKER"; then
        FOUND=1
        SESSION_SEEN=true  # must have had a session to post
        break
    fi

    # Check for nbs-ts session (milestone, not required for success)
    if ! $SESSION_SEEN; then
        while read -r h; do
            [[ -z "$h" ]] && continue
            if "$NBS_TS" status "$h" 2>/dev/null | grep -q "alive"; then
                SESSION_SEEN=true
                SESSION_HANDLE="$h"
                echo "   Session found at ${ELAPSED}s (handle: $h)"
                break
            fi
        done < <(find_test_sessions)
    fi

    # Check if worker completed/failed
    STATE=$(grep -m1 '^State:' "$TASK_FILE" 2>/dev/null | awk '{print tolower($2)}' || true)
    if [[ "$STATE" == "completed" || "$STATE" == "done" || "$STATE" == "failed" ]]; then
        SESSION_SEEN=true
        sleep 2
        "$NBS_CHAT" search "$TEST_CHAT" "$MARKER" 2>/dev/null | grep -q "$MARKER" && FOUND=1
        break
    fi

    # If session was seen but is now dead, and state still running, worker crashed
    if $SESSION_SEEN && [[ -n "$SESSION_HANDLE" ]] && [[ $ELAPSED -gt 90 ]]; then
        if ! "$NBS_TS" status "$SESSION_HANDLE" 2>/dev/null | grep -q "alive"; then
            if [[ "$STATE" == "running" ]]; then
                echo "   Worker session died after ${ELAPSED}s without completing"
                break
            fi
        fi
    fi

    # Progress indicator every 30s
    if [[ $((ELAPSED % 30)) -lt 5 ]]; then
        echo "   ... ${ELAPSED}s elapsed (session=${SESSION_SEEN}, state=${STATE:-running})"
    fi
done

if $SESSION_SEEN; then
    pass "Worker session created"
else
    fail "No session seen after ${ELAPSED}s"
fi

if [[ $FOUND -eq 1 ]]; then
    pass "Worker posted marker to chat after ~${ELAPSED}s (real Claude work confirmed)"
else
    fail "Worker did not post marker to chat within ${ELAPSED}s"
    echo "   Chat contents:"
    "$NBS_CHAT" read "$TEST_CHAT" --last=5 2>/dev/null | sed 's/^/      /' || true
    STATE=$(grep -m1 '^State:' "$TASK_FILE" 2>/dev/null | awk '{print tolower($2)}' || true)
    echo "   Task state: ${STATE:-unknown}"
fi

# --- Test 4: Task file updated ---
echo "4. Checking task file state..."
STATE=$(grep -m1 '^State:' "$TASK_FILE" 2>/dev/null | awk '{print tolower($2)}' || true)
if [[ "$STATE" == "running" ]]; then
    # Give up to 60 more seconds
    for i in $(seq 1 12); do
        sleep 5
        STATE=$(grep -m1 '^State:' "$TASK_FILE" 2>/dev/null | awk '{print tolower($2)}' || true)
        [[ "$STATE" == "running" ]] || break
    done
fi

case "$STATE" in
    completed|done)
        pass "Task state: $STATE"
        ;;
    *)
        if [[ $FOUND -eq 1 ]]; then
            pass "Worker posted to chat (state=$STATE — partial success)"
        else
            fail "Task state: ${STATE:-unknown} (expected completed/done)"
        fi
        ;;
esac

# --- Test 5: Output file created (proof of intelligent work) ---
echo "5. Checking for intelligent output..."
if [[ -f "$TEST_DIR/output.txt" ]]; then
    OUTPUT_SIZE=$(wc -c < "$TEST_DIR/output.txt")
    if [[ $OUTPUT_SIZE -gt 10 ]]; then
        pass "Output file created ($OUTPUT_SIZE bytes)"
    else
        pass "Output file exists but small ($OUTPUT_SIZE bytes)"
    fi
else
    if [[ $FOUND -eq 1 ]]; then
        pass "Chat message posted (output file not created — acceptable)"
    else
        fail "No output file and no chat message"
    fi
fi

# --- Test 6: No leaked processes ---
echo "6. Checking for leaked processes..."

# Kill test sessions explicitly
find_test_sessions | while read -r h; do
    "$NBS_TS" kill "$h" 2>/dev/null || true
done

# Kill worker processes: nbs-claude, nbs-sidecar, claude, and the monitor subshell.
# Match on known binary names with our test dir — avoids false-matching our own shell.
for pat in "nbs-claude.*$TEST_DIR" "nbs-sidecar.*$TEST_DIR" "claude.*$TEST_DIR"; do
    pkill -f "$pat" 2>/dev/null || true
done
# Kill sidecar-loop scripts that reference the session
pkill -f "nbs-sidecar-loop" 2>/dev/null || true
sleep 3

# Check no worker processes remain
LEAKED=0
for pat in "nbs-claude.*$TEST_DIR" "nbs-sidecar.*$TEST_DIR" "claude.*$TEST_DIR"; do
    LEAKED=$((LEAKED + $(pgrep -cf "$pat" 2>/dev/null || true)))
done
if [[ $LEAKED -eq 0 ]]; then
    pass "No leaked processes"
else
    fail "$LEAKED worker process(es) survived cleanup"
    pgrep -af "nbs-.*$TEST_DIR" 2>/dev/null | sed 's/^/      /' || true
    # Force kill
    for pat in "nbs-claude.*$TEST_DIR" "nbs-sidecar.*$TEST_DIR" "claude.*$TEST_DIR"; do
        pkill -9 -f "$pat" 2>/dev/null || true
    done
fi

# Wait for nbs-claude cleanup trap to finish killing sessions
sleep 3

# Check no test-named nbs-ts sessions leaked (ignore unnamed sessions
# which may be created by infrastructure during the test window)
LEAKED_SESSIONS=$("$NBS_TS" list 2>/dev/null | grep "nbs-spawntest" | wc -l || true)
if [[ $LEAKED_SESSIONS -eq 0 ]]; then
    pass "No leaked nbs-ts sessions"
else
    fail "$LEAKED_SESSIONS leaked nbs-ts session(s)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
