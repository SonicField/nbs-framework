#!/bin/bash
# Run all NBS Framework tests
#
# Usage: ./tests/run_all.sh [--quick]
#   --quick: Skip slow tests (worker tests, AI evaluation tests)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
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

run_unit_tests() {
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

    if [[ $unit_failed -eq 0 ]]; then
        PASSED=$((PASSED + 1))
    else
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

# --- Unit tests (fast, deterministic, no dependencies) ---
run_unit_tests

# --- Installation tests ---
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

# Multi-agent chat integration tests (deterministic, concurrent)
if [[ -f "$SCRIPT_DIR/automated/test_multi_agent_chat.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_multi_agent_chat.sh"
fi

# --- Audit fix verification tests ---
if [[ -f "$SCRIPT_DIR/automated/test_chat_main_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_chat_main_fixes.sh"
fi
if [[ -f "$SCRIPT_DIR/automated/test_worker_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_worker_fixes.sh"
fi
if [[ -f "$SCRIPT_DIR/automated/test_claude_remote_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_claude_remote_fixes.sh"
fi
if [[ -f "$SCRIPT_DIR/automated/test_chat_init_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_chat_init_fixes.sh"
fi
if [[ -f "$SCRIPT_DIR/automated/test_nbs_claude_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_nbs_claude_fixes.sh"
fi

# nbs-chat remote tests (requires ssh localhost)
if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_remote.sh" ]]; then
    if ssh -o BatchMode=yes -o ConnectTimeout=3 localhost true 2>/dev/null; then
        run_test "$SCRIPT_DIR/automated/test_nbs_chat_remote.sh"
    else
        skip_test "test_nbs_chat_remote (ssh localhost unavailable)"
    fi
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
    skip_test "test_nbs_chat_search_ai"
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
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_search_ai.sh" ]]; then
        run_test "$SCRIPT_DIR/automated/test_nbs_chat_search_ai.sh"
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
