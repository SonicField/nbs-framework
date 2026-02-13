#!/bin/bash
# Run all NBS Framework tests
#
# Usage: ./tests/run_all.sh [--quick]
#   --quick: Skip slow tests (worker tests, AI evaluation tests)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUICK_MODE=false

for arg in "$@"; do
    case $arg in
        --quick) QUICK_MODE=true ;;
    esac
done

PASSED=0
FAILED=0
SKIPPED=0

run_test() {
    local test_script="$1"
    local name=$(basename "$test_script" .sh)

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

skip_test() {
    local name="$1"
    echo "--- $name ---"
    echo "SKIPPED (--quick mode)"
    SKIPPED=$((SKIPPED + 1))
    echo ""
}

echo "=== NBS Framework Test Suite ==="
echo ""

# Installation tests
run_test "$SCRIPT_DIR/automated/test_install.sh"
run_test "$SCRIPT_DIR/automated/test_install_paths.sh"
run_test "$SCRIPT_DIR/automated/test_home_validation.sh"

# pty-session tests (quick, deterministic)
run_test "$SCRIPT_DIR/automated/test_pty_session_lifecycle.sh"

# nbs-chat tests (deterministic)
run_test "$SCRIPT_DIR/automated/test_nbs_chat_lifecycle.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_chat_terminal.sh"

# nbs-bus tests (deterministic)
if [[ -f "$SCRIPT_DIR/automated/test_nbs_bus.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_nbs_bus.sh"
fi

# nbs-claude sidecar tests (deterministic)
run_test "$SCRIPT_DIR/automated/test_nbs_claude.sh"

# nbs-chat bus bridge tests (deterministic)
if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_bus.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_nbs_chat_bus.sh"
fi

# Slow tests (AI evaluation, workers)
if $QUICK_MODE; then
    skip_test "test_install_worker"
    skip_test "test_nbs_command"
    skip_test "test_nbs_discovery"
    skip_test "test_nbs_recovery"
    skip_test "test_control_inbox_ai"
    skip_test "test_poll_registry_ai"
    skip_test "test_nbs_chat_ai_integration"
else
    if [[ -f "$SCRIPT_DIR/automated/test_install_worker.sh" ]]; then
        run_test "$SCRIPT_DIR/automated/test_install_worker.sh"
    fi
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_command.sh" ]]; then
        run_test "$SCRIPT_DIR/automated/test_nbs_command.sh"
    fi
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_discovery.sh" ]]; then
        run_test "$SCRIPT_DIR/automated/test_nbs_discovery.sh"
    fi
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_recovery.sh" ]]; then
        run_test "$SCRIPT_DIR/automated/test_nbs_recovery.sh"
    fi
    run_test "$SCRIPT_DIR/automated/test_control_inbox_ai.sh"
    run_test "$SCRIPT_DIR/automated/test_poll_registry_ai.sh"
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_ai_integration.sh" ]]; then
        run_test "$SCRIPT_DIR/automated/test_nbs_chat_ai_integration.sh"
    fi
fi

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
