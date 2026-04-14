#!/bin/bash
# test_sidecar_valgrind.sh — Run nbs-sidecar under valgrind.
#
# Creates a temporary project with .nbs/ structure, launches the
# sidecar under valgrind with a fake nbs-ts session, simulates
# 60 seconds of operation, then checks for leaks.
#
# Exit: 0 if no leaks, 1 if leaks detected.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="$REPO_ROOT/bin"
SUPP_FILE="$SCRIPT_DIR/nbs.supp"
VALGRIND_LOG=""
TMPDIR_ROOT=""

cleanup() {
    local exit_code=$?
    # Kill any remaining sidecar
    if [[ -n "${SIDECAR_PID:-}" ]] && kill -0 "$SIDECAR_PID" 2>/dev/null; then
        kill "$SIDECAR_PID" 2>/dev/null || true
        wait "$SIDECAR_PID" 2>/dev/null || true
    fi
    # Kill fake session if alive
    if [[ -n "${SESSION_PID:-}" ]] && kill -0 "$SESSION_PID" 2>/dev/null; then
        kill "$SESSION_PID" 2>/dev/null || true
        wait "$SESSION_PID" 2>/dev/null || true
    fi
    # Clean up temp dir
    if [[ -n "$TMPDIR_ROOT" && -d "$TMPDIR_ROOT" ]]; then
        rm -rf "$TMPDIR_ROOT"
    fi
    exit "$exit_code"
}
trap cleanup EXIT

# Verify prerequisites
for cmd in valgrind "$BIN_DIR/nbs-sidecar" "$BIN_DIR/nbs-chat" "$BIN_DIR/nbs-bus"; do
    if [[ ! -x "$cmd" ]] && ! command -v "$cmd" &>/dev/null; then
        echo "FAIL: required command not found: $cmd" >&2
        exit 1
    fi
done

echo "=== nbs-sidecar valgrind test ==="

# Create temporary project structure
TMPDIR_ROOT=$(mktemp -d /tmp/nbs-sidecar-valgrind.XXXXXX)
PROJECT="$TMPDIR_ROOT/project"
mkdir -p "$PROJECT/.nbs/chat" "$PROJECT/.nbs/events"

# Create a chat file
"$BIN_DIR/nbs-chat" create "$PROJECT/.nbs/chat/test.chat"

# Create a fake nbs-ts session directory
SESSION_HANDLE="deadbeef"
SESSION_DIR="$HOME/.nbs-ts/sessions/$SESSION_HANDLE"
mkdir -p "$SESSION_DIR"

# Create output.log with some initial content (simulates a running session)
cat > "$SESSION_DIR/output.log" << 'CONTENT'
$ claude
╭──────────────────────────────────────╮
│ Claude Code                          │
╰──────────────────────────────────────╯

  ❯
CONTENT

# Create a fake input FIFO
mkfifo "$SESSION_DIR/input.fifo" 2>/dev/null || true

# Start a background process to keep the FIFO open for reading
# and to hold a PID for the session
(
    exec cat > /dev/null < "$SESSION_DIR/input.fifo" &
    # Keep running to simulate a live session
    sleep 120
) &
SESSION_PID=$!

# Write PID file
echo "$SESSION_PID" > "$SESSION_DIR/pid"

# Set up valgrind log
VALGRIND_LOG="$TMPDIR_ROOT/valgrind.log"

echo "Starting sidecar under valgrind (60s test)..."

# Run sidecar under valgrind
valgrind \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-fds=yes \
    --track-origins=yes \
    --error-exitcode=42 \
    --suppressions="$SUPP_FILE" \
    --log-file="$VALGRIND_LOG" \
    "$BIN_DIR/nbs-sidecar" \
        --handle=testbot \
        --root="$PROJECT" \
        --session="$SESSION_HANDLE" \
        2>"$TMPDIR_ROOT/sidecar-stderr.log" &
SIDECAR_PID=$!

echo "Sidecar PID: $SIDECAR_PID (valgrind)"

# Wait for sidecar to initialise
sleep 5

# Simulate workload
echo "Simulating workload..."

# Send 10 chat messages (triggers notification injection checks)
for i in $(seq 1 10); do
    "$BIN_DIR/nbs-chat" send "$PROJECT/.nbs/chat/test.chat" "tester" \
        "Test message $i from valgrind harness" 2>/dev/null || true
    sleep 1
done

# Publish 5 bus events (triggers event processing)
for i in $(seq 1 5); do
    "$BIN_DIR/nbs-bus" publish "$PROJECT/.nbs/events" "tester" \
        "test-event" "normal" "event payload $i" 2>/dev/null || true
    sleep 1
done

# Publish 2 chat-query events (triggers handle_query)
for i in $(seq 1 2); do
    "$BIN_DIR/nbs-bus" publish "$PROJECT/.nbs/events" "tester" \
        "chat-query" "normal" "@testbot query $i" 2>/dev/null || true
    sleep 2
done

# Publish 1 chat-mention event
"$BIN_DIR/nbs-bus" publish "$PROJECT/.nbs/events" "tester" \
    "chat-mention" "normal" "@testbot mentioned in chat" 2>/dev/null || true
sleep 2

# Let the sidecar process everything
echo "Waiting for processing (20s)..."
sleep 20

# Clean shutdown
echo "Sending SIGTERM..."
kill "$SIDECAR_PID" 2>/dev/null || true

# Wait for valgrind to finish (it takes extra time after SIGTERM)
# Exit code will be non-zero (143 = signal 15) — that's expected.
wait "$SIDECAR_PID" 2>/dev/null || true
unset SIDECAR_PID

# Kill the fake session
kill "$SESSION_PID" 2>/dev/null || true
wait "$SESSION_PID" 2>/dev/null || true
unset SESSION_PID

# Clean up fake session directory
rm -rf "$SESSION_DIR"

echo ""
echo "=== Valgrind Output ==="
cat "$VALGRIND_LOG"
echo ""

# Parse valgrind output
DEFINITELY_LOST=$(grep -oP 'definitely lost: \K[0-9,]+' "$VALGRIND_LOG" | tr -d ',')
INDIRECTLY_LOST=$(grep -oP 'indirectly lost: \K[0-9,]+' "$VALGRIND_LOG" | tr -d ',')
STILL_REACHABLE=$(grep -oP 'still reachable: \K[0-9,]+' "$VALGRIND_LOG" | tr -d ',')

DEFINITELY_LOST=${DEFINITELY_LOST:-0}
INDIRECTLY_LOST=${INDIRECTLY_LOST:-0}
STILL_REACHABLE=${STILL_REACHABLE:-0}

echo "=== Leak Summary ==="
echo "  Definitely lost: $DEFINITELY_LOST bytes"
echo "  Indirectly lost: $INDIRECTLY_LOST bytes"
echo "  Still reachable: $STILL_REACHABLE bytes"
echo ""

PASS=1
if [[ "$DEFINITELY_LOST" -ne 0 ]]; then
    echo "FAIL: definitely lost = $DEFINITELY_LOST bytes"
    PASS=0
fi
if [[ "$INDIRECTLY_LOST" -ne 0 ]]; then
    echo "FAIL: indirectly lost = $INDIRECTLY_LOST bytes"
    PASS=0
fi

if [[ "$PASS" -eq 1 ]]; then
    echo "PASS: zero definitely/indirectly lost bytes"
    exit 0
else
    echo "FAIL: memory leaks detected"
    exit 1
fi
