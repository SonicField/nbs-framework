#!/bin/bash
# run_cinderx_tests.sh — Canonical CinderX JIT test runner for aarch64
#
# THE single test runner for CinderX on devgpu004/devgpu009.
# Runs all 41 CinderX test suites and produces a summary report.
#
# Usage:
#   ./run_cinderx_tests.sh              # Run all tests (default)
#   ./run_cinderx_tests.sh jit          # Run only JIT tests
#   ./run_cinderx_tests.sh runtime      # Run only runtime tests
#   ./run_cinderx_tests.sh compiler     # Run only compiler tests
#   ./run_cinderx_tests.sh TESTNAME     # Run a specific test module
#   ./run_cinderx_tests.sh --fix-opcode # Fix cinderx.opcode and exit
#
# Environment:
#   PYTHONPATH is set to include PythonLib for the opcode module.
#   CinderX tests use force_compile() internally — no JIT env var needed.
#   CINDERX_ROOT defaults to ~/local/cinderx_dev/cinderx
#
# Gate: Aborts immediately if CinderX JIT is not importable or not enabled.
#       Will NOT silently run tests on stock Python.

set -uo pipefail

CINDERX_ROOT="${CINDERX_ROOT:-$HOME/local/cinderx_dev/cinderx}"
PYTHONLIB="$CINDERX_ROOT/cinderx/PythonLib"
TEST_DIR="$PYTHONLIB/test_cinderx"
RESULTS_FILE="/tmp/cinderx_test_results_$(date +%Y%m%d_%H%M%S).txt"

# Ensure opcode.py is in place (cmake build skips this step from setup.py)
fix_opcode() {
    local opcode_src="$PYTHONLIB/opcodes/3.12/opcode.py"
    local opcode_dst="$PYTHONLIB/cinderx/opcode.py"
    if [ -f "$opcode_src" ] && [ ! -f "$opcode_dst" ]; then
        echo "Copying opcode.py (cmake build fixup)..."
        cp "$opcode_src" "$opcode_dst"
    fi
}

# NOTE: CINDERJIT_ENABLE is NOT a real CinderX env var. CinderX tests work
# because they call cinderjit.force_compile() internally, not because of
# any env var. The real env vars are PYTHONJITAUTO=N, PYTHONJITALL=1, etc.
# We do NOT set them here — force_compile handles compilation for CinderX tests.
export PYTHONPATH="$PYTHONLIB${PYTHONPATH:+:$PYTHONPATH}"

# Test suite definitions (41 suites total: 17 JIT + 14 runtime + 10 compiler)
JIT_TESTS=(
    test_cinderjit
    test_jit_async_generators
    test_jit_attr_cache
    test_jit_coroutines
    test_jit_count_calls
    test_jit_disable
    test_jit_exception
    test_jit_frame
    test_jit_generator_aarch64
    test_jit_generators
    test_jit_global_cache
    test_jitlist
    test_jit_perf_map
    test_jit_preload
    test_jit_specialization
    test_jit_support_instrumentation
    test_jit_type_annotations
)

RUNTIME_TESTS=(
    test_asynclazyvalue
    test_coro_extensions
    test_enabling_parallel_gc
    test_frame_evaluator
    test_immortalize
    test_oss_quick
    test_parallel_gc
    test_perfmaps
    test_perf_profiler_precompile
    test_python310_bytecodes
    test_python312_bytecodes
    test_python314_bytecodes
    test_shadowcode
    test_type_cache
)

COMPILER_TESTS=(
    test_compiler_sbs_stdlib_0
    test_compiler_sbs_stdlib_1
    test_compiler_sbs_stdlib_2
    test_compiler_sbs_stdlib_3
    test_compiler_sbs_stdlib_4
    test_compiler_sbs_stdlib_5
    test_compiler_sbs_stdlib_6
    test_compiler_sbs_stdlib_7
    test_compiler_sbs_stdlib_8
    test_compiler_sbs_stdlib_9
)

# Colour codes (disabled if not a terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    RED='' GREEN='' YELLOW='' BOLD='' RESET=''
fi

# Handle --fix-opcode before anything else
if [ "${1:-}" = "--fix-opcode" ]; then
    fix_opcode
    echo "Done."
    exit 0
fi

# Select test suites
# Strip leading -- from argument (accept both --all and all)
ARG="${1:-all}"
ARG="${ARG#--}"

