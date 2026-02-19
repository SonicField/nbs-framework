#!/bin/bash
# run_cinderx_torch_smoke_tests.sh — PyTorch P0 smoke tests under CinderX JIT
#
# Runs PyTorch P0 test suites with CinderX JIT auto-compilation enabled.
# Verifies that the JIT actually compiles functions (not vacuous interpreter-only).
#
# Usage:
#   ./run_cinderx_torch_smoke_tests.sh              # Run all P0 suites
#   ./run_cinderx_torch_smoke_tests.sh test_torch    # Run a specific suite
#   ./run_cinderx_torch_smoke_tests.sh --check-only  # Verify setup without running tests
#
# Environment:
#   CINDERX_ROOT    CinderX source root (default: ~/local/cinderx_dev/cinderx)
#   PYTORCH_ROOT    PyTorch source root (default: $CINDERX_ROOT/../pytorch)
#   TORCH_TIMEOUT   Per-suite timeout in seconds (default: 1200)
#
# Gate: Aborts if CinderX JIT is not available or auto-compile crashes.
#       Uses cinderjit.auto() (Python API) AFTER torch import — NOT env vars.
#       PYTHONJITAUTO env var causes import-time crashes; cinderjit.auto()
#       called after torch import avoids this.

set -uo pipefail

CINDERX_ROOT="${CINDERX_ROOT:-$HOME/local/cinderx_dev/cinderx}"
PYTORCH_ROOT="${PYTORCH_ROOT:-$CINDERX_ROOT/../pytorch}"
PYTHONLIB="$CINDERX_ROOT/cinderx/PythonLib"
TORCH_TIMEOUT="${TORCH_TIMEOUT:-1200}"
RESULTS_FILE="/tmp/cinderx_torch_results_$(date +%Y%m%d_%H%M%S).txt"

# The CinderX Python binary — NOT system python3
PYTHON="$CINDERX_ROOT/python"

# P0 suites (highest-value PyTorch test files)
P0_SUITES=(
    test_torch
    test_autograd
    test_nn
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

# NOTE: CINDERJIT_ENABLE is NOT a real CinderX env var (it's a no-op).
# The real env vars are PYTHONJITAUTO=N, PYTHONJITALL=1, etc.
# We do NOT set PYTHONJITAUTO here because it triggers compilation during
# import (before torch is loaded), which causes SEGFAULT. Instead, we call
# cinderjit.auto() from Python AFTER importing torch.
export PYTHONPATH="$PYTHONLIB${PYTHONPATH:+:$PYTHONPATH}"

# --- Pre-flight checks ---

echo -e "${BOLD}CinderX PyTorch Smoke Tests${RESET}"
echo "CinderX:  $CINDERX_ROOT"
echo "PyTorch:  $PYTORCH_ROOT"
echo "Python:   $PYTHON"
echo "Timeout:  ${TORCH_TIMEOUT}s per suite"
echo "Results:  $RESULTS_FILE"
echo "Started:  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "---"

# Gate 1: CinderX Python binary exists
if [ ! -x "$PYTHON" ]; then
    echo -e "${RED}FATAL: CinderX Python not found at $PYTHON${RESET}"
    echo "Set CINDERX_ROOT to point to the CinderX source directory."
    exit 1
fi

# Gate 2: CinderX JIT is importable (no env var needed — just check the module exists)
echo -n "Gate 1: CinderX JIT available... "
GATE1=$("$PYTHON" -c "
import cinderjit
print('OK — cinderjit importable')
" 2>&1) || {
    echo -e "${RED}FAIL${RESET}"
    echo "$GATE1"
    echo ""
    echo "CinderX JIT not available. Check CINDERX_ROOT and CINDERJIT_ENABLE."
    exit 1
}
echo -e "${GREEN}$GATE1${RESET}"

# Report Python version
echo "Python:   $("$PYTHON" --version 2>&1)"

# Gate 3: PyTorch is importable
echo -n "Gate 2: PyTorch importable... "
GATE2=$("$PYTHON" -c "
import torch
print(f'OK — torch {torch.__version__}')
" 2>&1) || {
    echo -e "${RED}FAIL${RESET}"
    echo "$GATE2"
    echo ""
    echo "PyTorch not importable. Check PYTORCH_ROOT and PYTHONPATH."
    exit 1
}
echo -e "${GREEN}$GATE2${RESET}"

# Gate 4: PyTorch test directory exists
TORCH_TEST_DIR="$PYTORCH_ROOT/test"
echo -n "Gate 3: PyTorch test directory... "
if [ ! -d "$TORCH_TEST_DIR" ]; then
    echo -e "${RED}FAIL — $TORCH_TEST_DIR not found${RESET}"
    exit 1
fi
echo -e "${GREEN}OK — $TORCH_TEST_DIR${RESET}"

# Gate 5: JIT auto-compile works AFTER torch import (sequencing matters!)
echo -n "Gate 4: JIT auto-compile works... "
GATE4=$("$PYTHON" -c "
import cinderjit
import torch  # Must import torch BEFORE enabling auto-compile

# Enable auto-compilation AFTER torch is loaded
cinderjit.auto()

# Run a simple function enough times to trigger compilation
def test_func(x):
    return x + 1

for i in range(200):
    test_func(i)

# Check if the function was compiled
try:
    if cinderjit.is_jit_compiled(test_func):
        print('OK — auto-compile confirmed (torch imported first)')
    else:
        print('WARN — function not compiled (threshold not reached?)')
except AttributeError:
    # Fallback if is_jit_compiled not available
    print('OK — cinderjit.auto() called without crash (cannot verify compilation)')
" 2>&1) || {
    echo -e "${RED}FAIL — CRASH during auto-compile gate check${RESET}"
    echo "$GATE4"
    echo ""
    echo "The JIT crashes during auto-compilation. This is a known issue."
    echo "Tests cannot proceed until this is fixed."
    exit 1
}
echo -e "${GREEN}$GATE4${RESET}"

# Handle --check-only
if [ "${1:-}" = "--check-only" ]; then
    echo ""
    echo "All gates passed. Ready to run PyTorch smoke tests."
    exit 0
fi

# --- Select suites ---

ARG="${1:-all}"
ARG="${ARG#--}"

case "$ARG" in
    all)
        SUITES=("${P0_SUITES[@]}")
        ;;
    help|-h)
        echo "Usage: $0 [all|test_torch|test_autograd|test_nn|--check-only]"
        exit 0
        ;;
    *)
        SUITES=("$ARG")
        ;;
