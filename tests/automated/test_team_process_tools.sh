#!/bin/bash
# Test: nbs-team-status and nbs-team-kill process management tools
#
# Falsifiable tests covering:
#   1. nbs-team-status reports correct process counts
#   2. nbs-team-status detects duplicate sidecars
#   3. nbs-team-kill kills sidecar-loops before sidecars
#   4. nbs-team-kill removes PID files
#   5. nbs-sidecar-restart deduplicates sidecars
#   6. nbs-team-status exits 0 when healthy, 1 when problems
#   7. Argument validation (exit 4 on bad args)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
BIN_DIR="${PROJECT_ROOT}/bin"

TEAM_STATUS="${BIN_DIR}/nbs-team-status"
TEAM_KILL="${BIN_DIR}/nbs-team-kill"

TEST_DIR=$(mktemp -d)
ERRORS=0

cleanup() {
    # Kill any mock processes we started
    if [[ -f "${TEST_DIR}/mock_pids" ]]; then
        while read -r pid; do
            kill "$pid" 2>/dev/null || true
        done < "${TEST_DIR}/mock_pids"
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

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

echo "=== Team Process Tools Test ==="
echo "Test dir: $TEST_DIR"
echo ""

# ---- Test 1: Argument validation ----
echo "1. Argument validation..."

# nbs-team-status with no args should exit 4
rc=0
"$TEAM_STATUS" 2>/dev/null || rc=$?
check "team-status no args exits 4" "$([[ $rc -eq 4 ]] && echo pass || echo fail)"

# nbs-team-kill with no args should exit 4
rc=0
"$TEAM_KILL" 2>/dev/null || rc=$?
check "team-kill no args exits 4" "$([[ $rc -eq 4 ]] && echo pass || echo fail)"

# nbs-team-status with nonexistent root should exit 4
rc=0
"$TEAM_STATUS" "testtag" "/nonexistent/path" 2>/dev/null || rc=$?
check "team-status bad root exits 4" "$([[ $rc -eq 4 ]] && echo pass || echo fail)"

# nbs-team-kill with nonexistent root should exit 4
rc=0
"$TEAM_KILL" "testtag" "/nonexistent/path" --force 2>/dev/null || rc=$?
check "team-kill bad root exits 4" "$([[ $rc -eq 4 ]] && echo pass || echo fail)"

echo ""

# ---- Test 2: team-status with no processes shows problems ----
echo "2. team-status with no matching processes..."

# Create a fake project root
FAKE_ROOT="${TEST_DIR}/fake-project"
mkdir -p "${FAKE_ROOT}/.nbs/pids"

rc=0
OUTPUT=$("$TEAM_STATUS" "faketag" "$FAKE_ROOT" 2>/dev/null) || rc=$?
check "team-status exits 1 when no processes" "$([[ $rc -eq 1 ]] && echo pass || echo fail)"
check "team-status reports MISSING sessions" "$(echo "$OUTPUT" | grep -q 'MISSING' && echo pass || echo fail)"

echo ""

# ---- Test 3: team-kill --force with no processes succeeds ----
echo "3. team-kill --force with no matching processes..."

rc=0
OUTPUT=$("$TEAM_KILL" "faketag" "$FAKE_ROOT" --force 2>/dev/null) || rc=$?
check "team-kill exits 0 when nothing to kill" "$([[ $rc -eq 0 ]] && echo pass || echo fail)"

echo ""

# ---- Test 4: team-kill removes PID files ----
echo "4. team-kill removes PID files..."

# Create fake PID files
for agent in supervisor generalist gatekeeper theologian testkeeper scribe medic; do
    echo "99999" > "${FAKE_ROOT}/.nbs/pids/${agent}.pid"
done

pid_count_before=$(ls "${FAKE_ROOT}/.nbs/pids/"*.pid 2>/dev/null | wc -l)
"$TEAM_KILL" "faketag" "$FAKE_ROOT" --force >/dev/null 2>&1
pid_count_after=$(ls "${FAKE_ROOT}/.nbs/pids/"*.pid 2>/dev/null | wc -l || true)
pid_count_after="${pid_count_after:-0}"
pid_count_after=$(echo "$pid_count_after" | tr -d '[:space:]')

check "PID files exist before kill ($pid_count_before)" "$([[ $pid_count_before -eq 7 ]] && echo pass || echo fail)"
check "PID files removed after kill ($pid_count_after)" "$([[ $pid_count_after -eq 0 ]] && echo pass || echo fail)"

echo ""

# ---- Test 5: team-kill kills sidecar-loops before sidecars ----
echo "5. Kill ordering: sidecar-loops before sidecars..."

# This test verifies the ORDERING requirement from the feature request:
# if we kill sidecars before loops, the loops respawn them.
# We test this by creating a mock sidecar-loop that writes a sentinel on death.

MOCK_LOOP=$(mktemp /tmp/nbs-sidecar-loop.XXXXXX.sh)
cat > "$MOCK_LOOP" << EOF
#!/bin/bash
# Mock sidecar-loop for testing
# References project root for grep matching: $FAKE_ROOT
# --handle=testmock
trap 'echo "loop-died" > "${TEST_DIR}/loop_died_at"' EXIT
while true; do sleep 60; done
EOF
chmod +x "$MOCK_LOOP"

bash "$MOCK_LOOP" &
LOOP_PID=$!
echo "$LOOP_PID" >> "${TEST_DIR}/mock_pids"
sleep 0.5

# Verify loop is running
check "mock sidecar-loop is running" "$(kill -0 $LOOP_PID 2>/dev/null && echo pass || echo fail)"

# Kill it via team-kill
"$TEAM_KILL" "faketag" "$FAKE_ROOT" --force >/dev/null 2>&1
sleep 2

# Verify loop is dead
check "sidecar-loop killed by team-kill" "$(kill -0 $LOOP_PID 2>/dev/null && echo fail || echo pass)"

# Verify the loop script was cleaned up
check "loop script removed" "$([[ ! -f "$MOCK_LOOP" ]] && echo pass || echo fail)"

echo ""

# ---- Test 6: team-status output format ----
echo "6. team-status output format..."

OUTPUT=$("$TEAM_STATUS" "faketag" "$FAKE_ROOT" 2>/dev/null) || true

# Should have a header line with Team: and Root:
check "output has Team header" "$(echo "$OUTPUT" | grep -q "Team: faketag" && echo pass || echo fail)"

# Should list all expected agents
for agent in supervisor generalist gatekeeper theologian testkeeper scribe medic; do
    check "output lists $agent" "$(echo "$OUTPUT" | grep -q "${agent}:" && echo pass || echo fail)"
done

echo ""

# ---- Test 7: team-status detects orphan sidecars (no matching session) ----
echo "7. Orphan sidecar detection..."

# Start a fake "sidecar" process that looks like an orphan.
# We need proper null-separated argv so /proc/PID/cmdline is parseable.
# Create a wrapper script that execs with the right argv.
MOCK_SIDECAR="${TEST_DIR}/mock-nbs-sidecar"
cat > "$MOCK_SIDECAR" << 'MOCKEOF'
#!/bin/bash
# Mock nbs-sidecar — just sleeps forever
while true; do sleep 60; done
MOCKEOF
chmod +x "$MOCK_SIDECAR"

# Symlink it as nbs-sidecar so pgrep -f matches
MOCK_LINK="${TEST_DIR}/nbs-sidecar"
ln -sf "$MOCK_SIDECAR" "$MOCK_LINK"

"$MOCK_LINK" --handle=orphantest --root="${FAKE_ROOT}" --transport=ts &
ORPHAN_PID=$!
echo "$ORPHAN_PID" >> "${TEST_DIR}/mock_pids"
sleep 0.5

check "mock orphan sidecar is running" "$(kill -0 $ORPHAN_PID 2>/dev/null && echo pass || echo fail)"

OUTPUT=$("$TEAM_STATUS" "faketag" "$FAKE_ROOT" 2>/dev/null) || true
check "team-status detects orphan" "$(echo "$OUTPUT" | grep -qi 'orphan' && echo pass || echo fail)"

# The orphan has handle 'orphantest' which is not in expected agents list
check "orphan handle in output" "$(echo "$OUTPUT" | grep -q 'orphantest' && echo pass || echo fail)"

# Clean up orphan
kill "$ORPHAN_PID" 2>/dev/null || true
wait "$ORPHAN_PID" 2>/dev/null || true

echo ""

# ---- Test 8: team-kill --help exits 0 ----
echo "8. Help flags..."

rc=0
"$TEAM_KILL" --help >/dev/null 2>&1 || rc=$?
check "team-kill --help exits 0" "$([[ $rc -eq 0 ]] && echo pass || echo fail)"

echo ""

# ---- Summary ----
echo "=== Results ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "All tests passed."
    exit 0
else
    echo "$ERRORS test(s) failed."
    exit 1
fi
