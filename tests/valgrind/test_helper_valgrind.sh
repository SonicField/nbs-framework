#!/bin/bash
# test_helper_valgrind.sh — Run nbs-ts-helper under valgrind.
#
# Starts the helper under valgrind, creates sessions via socket,
# exercises create/kill cycles, then checks for leaks.
#
# Exit: 0 if no leaks, 1 if leaks detected.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="$REPO_ROOT/bin"
SUPP_FILE="$SCRIPT_DIR/nbs.supp"
VALGRIND_LOG=""
TMPDIR_ROOT=""
HELPER_PID=""

cleanup() {
    local exit_code=$?
    # Kill helper
    if [[ -n "${HELPER_PID:-}" ]] && kill -0 "$HELPER_PID" 2>/dev/null; then
        kill "$HELPER_PID" 2>/dev/null || true
        wait "$HELPER_PID" 2>/dev/null || true
    fi
    # Kill any test sessions
    for pid_file in "$HOME"/.nbs-ts/sessions/*/pid; do
        [[ -f "$pid_file" ]] || continue
        local pid
        pid=$(cat "$pid_file" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    # Clean up temp dir
    if [[ -n "$TMPDIR_ROOT" && -d "$TMPDIR_ROOT" ]]; then
        rm -rf "$TMPDIR_ROOT"
    fi
    exit "$exit_code"
}
trap cleanup EXIT

# Verify prerequisites
for cmd in valgrind "$BIN_DIR/nbs-ts-helper" "$BIN_DIR/nbs-ts"; do
    if [[ ! -x "$cmd" ]] && ! command -v "$cmd" &>/dev/null; then
        echo "FAIL: required command not found: $cmd" >&2
        exit 1
    fi
done

echo "=== nbs-ts-helper valgrind test ==="

TMPDIR_ROOT=$(mktemp -d /tmp/nbs-helper-valgrind.XXXXXX)
VALGRIND_LOG="$TMPDIR_ROOT/valgrind.log"

# Kill any existing helper
EXISTING_SOCK="$HOME/.nbs-ts/helper.sock"
if [[ -S "$EXISTING_SOCK" ]]; then
    echo "Warning: existing helper socket found. A helper may be running."
    echo "This test needs exclusive access. Skipping if helper is alive."
    if socat -u /dev/null "UNIX-CONNECT:$EXISTING_SOCK" 2>/dev/null; then
        echo "SKIP: helper already running. Kill it first."
        exit 0
    fi
    rm -f "$EXISTING_SOCK"
fi

echo "Starting helper under valgrind (60s test)..."

# Run helper under valgrind
valgrind \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-fds=yes \
    --track-origins=yes \
    --error-exitcode=42 \
    --suppressions="$SUPP_FILE" \
    --log-file="$VALGRIND_LOG" \
    "$BIN_DIR/nbs-ts-helper" \
        > "$TMPDIR_ROOT/helper-stdout.log" \
        2>"$TMPDIR_ROOT/helper-stderr.log" &
HELPER_PID=$!

echo "Helper PID: $HELPER_PID (valgrind)"

# Wait for helper to start listening
sleep 3

# Verify socket exists
if [[ ! -S "$HOME/.nbs-ts/helper.sock" ]]; then
    echo "FAIL: helper socket not created after 3s"
    exit 1
fi

echo "Simulating workload..."

CREATED_SESSIONS=()

# Create 5 sessions
for i in $(seq 1 5); do
    HANDLE=$("$BIN_DIR/nbs-ts" create --name="valgrind-test-$i" "sleep 300" 2>/dev/null || true)
    if [[ -n "$HANDLE" ]]; then
        CREATED_SESSIONS+=("$HANDLE")
        echo "  Created session $i: $HANDLE"
    else
        echo "  Warning: failed to create session $i"
    fi
    sleep 1
done

# Send commands to sessions
for handle in "${CREATED_SESSIONS[@]}"; do
    "$BIN_DIR/nbs-ts" send "$handle" "echo hello from valgrind" 2>/dev/null || true
    sleep 0.5
done

# Read output from sessions
for handle in "${CREATED_SESSIONS[@]}"; do
    "$BIN_DIR/nbs-ts" read-new "$handle" --strip 2>/dev/null > /dev/null || true
done

# Kill 3 sessions
for i in $(seq 0 2); do
    if [[ $i -lt ${#CREATED_SESSIONS[@]} ]]; then
        echo "  Killing session: ${CREATED_SESSIONS[$i]}"
        "$BIN_DIR/nbs-ts" kill "${CREATED_SESSIONS[$i]}" 2>/dev/null || true
        sleep 1
    fi
done

# Create 2 more sessions (tests post-cleanup allocation)
for i in $(seq 6 7); do
    HANDLE=$("$BIN_DIR/nbs-ts" create --name="valgrind-test-$i" "sleep 300" 2>/dev/null || true)
    if [[ -n "$HANDLE" ]]; then
        CREATED_SESSIONS+=("$HANDLE")
        echo "  Created session $i: $HANDLE"
    else
        echo "  Warning: failed to create session $i"
    fi
    sleep 1
done

# Let the helper process reaping
echo "Waiting for reaping (10s)..."
sleep 10

# Clean shutdown
echo "Sending SIGTERM to helper..."
kill "$HELPER_PID" 2>/dev/null || true
# Wait for valgrind to finish — exit code may be non-zero (expected).
wait "$HELPER_PID" 2>/dev/null || true
unset HELPER_PID

# Clean up any remaining test sessions
for handle in "${CREATED_SESSIONS[@]}"; do
    "$BIN_DIR/nbs-ts" kill "$handle" 2>/dev/null || true
done

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
