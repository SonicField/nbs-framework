#!/bin/bash
# test_cinderx.sh — Single entry point for CinderX JIT per-file test sweep
#
# Runs each test file in cinderx/PythonLib/test_cinderx/ individually with
# PYTHONJIT=1 and reports pass/fail per file. Individual invocation avoids
# the Session.genitems JIT crash that occurs when running all files together.
#
# Usage:
#   ./test_cinderx.sh                    # Run all tests
#   ./test_cinderx.sh --filter=jit       # Run only files matching 'jit'
#   ./test_cinderx.sh --timeout=180      # Per-file timeout (default: 120s)
#   ./test_cinderx.sh --no-jit           # Run with PYTHONJIT=0
#
# Exit codes:
#   0 — All files passed
#   1 — Failures detected
#   2 — Usage error

# --- Configuration ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${SCRIPT_DIR}/cinderx/PythonLib/test_cinderx"
VENV="${SCRIPT_DIR}/../venv/bin/python3"
PER_FILE_TIMEOUT=120
FILTER=""
JIT_ENV="PYTHONJIT=1"

# --- Parse arguments ---
for arg in "$@"; do
    case "$arg" in
        --filter=*) FILTER="${arg#*=}" ;;
        --timeout=*) PER_FILE_TIMEOUT="${arg#*=}" ;;
        --no-jit) JIT_ENV="PYTHONJIT=0" ;;
        --help|-h)
            echo "Usage: $0 [--filter=PATTERN] [--timeout=SECONDS] [--no-jit]"
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 2 ;;
    esac
done

# --- Validate environment ---
if [ ! -d "$TEST_DIR" ]; then
    echo "ERROR: Test directory not found: $TEST_DIR"
    echo "Expected to be run from the cinderx repo root."
    exit 2
fi

if [ ! -x "$VENV" ]; then
    VENV="python3"
fi

# --- Helper: extract number before a word from pytest summary ---
extract_count() {
    # Usage: extract_count "summary line" "word"
    # Returns the number preceding the word, or 0 if not found
    local line="$1" word="$2"
    local n
    n=$(echo "$line" | grep -oE "[0-9]+ ${word}" | grep -oE "^[0-9]+" || true)
    echo "${n:-0}"
}

# --- Collect test files ---
FILES=()
for f in "$TEST_DIR"/test_*.py; do
    [ -f "$f" ] || continue
    base="$(basename "$f")"
    if [ -n "$FILTER" ] && ! echo "$base" | grep -q "$FILTER"; then
        continue
    fi
    FILES+=("$f")
done

if [ ${#FILES[@]} -eq 0 ]; then
    echo "ERROR: No test files found matching filter '${FILTER:-*}'"
    exit 2
fi

echo "=== CinderX Test Sweep ==="
echo "Date:      $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "JIT:       $JIT_ENV"
echo "Timeout:   ${PER_FILE_TIMEOUT}s per file"
echo "Files:     ${#FILES[@]}"
echo "Filter:    ${FILTER:-none}"
echo "Python:    $VENV"
echo "=========================="
echo ""

# --- Run tests ---
PASS=0
FAIL=0
SKIP=0
ERROR=0
TOTAL_PASSED=0
TOTAL_FAILED=0
TOTAL_SKIPPED=0
TOTAL_ERRORS=0
FAIL_LIST=""
ERROR_LIST=""

for f in "${FILES[@]}"; do
    base="$(basename "$f")"

    # Run pytest, capture output. Do NOT use set -e here.
    raw_output=""
    exit_code=0
    raw_output=$(env $JIT_ENV timeout "$PER_FILE_TIMEOUT" \
        "$VENV" -m pytest "$f" --tb=no -q 2>&1) || exit_code=$?

    # Parse the pytest summary line
    summary=$(echo "$raw_output" | grep -E '(passed|failed|error|skipped|no tests ran)' | tail -1)

    # Extract counts using portable grep -oE
    n_passed=$(extract_count "$summary" "passed")
    n_failed=$(extract_count "$summary" "failed")
    n_skipped=$(extract_count "$summary" "skipped")
    n_errors=$(extract_count "$summary" "error")

    # Accumulate totals
    TOTAL_PASSED=$((TOTAL_PASSED + n_passed))
    TOTAL_FAILED=$((TOTAL_FAILED + n_failed))
    TOTAL_SKIPPED=$((TOTAL_SKIPPED + n_skipped))
    TOTAL_ERRORS=$((TOTAL_ERRORS + n_errors))

    # Classify the file result — check failures FIRST (fixes the grep bug)
    if [ "$n_failed" -gt 0 ] || [ "$n_errors" -gt 0 ]; then
        status="FAIL"
        FAIL=$((FAIL + 1))
        detail="${n_passed}p/${n_failed}f/${n_errors}e/${n_skipped}s"
        FAIL_LIST="${FAIL_LIST}  - ${base} (${detail})\n"
    elif echo "$summary" | grep -q "no tests ran"; then
        status="SKIP"
        SKIP=$((SKIP + 1))
        detail="no tests collected"
    elif [ "$n_passed" -eq 0 ] && [ "$n_skipped" -gt 0 ]; then
        status="SKIP"
        SKIP=$((SKIP + 1))
        detail="${n_skipped} skipped"
    elif [ "$n_passed" -gt 0 ]; then
        status="PASS"
        PASS=$((PASS + 1))
        detail="${n_passed} passed"
        [ "$n_skipped" -gt 0 ] && detail="${detail}, ${n_skipped} skipped"
    elif [ "$exit_code" -eq 124 ]; then
        status="TMOUT"
        ERROR=$((ERROR + 1))
        detail="timeout after ${PER_FILE_TIMEOUT}s"
        ERROR_LIST="${ERROR_LIST}  - ${base} (TIMEOUT)\n"
    else
        status="ERROR"
        ERROR=$((ERROR + 1))
        detail="exit=${exit_code}"
        ERROR_LIST="${ERROR_LIST}  - ${base} (exit=${exit_code})\n"
    fi

    printf "  %-5s %-50s %s\n" "$status" "$base" "$detail"
done

# --- Summary ---
echo ""
echo "=== SWEEP SUMMARY ==="
echo "Files:    ${#FILES[@]} total"
echo "  PASS:   $PASS"
echo "  FAIL:   $FAIL"
echo "  SKIP:   $SKIP"
echo "  ERROR:  $ERROR"
echo ""
echo "Tests:    $((TOTAL_PASSED + TOTAL_FAILED + TOTAL_SKIPPED + TOTAL_ERRORS)) total"
echo "  passed:  $TOTAL_PASSED"
echo "  failed:  $TOTAL_FAILED"
echo "  skipped: $TOTAL_SKIPPED"
echo "  errors:  $TOTAL_ERRORS"

if [ -n "$FAIL_LIST" ]; then
    echo ""
    echo "Failed files:"
    printf "$FAIL_LIST"
fi

if [ -n "$ERROR_LIST" ]; then
    echo ""
    echo "Error files:"
    printf "$ERROR_LIST"
fi

echo "=== SWEEP DONE ==="

# Exit with 1 if any failures, 0 otherwise
if [ "$FAIL" -gt 0 ] || [ "$ERROR" -gt 0 ]; then
    exit 1
fi
exit 0
