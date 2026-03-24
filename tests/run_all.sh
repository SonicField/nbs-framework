#!/bin/bash
# Run all NBS Framework tests
#
# Usage: ./tests/run_all.sh [--quick] [--target=NAME]
#   --quick:        Skip slow tests (worker tests, AI evaluation tests)
#   --target=NAME:  Run only the test matching NAME (substring match)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
QUICK_MODE=false
TARGET=""

for arg in "$@"; do
    case $arg in
        --quick) QUICK_MODE=true ;;
        --target=*) TARGET="${arg#--target=}" ;;
    esac
done

PASSED=0
FAILED=0
SKIPPED=0

should_run() {
    [[ -z "$TARGET" ]] || [[ "$1" == *"$TARGET"* ]]
}

run_test() {
    local test_script="$1"
    local name=$(basename "$test_script" .sh)

    if ! should_run "$name"; then return; fi

    echo "--- $name ---"
    if "$test_script"; then
        echo "PASSED: $name"
        PASSED=$((PASSED + 1))
    else
        echo "FAILED: $name"
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

# Run AI tests via nbs-ts when inside Claude Code to avoid the
# nested session restriction. The test script runs unchanged in a
# fresh session with the user's full environment.
run_ai_test() {
    local test_script="$1"
    local name=$(basename "$test_script" .sh)

    if ! should_run "$name"; then return; fi

    echo "--- $name ---"
    if [[ -z "${CLAUDECODE:-}" ]]; then
        # Not inside Claude Code — run directly
        if "$test_script"; then
            echo "PASSED: $name"
            PASSED=$((PASSED + 1))
        else
            echo "FAILED: $name"
            FAILED=$((FAILED + 1))
        fi
    else
        # Inside Claude Code — route through nbs-ts
        local session="test-${name}-$$"
        local NBS_TS="${PROJECT_DIR}/bin/nbs-ts"
        if [[ ! -x "$NBS_TS" ]]; then
            echo "SKIPPED: nbs-ts not available"
            SKIPPED=$((SKIPPED + 1))
            echo ""
            return
        fi

        export NBS_PTY_QUIET=1
        # Write exit code to a temp file — avoids parsing terminal output
        # which contains ANSI escapes and command echo.
        local rc_file="/tmp/nbs-test-rc-${name}-$$"
        rm -f "$rc_file"
        "$NBS_TS" create "$session" bash 2>/dev/null
        # Unset TMUX so tests that use nbs-ts internally can create
        # their own sessions without tmux refusing to nest.
        "$NBS_TS" send "$session" "unset TMUX; $test_script; echo \$? > $rc_file; exit" 2>/dev/null
        # Wait for session to exit (read --wait blocks until process dies)
        "$NBS_TS" read "$session" --wait --timeout=300 2>/dev/null || true
        "$NBS_TS" kill "$session" 2>/dev/null || true
        local exit_code="1"
        if [[ -f "$rc_file" ]]; then
            exit_code=$(cat "$rc_file")
            rm -f "$rc_file"
        fi
        if [[ "$exit_code" == "0" ]]; then
            echo "PASSED: $name"
            PASSED=$((PASSED + 1))
        else
            echo "FAILED: $name"
            FAILED=$((FAILED + 1))
        fi
    fi
    echo ""
}

run_unit_tests() {
    if ! should_run "unit_tests"; then return; fi
    echo "--- C unit tests (make test-unit) ---"
    local unit_failed=0

    # Build and run bus unit tests
    echo "  Building nbs-bus unit tests..."
    if (cd "$PROJECT_DIR/src/nbs-bus" && make test-unit 2>&1); then
        echo "  PASSED: nbs-bus unit tests"
    else
        echo "  FAILED: nbs-bus unit tests"
        unit_failed=1
    fi

    # Build and run chat unit tests
    echo "  Building nbs-chat unit tests..."
    if (cd "$PROJECT_DIR/src/nbs-chat" && make test-unit 2>&1); then
        echo "  PASSED: nbs-chat unit tests"
    else
        echo "  FAILED: nbs-chat unit tests"
        unit_failed=1
    fi

    # Build and run sidecar unit tests
    echo "  Building nbs-sidecar unit tests..."
    if (cd "$PROJECT_DIR/src/nbs-sidecar" && make test-unit 2>&1); then
        echo "  PASSED: nbs-sidecar unit tests"
    else
        echo "  FAILED: nbs-sidecar unit tests"
        unit_failed=1
    fi

    # Build and run workers unit tests
    echo "  Building nbs-workers unit tests..."
    if (cd "$PROJECT_DIR/src/nbs-workers" && make test-unit 2>&1); then
        echo "  PASSED: nbs-workers unit tests"
    else
        echo "  FAILED: nbs-workers unit tests"
        unit_failed=1
    fi

    if [[ $unit_failed -eq 0 ]]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

skip_test() {
    local name="$1"
    if ! should_run "$name"; then return; fi
    echo "--- $name ---"
    echo "SKIPPED (--quick mode)"
    SKIPPED=$((SKIPPED + 1))
    echo ""
}

echo "=== NBS Framework Test Suite ==="
echo ""

# --- Unit tests (fast, deterministic, no dependencies) ---
run_unit_tests

# --- Auto-discovery of shell tests ---
# All test_*.sh files in automated/ are discovered automatically.
# New tests are included by default — no need to edit this file.
# Tests requiring AI/Claude are routed through run_ai_test when not in --quick mode.

# Tests that require AI (Claude) — skipped with --quick, routed through
# run_ai_test (nbs-ts wrapper) when running inside Claude Code.
AI_TESTS="
test_install_worker
test_nbs_command
test_nbs_discovery
test_nbs_recovery
test_control_inbox_ai
test_poll_registry_ai
test_nbs_chat_ai_integration
test_nbs_chat_search_ai
test_worker_spawn_survival
test_pythia_ai
test_pythia_adversarial
test_pythia_adv_no_chat
test_scribe_ai
test_scribe_log_ai
"

is_ai_test() {
    echo "$AI_TESTS" | grep -qw "$1"
}

# Special-case: nbs-chat remote requires ssh localhost or mock server
run_remote_test() {
    local test_file="$1"
    if ! should_run "test_nbs_chat_remote"; then return; fi
    if ssh -o BatchMode=yes -o ConnectTimeout=3 localhost true 2>/dev/null; then
        run_test "$test_file"
    elif [[ -x "$HOME/local/nbs-ssh/venv/bin/python" ]]; then
        echo "--- test_nbs_chat_remote (via mock SSH) ---"
        if "$HOME/local/nbs-ssh/venv/bin/python" "$SCRIPT_DIR/automated/test_nbs_chat_remote_mock.py"; then
            echo "PASSED: test_nbs_chat_remote"
            PASSED=$((PASSED + 1))
        else
            echo "FAILED: test_nbs_chat_remote"
            FAILED=$((FAILED + 1))
        fi
        echo ""
    else
        skip_test "test_nbs_chat_remote (ssh localhost unavailable, no mock server)"
    fi
}

# Discover and run all test_*.sh files in automated/
for test_file in "$SCRIPT_DIR"/automated/test_*.sh; do
    [[ -f "$test_file" ]] || continue
    test_name=$(basename "$test_file" .sh)

    # Special case: remote test needs ssh handling
    if [[ "$test_name" == "test_nbs_chat_remote" ]]; then
        run_remote_test "$test_file"
        continue
    fi

    # AI tests: skip in --quick mode, route through run_ai_test otherwise
    if is_ai_test "$test_name"; then
        if $QUICK_MODE; then
            skip_test "$test_name"
        else
            run_ai_test "$test_file"
        fi
        continue
    fi

    # Regular deterministic test
    run_test "$test_file"
done

echo "=== Summary ==="
echo "Passed:  $PASSED"
echo "Failed:  $FAILED"
echo "Skipped: $SKIPPED"
echo ""

if [[ $FAILED -gt 0 ]]; then
    echo "=== TESTS FAILED ==="
    exit 1
else
    echo "=== ALL TESTS PASSED ==="
    exit 0
fi
