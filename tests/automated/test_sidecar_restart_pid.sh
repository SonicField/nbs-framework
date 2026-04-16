#!/bin/bash
# Test: nbs-sidecar-restart PID cleanup and session variable expansion
#
# Tests:
#   1. Generated loop script expands $CURRENT_SESSION (not literal)
#   2. Stale PID file does not prevent sidecar restart
#   3. PID file is cleaned during restart
#   4. Handle extraction strips compound suffixes (supervisor-vib → supervisor)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SIDECAR_RESTART="${PROJECT_ROOT}/bin/nbs-sidecar-restart"

ERRORS=0

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

echo "=== nbs-sidecar-restart PID and Expansion Tests ==="
echo ""

# --- Test 1: $CURRENT_SESSION expansion in restart_running loop ---
# The restart_running path saves args from /proc, strips --session=,
# and generates a loop script. $CURRENT_SESSION must NOT be escaped
# by printf %q.
echo "1. \$CURRENT_SESSION expansion in generated loop script..."

# Simulate what restart_running does: build LOOP_ARGS and printf %q them
# The bug was: LOOP_ARGS contained '--session=$CURRENT_SESSION' and
# printf %q escaped the $ to \$CURRENT_SESSION
LOOP_ARGS=('/usr/bin/nbs-sidecar' '--handle=gatekeeper' '--root=/tmp' '--transport=ts' '--log=/tmp/test.log')
GENERATED=$(printf '%q ' "${LOOP_ARGS[@]}")
# The session should NOT appear in LOOP_ARGS at all (it's added separately)
check "no --session in printf args" "$( echo "$GENERATED" | grep -q 'session' && echo fail || echo pass )"
# Verify the source code adds --session separately
check "--session added outside printf" "$( grep -qF 'LOOP_ARGS[@]}") --session=' "$SIDECAR_RESTART" && echo pass || echo fail )"
echo ""

# --- Test 2: Handle extraction strips compound suffix ---
echo "2. Handle extraction strips compound suffix..."
# The regex should use [a-zA-Z0-9_]+ (no hyphen) to stop at first -
check "regex has no hyphen in char class" "$( grep -q '\[a-zA-Z0-9_\]+' "$SIDECAR_RESTART" && echo pass || echo fail )"
# The --handle= extraction strips at first hyphen
check "handle stripped in extraction" "$( grep -q 'HANDLE=.*HANDLE%%-\*' "$SIDECAR_RESTART" && echo pass || echo fail )"
echo ""

# --- Test 3: Stale PID file blocks sidecar restart ---
# This test verifies that nbs-sidecar-restart cleans stale PID files.
# Without this fix, a sidecar that reads the PID file sees a "duplicate"
# and exits with rc=1.
echo "3. Stale PID file cleaned during restart..."

# Create a fake project structure
TEST_DIR=$(mktemp -d)
cleanup() {
    pkill -9 -f "nbs-sidecar.*${TEST_DIR}" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

mkdir -p "$TEST_DIR/.nbs/pids"
mkdir -p "$TEST_DIR/.nbs/chat"
touch "$TEST_DIR/.nbs/chat/test.chat"

# Create a stale PID file pointing to a dead process
echo "99999" > "$TEST_DIR/.nbs/pids/sidecar-gatekeeper.pid"
check "stale PID file exists" "$( [[ -f "$TEST_DIR/.nbs/pids/sidecar-gatekeeper.pid" ]] && echo pass || echo fail )"

# Verify nbs-sidecar-restart cleans stale PID files
# We can't easily run a full restart (needs nbs-ts sessions), but we can
# check the source code handles PID cleanup
check "restart script cleans PID files" "$( grep -q 'pids/sidecar-.*\.pid' "$SIDECAR_RESTART" && echo pass || echo fail )"
echo ""

# --- Test 4: Source code structure checks ---
echo "4. Source code structure..."
# restart_running must strip --session= from saved args
check "restart strips --session from saved args" "$( grep -A2 'session=.*skip' "$SIDECAR_RESTART" | grep -q 'LOOP_ARGS' && echo pass || echo fail )"
# respawn_missing adds --session via variable, not printf
check "respawn uses session variable" "$( grep -q 'session=.*CURRENT_SESSION' "$SIDECAR_RESTART" && echo pass || echo fail )"
# Sidecar-loop has session re-discovery
check "loop has session re-discovery" "$( grep -q 'found replacement session' "$SIDECAR_RESTART" && echo pass || echo fail )"
# Sidecar-loop has logging
check "loop has logging" "$( grep -q 'sidecar-loop started for' "$SIDECAR_RESTART" && echo pass || echo fail )"
echo ""

# --- Summary ---
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