case "$ARG" in
    jit)      SUITES=("${JIT_TESTS[@]}") ;;
    runtime)  SUITES=("${RUNTIME_TESTS[@]}") ;;
    compiler) SUITES=("${COMPILER_TESTS[@]}") ;;
    all)      SUITES=("${JIT_TESTS[@]}" "${RUNTIME_TESTS[@]}" "${COMPILER_TESTS[@]}") ;;
    help|-h)
        echo "Usage: $0 [all|jit|runtime|compiler|TESTNAME|--fix-opcode]"
        echo ""
        echo "  all          Run all 41 test suites (default)"
        echo "  jit          Run 17 JIT test suites only"
        echo "  runtime      Run 14 runtime test suites only"
        echo "  compiler     Run 10 compiler test suites only"
        echo "  TESTNAME     Run a specific test module (e.g. test_jit_attr_cache)"
        echo "  --fix-opcode Fix cinderx.opcode import and exit"
        exit 0
        ;;
    *)
        # Single test module
        SUITES=("$ARG")
        ;;
esac

# Always fix opcode first
fix_opcode

# Results tracking
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_ERROR=0
TOTAL_SKIP=0
PASSED_SUITES=()
FAILED_SUITES=()
ERROR_SUITES=()
SKIPPED_SUITES=()
SUITE_COUNT=0

echo -e "${BOLD}CinderX Test Runner${RESET}"
echo "Root:     $CINDERX_ROOT"
echo "Python:   $(python3 --version 2>&1)"
echo "Suites:   ${#SUITES[@]}"
echo "Results:  $RESULTS_FILE"
echo "Started:  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "---"

# HARD GATE: verify CinderX is actually loaded and JIT is active.
# CinderX tests use force_compile() internally, so the JIT must be available.
# This prevents silently running tests on stock Python without the cinderjit module.
echo -n "Verifying CinderX JIT... "
CINDERX_CHECK=$(python3 -c "
import cinderjit
assert cinderjit.is_enabled(), 'cinderjit imported but JIT not enabled'
print('OK')
" 2>&1) || {
    echo -e "${RED}FATAL: CinderX JIT not available${RESET}"
    echo "$CINDERX_CHECK"
    echo ""
    echo "Tests CANNOT run without CinderX JIT. Ensure:"
    echo "  1. Python is the CinderX-patched build (not stock Python)"
    echo "  2. cinderjit module is importable and JIT is enabled"
    exit 1
}
echo -e "${GREEN}$CINDERX_CHECK${RESET}"

# Clear results file
> "$RESULTS_FILE"

cd "$PYTHONLIB"

