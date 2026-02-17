#!/bin/bash
# CinderX ARM JIT — Phase 2a Baseline Test Script
# Runs PyTorch test suites and captures structured results
#
# Usage:
#   ./17-02-2026-cinderx-baseline-test.sh <mode> <output_dir>
#
# Modes:
#   stock    — Meta Python 3.12.12+meta venv, no CinderX (Baseline A)
#   stub     — CinderX with CINDER_UNSUPPORTED (JIT stubs, Baseline B)
#   jit      — CinderX with JIT active (Target)
#
# Output: JSON report per test file + summary CSV

set -euo pipefail

MODE="${1:?Usage: $0 <stock|stub|jit> <output_dir>}"
OUTPUT_DIR="${2:?Usage: $0 <mode> <output_dir>}"

CINDERX_DEV="$HOME/local/cinderx_dev"
PYTORCH_DIR="$CINDERX_DEV/pytorch"
PYTORCH_TEST_DIR="$PYTORCH_DIR/test"
CINDERX_DIR="$CINDERX_DEV/cinderx"

# Validate mode
case "$MODE" in
    stock|stub|jit)
        ;;
    *)
        echo "ERROR: Invalid mode '$MODE'. Must be stock, stub, or jit." >&2
        exit 1
        ;;
esac

# Create output directory
mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date -u +%Y%m%dT%H%M%SZ)
REPORT_DIR="$OUTPUT_DIR/${MODE}_${TIMESTAMP}"
mkdir -p "$REPORT_DIR"

echo "=== CinderX Baseline Test: mode=$MODE ==="
echo "Output: $REPORT_DIR"
echo "Timestamp: $TIMESTAMP"
echo "Architecture: $(uname -m)"
echo "Python: $(python3 --version 2>&1)"

# Record environment
{
    echo "mode=$MODE"
    echo "timestamp=$TIMESTAMP"
    echo "arch=$(uname -m)"
    echo "python=$(python3 --version 2>&1)"
    echo "hostname=$(hostname)"
    echo "cinderx_dir=$CINDERX_DIR"
    echo "pytorch_dir=$PYTORCH_DIR"
} > "$REPORT_DIR/environment.txt"

# Set up CinderX based on mode
setup_cinderx() {
    case "$MODE" in
        stock)
            # No CinderX — remove from PYTHONPATH if present
            unset CINDERX_PYTHONPATH
            echo "Mode: stock (no CinderX)"
            ;;
        stub)
            # CinderX with CINDER_UNSUPPORTED — current default aarch64 build
            export PYTHONPATH="${CINDERX_DIR}/scratch/lib.linux-aarch64-cpython-312:${PYTHONPATH:-}"
            echo "Mode: stub (CinderX with JIT stubs)"
            python3 -c "
import cinderx
cinderx.init()
print(f'CinderX loaded: {cinderx.__file__}')
print(f'Supported runtime: {cinderx.is_supported_runtime()}')
print(f'Frame evaluator installed: {cinderx.is_frame_evaluator_installed()}')
" 2>&1 | tee "$REPORT_DIR/cinderx_status.txt"
            ;;
        jit)
            # CinderX with JIT active (CINDER_UNSUPPORTED removed)
            export PYTHONPATH="${CINDERX_DIR}/scratch/lib.linux-aarch64-cpython-312:${PYTHONPATH:-}"
            echo "Mode: jit (CinderX with ARM JIT active)"
            python3 -c "
import cinderx
cinderx.init()
cinderx.install_frame_evaluator()
print(f'CinderX loaded: {cinderx.__file__}')
print(f'Supported runtime: {cinderx.is_supported_runtime()}')
print(f'Frame evaluator installed: {cinderx.is_frame_evaluator_installed()}')
" 2>&1 | tee "$REPORT_DIR/cinderx_status.txt"
            ;;
    esac
}

# Core test files for PyTorch CPU suite
# These are the most important — they exercise the computation paths CinderX JIT affects
CPU_TEST_FILES=(
    "test_torch"
    "test_autograd"
    "test_nn"
    "test_binary_ufuncs"
    "test_unary_ufuncs"
    "test_reductions"
    "test_linalg"
    "test_type_promotion"
    "test_complex"
    "test_view_ops"
    "test_indexing"
    "test_sort_and_select"
    "test_shape_ops"
    "test_autocast"
    "test_custom_ops"
)

# GPU test files
GPU_TEST_FILES=(
    "test_cuda"
    "test_cuda_multigpu"
    "test_cuda_primary_ctx"
)

