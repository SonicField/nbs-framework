#!/bin/bash
# Test: nbs-sidecar-lib.sh — unit tests for the sidecar library
#
# Each test sources the library and calls a function with known inputs.
# Tests run against the real system where possible (live processes,
# real nbs-ts sessions) and against synthetic data where needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
LIB="${PROJECT_ROOT}/bin/nbs-sidecar-lib.sh"

ERRORS=0
PASS=0

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

echo "=== nbs-sidecar-lib.sh Unit Tests ==="
echo ""

# --- Test 0: Library exists and sources cleanly ---
echo "0. Library sources without error..."
check "library exists" "$( [[ -f "$LIB" ]] && echo pass || echo fail )"
check "library sources" "$( source "$LIB" 2>/dev/null && echo pass || echo fail )"
echo ""

# Source it for subsequent tests
source "$LIB" 2>/dev/null || { echo "Cannot source library — aborting"; exit 1; }

# --- Test 1: nbs_sc_cmdline_has ---
echo "1. nbs_sc_cmdline_has..."
# Test against our own process (PID $$) — bash's cmdline contains 'bash'
check "finds own process name" "$( nbs_sc_cmdline_has $$ "bash" && echo pass || echo fail )"
check "rejects missing flag" "$( nbs_sc_cmdline_has $$ "NONEXISTENT_FLAG_xyz" && echo fail || echo pass )"
check "rejects dead PID" "$( nbs_sc_cmdline_has 99999999 "anything" && echo fail || echo pass )"
echo ""

# --- Test 2: nbs_sc_extract_handle ---
echo "2. nbs_sc_extract_handle..."
check "supervisor from nbs-supervisor-phoenix" "$( [[ $(nbs_sc_extract_handle 'nbs-supervisor-phoenix') == 'supervisor' ]] && echo pass || echo fail )"
check "scribe from nbs-scribe-vib-jit" "$( [[ $(nbs_sc_extract_handle 'nbs-scribe-vib-jit') == 'scribe' ]] && echo pass || echo fail )"
check "gatekeeper from nbs-gatekeeper-nbsterm" "$( [[ $(nbs_sc_extract_handle 'nbs-gatekeeper-nbsterm') == 'gatekeeper' ]] && echo pass || echo fail )"
check "empty on bad input" "$( [[ -z $(nbs_sc_extract_handle 'random-string') ]] && echo pass || echo fail )"
check "empty on empty input" "$( [[ -z $(nbs_sc_extract_handle '') ]] && echo pass || echo fail )"
echo ""

# --- Test 3: nbs_sc_find_session ---
echo "3. nbs_sc_find_session..."
# This tests against live nbs-ts sessions if any exist
live_session=$(nbs-ts list 2>/dev/null | grep alive | head -1 | awk '{print $3}')
if [[ -n "$live_session" ]]; then
    live_id=$(nbs-ts list 2>/dev/null | grep alive | head -1 | awk '{print $1}')
    found=$(nbs_sc_find_session "$live_session")
    check "finds live session '$live_session'" "$( [[ "$found" == "$live_id" ]] && echo pass || echo fail )"
else
    echo "   SKIP: no live sessions to test against"
fi
check "returns empty for nonexistent" "$( [[ -z $(nbs_sc_find_session 'nbs-NONEXISTENT-session-xyz') ]] && echo pass || echo fail )"
echo ""

# --- Test 4: nbs_sc_clean_pid ---
echo "4. nbs_sc_clean_pid..."
TEST_DIR=$(mktemp -d)
mkdir -p "$TEST_DIR/.nbs/pids"
echo "12345" > "$TEST_DIR/.nbs/pids/sidecar-testhandle.pid"
check "pid file exists before clean" "$( [[ -f "$TEST_DIR/.nbs/pids/sidecar-testhandle.pid" ]] && echo pass || echo fail )"
nbs_sc_clean_pid "testhandle" "$TEST_DIR"
check "pid file gone after clean" "$( [[ ! -f "$TEST_DIR/.nbs/pids/sidecar-testhandle.pid" ]] && echo pass || echo fail )"
# Clean nonexistent — should not error
nbs_sc_clean_pid "nonexistent" "$TEST_DIR"
check "clean nonexistent no error" "pass"
rm -rf "$TEST_DIR"
echo ""

# --- Test 5: nbs_sc_is_infrastructure ---
echo "5. nbs_sc_is_infrastructure..."
check "pythia is infra" "$( nbs_sc_is_infrastructure 'pythia' && echo pass || echo fail )"
check "shepard is infra" "$( nbs_sc_is_infrastructure 'shepard' && echo pass || echo fail )"
check "fixup is infra" "$( nbs_sc_is_infrastructure 'fixup' && echo pass || echo fail )"
check "librarian is infra" "$( nbs_sc_is_infrastructure 'librarian' && echo pass || echo fail )"
check "chatdigest is infra" "$( nbs_sc_is_infrastructure 'chatdigest' && echo pass || echo fail )"
check "supervisor is NOT infra" "$( nbs_sc_is_infrastructure 'supervisor' && echo fail || echo pass )"
check "scribe is NOT infra" "$( nbs_sc_is_infrastructure 'scribe' && echo fail || echo pass )"
check "medic is NOT infra" "$( nbs_sc_is_infrastructure 'medic' && echo fail || echo pass )"
echo ""

# --- Test 6: nbs_sc_has_loop ---
echo "6. nbs_sc_has_loop..."
# PID 1 (init/systemd) does not have a sidecar-loop parent
check "PID 1 has no loop" "$( nbs_sc_has_loop 1 && echo fail || echo pass )"
check "dead PID has no loop" "$( nbs_sc_has_loop 99999999 && echo fail || echo pass )"
echo ""

# --- Test 7: nbs_sc_generate_loop ---
echo "7. nbs_sc_generate_loop..."
TEST_DIR2=$(mktemp -d)
script=$(nbs_sc_generate_loop \
    --sidecar-bin=/usr/bin/false \
    --handle=testhandle \
    --root="$TEST_DIR2" \
    --session=abc123 \
    --session-name=nbs-testhandle-test \
    --log="$TEST_DIR2/test.log")
check "generates a script file" "$( [[ -f "$script" ]] && echo pass || echo fail )"
check "script is executable" "$( [[ -x "$script" ]] && echo pass || echo fail )"
check "script has CURRENT_SESSION" "$( grep -q 'CURRENT_SESSION=' "$script" && echo pass || echo fail )"
check "script has correct session value" "$( grep -q 'CURRENT_SESSION="abc123"' "$script" && echo pass || echo fail )"
check "script has trap for cleanup" "$( grep -q 'trap' "$script" && echo pass || echo fail )"
check "script has log function" "$( grep -q '^log()' "$script" && echo pass || echo fail )"
check "script has session re-discovery" "$( grep -q 'nbs-sidecar-find-session' "$script" && echo pass || echo fail )"
check "script uses exact session name" "$( grep -q 'nbs-testhandle-test' "$script" && echo pass || echo fail )"
check "--session not in printf %q args" "$( ! grep -q "printf.*session" "$script" && echo pass || echo fail )"
check "session passed as variable" "$( grep -q -- '--session="\$CURRENT_SESSION"' "$script" && echo pass || echo fail )"
# Verify no escaped dollar
check "no escaped dollar in session" "$( ! grep -q '\\$CURRENT_SESSION' "$script" && echo pass || echo fail )"
rm -f "$script"
rm -rf "$TEST_DIR2"
echo ""

# --- Summary ---
echo "=== Result ==="
echo "$PASS passed, $ERRORS failed"
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