for suite in "${SUITES[@]}"; do
    SUITE_COUNT=$((SUITE_COUNT + 1))
    printf "[%d/%d] %-45s " "$SUITE_COUNT" "${#SUITES[@]}" "$suite"

    # Run with timeout (120s per suite) and capture output
    OUTPUT=$(timeout 120 python3 -m unittest "test_cinderx.$suite" 2>&1)
    TEST_EXIT=$?

    # Parse results from unittest output
    RAN_LINE=$(echo "$OUTPUT" | grep -E '^Ran [0-9]+ test' || echo "")
    # STATUS_LINE is the line after "Ran N tests" — look for OK/FAILED there
    STATUS_LINE=$(echo "$OUTPUT" | grep -E '^(OK|FAILED)' | tail -1 || echo "")

    if [ -z "$RAN_LINE" ]; then
        # No "Ran N tests" line — check crash, timeout, skip, or import error
        if [ $TEST_EXIT -eq 124 ]; then
            # timeout(1) returns 124 when the command times out
            PARTIAL_DOTS=$(echo "$OUTPUT" | grep -c '\.\.\.' || echo 0)
            printf "${RED}TIMEOUT${RESET} (120s, ~%d tests ran before timeout)\n" "$PARTIAL_DOTS"
            FAILED_SUITES+=("$suite")
            TOTAL_FAIL=$((TOTAL_FAIL + 1))
            echo "$OUTPUT" > "/tmp/cinderx_timeout_${suite}.log"
        elif [ $TEST_EXIT -gt 128 ]; then
            # Process killed by signal (SIGSEGV=11, SIGBUS=7, SIGABRT=6)
            SIG=$((TEST_EXIT - 128))
            PARTIAL_DOTS=$(echo "$OUTPUT" | grep -c '\.\.\.' || echo 0)
            printf "${RED}CRASH${RESET} (signal %d, ~%d tests ran before crash)\n" "$SIG" "$PARTIAL_DOTS"
            FAILED_SUITES+=("$suite")
            TOTAL_FAIL=$((TOTAL_FAIL + 1))
            echo "$OUTPUT" > "/tmp/cinderx_crash_${suite}.log"
        elif echo "$OUTPUT" | grep -qE 'SkipTest:'; then
            SKIP_REASON=$(echo "$OUTPUT" | grep -oP 'SkipTest: \K.*' | head -1 || echo "")
            printf "${YELLOW}SKIP${RESET} (%s)\n" "${SKIP_REASON:-module-level skip}"
            SKIPPED_SUITES+=("$suite")
            TOTAL_SKIP=$((TOTAL_SKIP + 1))
        else
            # Genuine error — suite did not execute
            ERR_MSG=$(echo "$OUTPUT" | grep -E '(ModuleNotFoundError|ImportError|SyntaxError|AttributeError):' | tail -1 | head -c 60)
            printf "${RED}ERROR${RESET} (%s)\n" "${ERR_MSG:-did not execute}"
            ERROR_SUITES+=("$suite")
            TOTAL_ERROR=$((TOTAL_ERROR + 1))
            # Save full output for diagnosis
            echo "$OUTPUT" > "/tmp/cinderx_fail_${suite}.log"
        fi
    else
        TESTS=$(echo "$RAN_LINE" | grep -oP '^Ran \K[0-9]+' || echo 0)

        if [ "$TESTS" -eq 0 ]; then
            # Ran 0 tests — treat as skip
            SKIP_REASON=$(echo "$OUTPUT" | grep -oP 'SkipTest: \K.*' | head -1 || echo "")
            printf "${YELLOW}SKIP${RESET} (%s)\n" "${SKIP_REASON:-all tests skipped}"
            SKIPPED_SUITES+=("$suite")
            TOTAL_SKIP=$((TOTAL_SKIP + 1))
        elif echo "$STATUS_LINE" | grep -q 'FAILED'; then
            FAILS=$(echo "$STATUS_LINE" | grep -oP 'failures=\K[0-9]+' || echo 0)
            ERRS=$(echo "$STATUS_LINE" | grep -oP 'errors=\K[0-9]+' || echo 0)
            PASSED=$((TESTS - FAILS - ERRS))
            printf "${RED}FAIL${RESET} (%d pass, %d fail, %d error)\n" "$PASSED" "$FAILS" "$ERRS"
            FAILED_SUITES+=("$suite")
            TOTAL_PASS=$((TOTAL_PASS + PASSED))
            TOTAL_FAIL=$((TOTAL_FAIL + FAILS))
            TOTAL_ERROR=$((TOTAL_ERROR + ERRS))
            # Save full output for diagnosis
            echo "$OUTPUT" > "/tmp/cinderx_fail_${suite}.log"
        elif echo "$STATUS_LINE" | grep -q 'OK'; then
            SKIPS=$(echo "$STATUS_LINE" | grep -oP 'skipped=\K[0-9]+' || echo 0)
            printf "${GREEN}OK${RESET}   (%d pass" "$((TESTS - SKIPS))"
            if [ "$SKIPS" -gt 0 ]; then
                printf ", %d skip" "$SKIPS"
                TOTAL_SKIP=$((TOTAL_SKIP + SKIPS))
            fi
            printf ")\n"
            PASSED_SUITES+=("$suite")
            TOTAL_PASS=$((TOTAL_PASS + TESTS - SKIPS))
        else
            # Unknown status — report as unknown but count test
            printf "${YELLOW}???${RESET}  (%d tests, status unclear)\n" "$TESTS"
            ERROR_SUITES+=("$suite")
        fi
    fi

    # Record per-suite results to CSV
    echo "$suite|$SUITE_COUNT|${TESTS:-0}|${PASSED:-0}|${FAILS:-0}|${ERRS:-0}|${SKIPS:-0}" >> "$RESULTS_FILE"
done

# Summary
echo ""
echo "=== SUMMARY ==="
echo "Finished: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""
printf "  Tests:   %d pass, %d fail, %d error, %d skip\n" "$TOTAL_PASS" "$TOTAL_FAIL" "$TOTAL_ERROR" "$TOTAL_SKIP"
printf "  Suites:  %d pass, %d fail, %d error, %d skip (of %d)\n" \
    "${#PASSED_SUITES[@]}" "${#FAILED_SUITES[@]}" "${#ERROR_SUITES[@]}" "${#SKIPPED_SUITES[@]}" "$SUITE_COUNT"
echo ""

if [ ${#FAILED_SUITES[@]} -gt 0 ]; then
    echo -e "${RED}Failed suites:${RESET}"
    for s in "${FAILED_SUITES[@]}"; do echo "  - $s (log: /tmp/cinderx_fail_${s}.log)"; done
    echo ""
fi

if [ ${#ERROR_SUITES[@]} -gt 0 ]; then
    echo -e "${RED}Error suites (did not run):${RESET}"
    for s in "${ERROR_SUITES[@]}"; do echo "  - $s (log: /tmp/cinderx_fail_${s}.log)"; done
    echo ""
fi

if [ ${#SKIPPED_SUITES[@]} -gt 0 ]; then
    echo -e "${YELLOW}Skipped suites:${RESET}"
    for s in "${SKIPPED_SUITES[@]}"; do echo "  - $s"; done
    echo ""
fi

echo "Results CSV: $RESULTS_FILE"

# Exit code: 0 if all suites executed (failures ok), 1 if any suite errored (didn't execute)
if [ ${#ERROR_SUITES[@]} -gt 0 ]; then
    exit 1
else
    exit 0
fi
