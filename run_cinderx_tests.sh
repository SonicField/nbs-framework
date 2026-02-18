#!/bin/bash
# run_cinderx_tests.sh — Unified CinderX JIT test runner for aarch64
#
# Usage: ./run_cinderx_tests.sh [--jit-only] [--all] [--suite NAME]
#
# Runs all CinderX test suites in test_cinderx/ using:
#   CINDERJIT_ENABLE=1 PYTHONPATH=. python3 -m unittest test_cinderx.<module> -v
#
# Prerequisites:
#   - CinderX built and python3 on PATH (typically via venv)
#   - cinderx.opcode available (run --fix-opcode if needed)
#   - Working directory: cinderx/PythonLib/ within the CinderX source tree
#
# Author: @testkeeper
# Date: 18-02-2026

set -euo pipefail

# Configuration
CINDERX_ROOT="${CINDERX_ROOT:-$(cd "$(dirname "$0")" 2>/dev/null && pwd)}"
PYTHONLIB_DIR=""
RESULTS_FILE="/tmp/cinderx_test_results_$(date +%Y%m%d_%H%M%S).txt"

# Colour output (if terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' NC=''
fi

# Find PythonLib directory
find_pythonlib() {
    # Try common locations
    local candidates=(
        "$CINDERX_ROOT/cinderx/PythonLib"
        "$HOME/local/cinderx_dev/cinderx/cinderx/PythonLib"
        "./cinderx/PythonLib"
        "."
    )
    for dir in "${candidates[@]}"; do
        if [ -d "$dir/test_cinderx" ]; then
            PYTHONLIB_DIR="$dir"
            return 0
        fi
    done
    echo "ERROR: Cannot find test_cinderx/ directory. Set CINDERX_ROOT or run from cinderx/PythonLib/"
    exit 1
}

# Fix cinderx.opcode import (cmake build misses this step from setup.py)
fix_opcode() {
    local opcode_src="$PYTHONLIB_DIR/opcodes/3.12/opcode.py"
    local opcode_dst="$PYTHONLIB_DIR/cinderx/opcode.py"
    if [ ! -f "$opcode_dst" ] && [ -f "$opcode_src" ]; then
        echo -e "${YELLOW}Fixing cinderx.opcode import: copying opcode.py${NC}"
        cp "$opcode_src" "$opcode_dst"
        echo -e "${GREEN}Fixed: $opcode_dst${NC}"
    elif [ -f "$opcode_dst" ]; then
        echo -e "${GREEN}cinderx.opcode already available${NC}"
    else
        echo -e "${RED}WARNING: Cannot find opcode source at $opcode_src${NC}"
    fi
}

# JIT test suites (core — these exercise the JIT compiler)
JIT_SUITES=(
    test_jit_attr_cache
    test_jit_generator_aarch64
    test_jit_generators
    test_jit_async_generators
    test_jit_coroutines
    test_jit_count_calls
    test_jit_disable
    test_jit_exception
    test_jit_frame
    test_jit_global_cache
    test_jitlist
    test_jit_perf_map
    test_jit_preload
    test_jit_specialization
    test_jit_support_instrumentation
    test_jit_type_annotations
    test_cinderjit
)