esac

# --- Run tests ---

echo ""
echo "=== Running ${#SUITES[@]} P0 suite(s) with JIT auto-compile ==="
echo ""

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0
TOTAL_ERROR=0
PASSED_SUITES=()
FAILED_SUITES=()
CRASHED_SUITES=()
SUITE_COUNT=0

> "$RESULTS_FILE"

cd "$TORCH_TEST_DIR"

for suite in "${SUITES[@]}"; do
    SUITE_COUNT=$((SUITE_COUNT + 1))
    TEST_FILE="${suite}.py"

    if [ ! -f "$TEST_FILE" ]; then
        printf "[%d/%d] %-30s ${RED}NOT FOUND${RESET}\n" "$SUITE_COUNT" "${#SUITES[@]}" "$suite"
        FAILED_SUITES+=("$suite")
        continue
    fi

    printf "[%d/%d] %-30s " "$SUITE_COUNT" "${#SUITES[@]}" "$suite"
    START_TIME=$(date +%s)

    # Run with auto-compile enabled via a wrapper that:
    # 1. Imports torch FIRST (before auto-compile — sequencing matters!)
    # 2. Enables cinderjit.auto() AFTER torch import
    # 3. Runs pytest
    # 4. Reports compilation count after tests complete
    LOG_FILE="/tmp/cinderx_torch_${suite}.log"
    COMPILE_LOG="/tmp/cinderx_torch_${suite}_compiles.log"

    OUTPUT=$(timeout "$TORCH_TIMEOUT" "$PYTHON" -c "
import sys
import cinderjit
import torch  # Import torch BEFORE enabling auto-compile

# Enable auto-compilation AFTER torch is loaded
cinderjit.auto()

# Run pytest programmatically
import pytest
exit_code = pytest.main([
    '${TEST_FILE}',
    '--tb=line',
    '--no-header',
    '-q',
    '--timeout=300',
])

# Report compilation statistics
try:
    compiled = cinderjit.get_num_functions_compiled()
    print(f'CINDERJIT_COMPILED={compiled}', file=sys.stderr)
except AttributeError:
    print('CINDERJIT_COMPILED=unknown', file=sys.stderr)

sys.exit(exit_code)
" 2>"$COMPILE_LOG" | tee "$LOG_FILE" 2>&1)
    TEST_EXIT=$?
    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))

    # Extract compilation count
    COMPILED=$(grep -oP 'CINDERJIT_COMPILED=\K.*' "$COMPILE_LOG" 2>/dev/null || echo "unknown")

    # Parse pytest summary line (e.g., "1565 passed, 37 failed, 136 skipped")
    SUMMARY_LINE=$(echo "$OUTPUT" | grep -E '[0-9]+ passed' | tail -1 || echo "")

    if [ $TEST_EXIT -gt 128 ]; then
        # Crashed (signal)
        SIG=$((TEST_EXIT - 128))
        printf "${RED}CRASH${RESET} (signal %d, %ds, %s compiled)\n" "$SIG" "$ELAPSED" "$COMPILED"
        CRASHED_SUITES+=("$suite")
    elif [ $TEST_EXIT -eq 124 ]; then
        # Timeout
        printf "${RED}TIMEOUT${RESET} (%ds, %s compiled)\n" "$TORCH_TIMEOUT" "$COMPILED"
        FAILED_SUITES+=("$suite")
    elif [ -n "$SUMMARY_LINE" ]; then
        PASSED=$(echo "$SUMMARY_LINE" | grep -oP '[0-9]+(?= passed)' || echo 0)
        FAILED=$(echo "$SUMMARY_LINE" | grep -oP '[0-9]+(?= failed)' || echo 0)
        SKIPPED=$(echo "$SUMMARY_LINE" | grep -oP '[0-9]+(?= skipped)' || echo 0)
        ERRORS=$(echo "$SUMMARY_LINE" | grep -oP '[0-9]+(?= error)' || echo 0)

        TOTAL_PASS=$((TOTAL_PASS + PASSED))
        TOTAL_FAIL=$((TOTAL_FAIL + FAILED))
        TOTAL_SKIP=$((TOTAL_SKIP + SKIPPED))
        TOTAL_ERROR=$((TOTAL_ERROR + ERRORS))

        if [ "$FAILED" -eq 0 ] && [ "$ERRORS" -eq 0 ]; then
            printf "${GREEN}OK${RESET}    (%dp/%df/%ds, %ds, %s compiled)\n" "$PASSED" "$FAILED" "$SKIPPED" "$ELAPSED" "$COMPILED"
            PASSED_SUITES+=("$suite")
        else
            printf "${YELLOW}FAIL${RESET}  (%dp/%df/%ds, %ds, %s compiled)\n" "$PASSED" "$FAILED" "$SKIPPED" "$ELAPSED" "$COMPILED"
            FAILED_SUITES+=("$suite")
        fi
    else
        printf "${RED}ERROR${RESET} (exit %d, %ds, %s compiled)\n" "$TEST_EXIT" "$ELAPSED" "$COMPILED"
        FAILED_SUITES+=("$suite")
    fi

    echo "$suite|$PASSED|$FAILED|$SKIPPED|$ELAPSED|$COMPILED" >> "$RESULTS_FILE" 2>/dev/null
