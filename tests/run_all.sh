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

# --- Installation tests ---
run_test "$SCRIPT_DIR/automated/test_install.sh"
run_test "$SCRIPT_DIR/automated/test_install_paths.sh"
run_test "$SCRIPT_DIR/automated/test_home_validation.sh"

# nbs-ts (pty-session) tests (quick, deterministic)
run_test "$SCRIPT_DIR/automated/test_pty_session_lifecycle.sh"

# nbs-chat tests (deterministic)
run_test "$SCRIPT_DIR/automated/test_nbs_chat_lifecycle.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_chat_terminal.sh"

# nbs-bus tests (deterministic)
if [[ -f "$SCRIPT_DIR/automated/test_nbs_bus.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_nbs_bus.sh"
fi

# nbs-claude sidecar tests — polling/dialogue detection tested by C unit tests

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
if [[ -f "$SCRIPT_DIR/automated/test_claude_remote_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_claude_remote_fixes.sh"
fi
if [[ -f "$SCRIPT_DIR/automated/test_chat_init_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_chat_init_fixes.sh"
fi
if [[ -f "$SCRIPT_DIR/automated/test_nbs_claude_fixes.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_nbs_claude_fixes.sh"
fi

# --- Previously orphaned tests (added by testkeeper audit 2026-03-02) ---

# nbs-ts (pty-session) adversarial and feature tests
run_test "$SCRIPT_DIR/automated/test_pty_session_adversarial.sh"
run_test "$SCRIPT_DIR/automated/test_pty_session_adv_invalid.sh"
run_test "$SCRIPT_DIR/automated/test_pty_session_adv_no_collision.sh"
run_test "$SCRIPT_DIR/automated/test_pty_session_last.sh"
run_test "$SCRIPT_DIR/automated/test_pty_session_lock.sh"
run_test "$SCRIPT_DIR/automated/test_pty_session_timeout.sh"
run_test "$SCRIPT_DIR/automated/test_pty_session_wait_fallback.sh"

# nbs-chat additional tests
run_test "$SCRIPT_DIR/automated/test_nbs_chat_gaps.sh"
# test_nbs_chat_web removed — web client archived
run_test "$SCRIPT_DIR/automated/test_auto_archive.sh"

# nbs-claude additional tests
run_test "$SCRIPT_DIR/automated/test_nbs_claude_args.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_claude_audit_v17.sh"
# test_nbs_prompts removed — standup system deleted

# nbs-remote tests
run_test "$SCRIPT_DIR/automated/test_nbs_remote_build.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_remote_diff_status.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_remote_edit_static.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_remote_read.sh"
if [[ -f "$SCRIPT_DIR/automated/test_claude_remote_audit_v2.sh" ]]; then
    run_test "$SCRIPT_DIR/automated/test_claude_remote_audit_v2.sh"
fi

# nbs-scribe tests
run_test "$SCRIPT_DIR/automated/test_nbs_scribe_log.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_scribe_log_audit.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_scribe_query.sh"
run_test "$SCRIPT_DIR/automated/test_scribe_adversarial.sh"
run_test "$SCRIPT_DIR/automated/test_scribe_adv_no_noise.sh"

# Installation and integration tests
run_test "$SCRIPT_DIR/automated/test_install_audit.sh"
run_test "$SCRIPT_DIR/automated/test_tripod_integration.sh"

# Base64 assertion tests
run_test "$SCRIPT_DIR/automated/test_base64_assertions.sh"

# Sidecar tests
run_test "$SCRIPT_DIR/automated/test_sidecar_restart_fixes.sh"
run_test "$SCRIPT_DIR/automated/test_supervisor_adv_no_old_pattern.sh"

# Worker tests
run_test "$SCRIPT_DIR/automated/test_worker_adv_no_raw_log.sh"

# Digest and librarian tests
run_test "$SCRIPT_DIR/automated/test_digest_spawn_fixes.sh"
run_test "$SCRIPT_DIR/automated/test_librarian.sh"

# Investigation tests
run_test "$SCRIPT_DIR/automated/test_investigation_adversarial.sh"
run_test "$SCRIPT_DIR/automated/test_investigation_adv_no_normal.sh"
run_test "$SCRIPT_DIR/automated/test_investigation_adv_no_silent.sh"
run_test "$SCRIPT_DIR/automated/test_investigation_ask.sh"
run_test "$SCRIPT_DIR/automated/test_investigation_branch.sh"
run_test "$SCRIPT_DIR/automated/test_investigation_dispatch.sh"
run_test "$SCRIPT_DIR/automated/test_investigation_file.sh"

# Integration tests (comprehensive, 122 sub-tests)
run_test "$SCRIPT_DIR/automated/test_integration.sh"

# nbs-ts tests (session management)
run_test "$SCRIPT_DIR/automated/test_nbs_ts_lifecycle.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_cleanup.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_status.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_read.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_wait.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_oneshot.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_sigkill.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_completion.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_completion_edge.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_transport.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_adversarial.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_adversarial_fifo.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_no_tmux.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_restart.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_remote.sh"
run_test "$SCRIPT_DIR/automated/test_nbs_ts_worker.sh"

# nbs-chat remote tests (requires ssh localhost or mock server)
if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_remote.sh" ]] && should_run "test_nbs_chat_remote"; then
    if ssh -o BatchMode=yes -o ConnectTimeout=3 localhost true 2>/dev/null; then
        run_test "$SCRIPT_DIR/automated/test_nbs_chat_remote.sh"
    elif [[ -x "$HOME/local/nbs-ssh/venv/bin/python" ]]; then
        # ssh localhost blocked — use mock SSH server
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
    skip_test "test_worker_spawn_survival"
else
    if [[ -f "$SCRIPT_DIR/automated/test_install_worker.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_install_worker.sh"
    fi
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_command.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_nbs_command.sh"
    fi
    # test_nbs_discovery: AI evaluator output is non-deterministic (sometimes
    # produces conversation instead of JSON verdict). Accepted as flaky.
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_discovery.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_nbs_discovery.sh"
    fi
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_recovery.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_nbs_recovery.sh"
    fi
    run_ai_test "$SCRIPT_DIR/automated/test_control_inbox_ai.sh"
    run_ai_test "$SCRIPT_DIR/automated/test_poll_registry_ai.sh"
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_ai_integration.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_nbs_chat_ai_integration.sh"
    fi
    if [[ -f "$SCRIPT_DIR/automated/test_nbs_chat_search_ai.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_nbs_chat_search_ai.sh"
    fi
    # Worker spawn survival: verifies workers live long enough to complete tasks.
    # Catches the 30s death bug where C binary spawn path killed workers.
    if [[ -f "$SCRIPT_DIR/automated/test_worker_spawn_survival.sh" ]]; then
        run_ai_test "$SCRIPT_DIR/automated/test_worker_spawn_survival.sh"
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