# Non-JIT test suites (runtime, compiler, GC, etc.)
OTHER_SUITES=(
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

# Compiler test suites (heavy, may have additional dependencies)
COMPILER_SUITES=(
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

# Run a single test suite, capture results
run_suite() {
    local suite="$1"
    local label="$2"
    local start_time=$(date +%s)

    echo -e "\n${BLUE}━━━ $label: $suite ━━━${NC}"

    local output
    local exit_code=0
    output=$(cd "$PYTHONLIB_DIR" && CINDERJIT_ENABLE=1 PYTHONPATH=. python3 -m unittest "test_cinderx.$suite" -v 2>&1) || exit_code=$?

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    # Extract summary line (e.g., "Ran 24 tests in 0.123s")
    local ran_line=$(echo "$output" | grep -E "^Ran [0-9]+ test" || echo "")
    local result_line=$(echo "$output" | tail -1)

    # Count results
    local total=0 passed=0 failed=0 errors=0 skipped=0
    if [[ "$ran_line" =~ Ran\ ([0-9]+)\ test ]]; then
        total="${BASH_REMATCH[1]}"
    fi

    if echo "$result_line" | grep -q "^OK"; then
        passed=$total
        if [[ "$result_line" =~ skipped=([0-9]+) ]]; then
            skipped="${BASH_REMATCH[1]}"
            passed=$((total - skipped))
        fi
        echo -e "${GREEN}PASS${NC} — $ran_line ($result_line) [${duration}s]"
    elif echo "$result_line" | grep -q "FAILED"; then
        if [[ "$result_line" =~ failures=([0-9]+) ]]; then
            failed="${BASH_REMATCH[1]}"
        fi
        if [[ "$result_line" =~ errors=([0-9]+) ]]; then
            errors="${BASH_REMATCH[1]}"
        fi
        passed=$((total - failed - errors))
        echo -e "${RED}FAIL${NC} — $ran_line ($result_line) [${duration}s]"
        # Show failure details
        echo "$output" | grep -E "^(FAIL|ERROR):" | head -10
    else
        # Import error or other catastrophic failure
        echo -e "${RED}ERROR${NC} — could not run suite [${duration}s]"
        echo "$output" | tail -5
        errors=1
    fi

    # Record to results file
    echo "$suite|$label|$total|$passed|$failed|$errors|$skipped|$exit_code|$duration" >> "$RESULTS_FILE"

    # If verbose failures, save full output
    if [ $failed -gt 0 ] || [ $errors -gt 0 ]; then
        local fail_log="/tmp/cinderx_fail_${suite}.log"
        echo "$output" > "$fail_log"
        echo -e "  ${YELLOW}Full output: $fail_log${NC}"
    fi
}

# Print summary table
print_summary() {
    echo -e "\n${BLUE}══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}                    CinderX Test Summary                      ${NC}"
    echo -e "${BLUE}══════════════════════════════════════════════════════════════${NC}"
    echo ""
    printf "%-35s %6s %6s %6s %6s %6s %5s\n" "Suite" "Total" "Pass" "Fail" "Error" "Skip" "Time"
    printf "%-35s %6s %6s %6s %6s %6s %5s\n" "---" "---" "---" "---" "---" "---" "---"

    local grand_total=0 grand_pass=0 grand_fail=0 grand_error=0 grand_skip=0

    while IFS='|' read -r suite label total passed failed errors skipped exit_code duration; do
        local status_colour="$GREEN"
        if [ "$failed" -gt 0 ] || [ "$errors" -gt 0 ]; then
            status_colour="$RED"
        elif [ "$total" -eq 0 ]; then
            status_colour="$YELLOW"
        fi

        printf "${status_colour}%-35s %6s %6s %6s %6s %6s %4ss${NC}\n" \
            "$suite" "$total" "$passed" "$failed" "$errors" "$skipped" "$duration"

        grand_total=$((grand_total + total))
        grand_pass=$((grand_pass + passed))
        grand_fail=$((grand_fail + failed))
        grand_error=$((grand_error + errors))
        grand_skip=$((grand_skip + skipped))
    done < "$RESULTS_FILE"

    printf "%-35s %6s %6s %6s %6s %6s\n" "---" "---" "---" "---" "---" "---"
    printf "%-35s %6s %6s %6s %6s %6s\n" \
        "TOTAL" "$grand_total" "$grand_pass" "$grand_fail" "$grand_error" "$grand_skip"

    echo ""
    if [ "$grand_fail" -eq 0 ] && [ "$grand_error" -eq 0 ]; then
        echo -e "${GREEN}ALL TESTS PASSED${NC} ($grand_pass passed, $grand_skip skipped)"
    else
        echo -e "${RED}FAILURES DETECTED${NC} ($grand_fail failures, $grand_error errors out of $grand_total tests)"
    fi
    echo ""
    echo "Results saved to: $RESULTS_FILE"
    echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
}

# Main
main() {
    local mode="jit"  # default to JIT-only

    for arg in "$@"; do
        case "$arg" in
            --jit-only) mode="jit" ;;
            --all) mode="all" ;;
            --fix-opcode) fix_opcode; exit 0 ;;
            --suite)
                # Next arg is suite name — handled below
                ;;
            --help|-h)
                echo "Usage: $0 [--jit-only] [--all] [--fix-opcode] [--suite NAME]"
                echo ""
                echo "Options:"
                echo "  --jit-only     Run JIT test suites only (default)"
                echo "  --all          Run all test suites (JIT + runtime + compiler)"
                echo "  --fix-opcode   Fix cinderx.opcode import and exit"
                echo "  --suite NAME   Run a single named suite"
                echo ""
                echo "Environment:"
                echo "  CINDERX_ROOT   Path to cinderx source root"
                echo "  CINDERJIT_ENABLE is set automatically"
                exit 0
                ;;
            *)
                # Check if previous arg was --suite
                if [ "${prev_arg:-}" = "--suite" ]; then
                    mode="single"
                    single_suite="$arg"
                fi
                ;;
        esac
        prev_arg="$arg"
    done

    find_pythonlib
    echo -e "${BLUE}CinderX Test Runner${NC}"
    echo "PythonLib: $PYTHONLIB_DIR"
    echo "Python: $(which python3)"
    echo "Mode: $mode"
    echo "Results: $RESULTS_FILE"
    echo ""

    # Always fix opcode first
    fix_opcode

    # Verify JIT is available
    echo -n "Checking JIT availability... "
    if cd "$PYTHONLIB_DIR" && CINDERJIT_ENABLE=1 PYTHONPATH=. python3 -c "import cinderjit; print('OK')" 2>/dev/null; then
        echo -e "${GREEN}JIT available${NC}"
    else
        echo -e "${YELLOW}JIT not available (tests will run without JIT)${NC}"
    fi

    # Clear results file
    > "$RESULTS_FILE"

    case "$mode" in
        jit)
            echo -e "\n${BLUE}Running ${#JIT_SUITES[@]} JIT test suites...${NC}"
            for suite in "${JIT_SUITES[@]}"; do
                run_suite "$suite" "JIT"
            done
            ;;
        all)
            echo -e "\n${BLUE}Running ${#JIT_SUITES[@]} JIT + ${#OTHER_SUITES[@]} runtime + ${#COMPILER_SUITES[@]} compiler suites...${NC}"
            for suite in "${JIT_SUITES[@]}"; do
                run_suite "$suite" "JIT"
            done
            for suite in "${OTHER_SUITES[@]}"; do
                run_suite "$suite" "Runtime"
            done
            for suite in "${COMPILER_SUITES[@]}"; do
                run_suite "$suite" "Compiler"
            done
            ;;
        single)
            run_suite "$single_suite" "Single"
            ;;
    esac

    print_summary
    echo "TEST_RUNNER_DONE"
}

main "$@"