# Run a single test file and capture structured output
run_test_file() {
    local test_name="$1"
    local test_file="$PYTORCH_TEST_DIR/${test_name}.py"
    local result_file="$REPORT_DIR/${test_name}.json"
    local log_file="$REPORT_DIR/${test_name}.log"

    if [ ! -f "$test_file" ]; then
        echo "SKIP: $test_file not found"
        echo '{"status":"skip","reason":"file_not_found"}' > "$result_file"
        return
    fi

    echo -n "  Running $test_name ... "
    local start_time
    start_time=$(date +%s)

    # Run with pytest, capture XML output for structured results
    local xml_file="$REPORT_DIR/${test_name}_junit.xml"
    local exit_code=0

    cd "$PYTORCH_TEST_DIR"
    timeout 600 python3 -m pytest "$test_file" \
        --timeout=120 \
        -x \
        --tb=short \
        --no-header \
        -q \
        --junit-xml="$xml_file" \
        > "$log_file" 2>&1 || exit_code=$?

    local end_time
    end_time=$(date +%s)
    local duration=$((end_time - start_time))

    # Parse pytest output for counts
    local passed=0 failed=0 errors=0 skipped=0
    if [ -f "$xml_file" ]; then
        # Extract from JUnit XML
        passed=$(python3 -c "
import xml.etree.ElementTree as ET
try:
    tree = ET.parse('$xml_file')
    root = tree.getroot()
    ts = root.find('.//testsuite')
    if ts is not None:
        tests = int(ts.get('tests', 0))
        failures = int(ts.get('failures', 0))
        errors = int(ts.get('errors', 0))
        skipped = int(ts.get('skipped', 0))
        print(tests - failures - errors - skipped)
    else:
        print(0)
except:
    print(0)
" 2>/dev/null)
        failed=$(python3 -c "
import xml.etree.ElementTree as ET
try:
    tree = ET.parse('$xml_file')
    root = tree.getroot()
    ts = root.find('.//testsuite')
    print(int(ts.get('failures', 0)) if ts is not None else 0)
except:
    print(0)
" 2>/dev/null)
        errors=$(python3 -c "
import xml.etree.ElementTree as ET
try:
    tree = ET.parse('$xml_file')
    root = tree.getroot()
    ts = root.find('.//testsuite')
    print(int(ts.get('errors', 0)) if ts is not None else 0)
except:
    print(0)
" 2>/dev/null)
        skipped=$(python3 -c "
import xml.etree.ElementTree as ET
try:
    tree = ET.parse('$xml_file')
    root = tree.getroot()
    ts = root.find('.//testsuite')
    print(int(ts.get('skipped', 0)) if ts is not None else 0)
except:
    print(0)
" 2>/dev/null)
    fi

    # Write structured result
    python3 -c "
import json
result = {
    'test_name': '$test_name',
    'mode': '$MODE',
    'exit_code': $exit_code,
    'duration_s': $duration,
    'passed': $passed,
    'failed': $failed,
    'errors': $errors,
    'skipped': $skipped,
    'has_junit_xml': True if '$xml_file' else False,
}
with open('$result_file', 'w') as f:
    json.dump(result, f, indent=2)
"

    # Summary line
    if [ "$exit_code" -eq 0 ]; then
        echo "PASS (${passed}p/${skipped}s, ${duration}s)"
    elif [ "$exit_code" -eq 124 ]; then
        echo "TIMEOUT (${duration}s)"
    else
        echo "FAIL (${passed}p/${failed}f/${errors}e/${skipped}s, ${duration}s)"
    fi
}

# Main execution
setup_cinderx

echo ""
echo "=== CPU Test Suite ==="
for test in "${CPU_TEST_FILES[@]}"; do
    run_test_file "$test"
done

echo ""
echo "=== GPU Test Suite ==="
# Check CUDA availability first
CUDA_AVAILABLE=$(python3 -c "
try:
    import torch
    print('yes' if torch.cuda.is_available() else 'no')
except:
    print('no')
" 2>/dev/null)

if [ "$CUDA_AVAILABLE" = "yes" ]; then
    for test in "${GPU_TEST_FILES[@]}"; do
        run_test_file "$test"
    done
else
    echo "  CUDA not available — skipping GPU tests"
    echo '{"status":"skip","reason":"cuda_not_available"}' > "$REPORT_DIR/gpu_skipped.json"
fi

# Generate summary CSV
echo ""
echo "=== Generating Summary ==="
{
    echo "test_name,mode,exit_code,duration_s,passed,failed,errors,skipped"
    for result_file in "$REPORT_DIR"/*.json; do
        [ -f "$result_file" ] || continue
        python3 -c "
import json, sys
try:
    with open('$result_file') as f:
        r = json.load(f)
    if 'test_name' in r:
        print(f\"{r['test_name']},{r['mode']},{r['exit_code']},{r['duration_s']},{r['passed']},{r['failed']},{r['errors']},{r['skipped']}\")
except:
    pass
" 2>/dev/null
    done
} > "$REPORT_DIR/summary.csv"

# Print summary
echo ""
echo "=== Results Summary ==="
echo "Mode: $MODE"
echo "Output: $REPORT_DIR"
cat "$REPORT_DIR/summary.csv" | column -t -s',' 2>/dev/null || cat "$REPORT_DIR/summary.csv"

# Calculate totals
python3 -c "
import csv
totals = {'passed': 0, 'failed': 0, 'errors': 0, 'skipped': 0}
with open('$REPORT_DIR/summary.csv') as f:
    reader = csv.DictReader(f)
    for row in reader:
        for key in totals:
            totals[key] += int(row.get(key, 0))
print(f\"\\nTotals: {totals['passed']} passed, {totals['failed']} failed, {totals['errors']} errors, {totals['skipped']} skipped\")
" 2>/dev/null

echo ""
echo "=== Baseline capture complete ==="