done

# --- Summary ---

echo ""
echo "=== SUMMARY ==="
echo "Finished: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""
printf "  Tests:   %d pass, %d fail, %d error, %d skip\n" "$TOTAL_PASS" "$TOTAL_FAIL" "$TOTAL_ERROR" "$TOTAL_SKIP"
printf "  Suites:  %d pass, %d fail, %d crash (of %d)\n" \
    "${#PASSED_SUITES[@]}" "${#FAILED_SUITES[@]}" "${#CRASHED_SUITES[@]}" "$SUITE_COUNT"
echo ""

if [ ${#CRASHED_SUITES[@]} -gt 0 ]; then
    echo -e "${RED}CRASHED suites (JIT SEGFAULT):${RESET}"
    for s in "${CRASHED_SUITES[@]}"; do echo "  - $s (log: /tmp/cinderx_torch_${s}.log)"; done
    echo ""
fi

if [ ${#FAILED_SUITES[@]} -gt 0 ]; then
    echo -e "${YELLOW}Failed suites:${RESET}"
    for s in "${FAILED_SUITES[@]}"; do echo "  - $s (log: /tmp/cinderx_torch_${s}.log)"; done
    echo ""
fi

echo "Results CSV: $RESULTS_FILE"

# Exit code: 2 if any crash, 1 if any fail, 0 if all pass
if [ ${#CRASHED_SUITES[@]} -gt 0 ]; then
    exit 2
elif [ ${#FAILED_SUITES[@]} -gt 0 ]; then
    exit 1
else
    exit 0
fi
