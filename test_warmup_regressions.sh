#!/bin/bash
# Test warmup hypothesis for 5 regressing benchmarks
# Runs each with higher warmup (15000+ calls vs original 200-500)
# Uses ABBA pattern: spec ON, spec OFF, spec OFF, spec ON

set -euo pipefail

PYTHON="/data/users/alexturner/cinderx_dev/python-3.12/python"
BENCHMARKS_DIR="/tmp/warmup_test_$$"
mkdir -p "$BENCHMARKS_DIR"

echo "=== Warmup Regression Test ==="
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "Testing 5 benchmarks with warmup=15000"
echo ""

# Extract each benchmark from the main script
SCRIPT="/data/users/alexturner/cinderx_dev/cinderx/benchmark_specialisation.sh"

for bench in deep_class kwargs_dispatch dunder_protocol nn_module_forward context_manager; do
  # Extract the Python code between the PYEOF markers for this benchmark
  sed -n "/cat > .*\/${bench}\.py.*<< 'PYEOF'/,/^PYEOF$/p" "$SCRIPT" | \
    sed '1d' | sed '$d' > "$BENCHMARKS_DIR/${bench}.py"

  # Fix warmup: replace low-iteration warmup with 15000
  if [ "$bench" = "deep_class" ]; then
    # deep_class has: for _ in range(100): benchmark_deep_class(iterations=5)
    sed -i 's/for _ in range(100): benchmark_deep_class(iterations=5)/for _ in range(3000): benchmark_deep_class(iterations=5)/' "$BENCHMARKS_DIR/${bench}.py"
  else
    # Others have: benchmark_XXX(iterations=200)
    # The warmup call is the one BEFORE start = time.perf_counter_ns()
    # Replace iterations=200 in the warmup call only (first occurrence)
    sed -i "0,/benchmark_${bench}(iterations=200)/s/benchmark_${bench}(iterations=200)/benchmark_${bench}(iterations=15000)/" "$BENCHMARKS_DIR/${bench}.py"
  fi
done

echo "Benchmark                  Spec ON(ms)  Spec OFF(ms)      A/B"
echo "───────────────────────── ──────────── ──────────── ────────"

for bench in deep_class kwargs_dispatch dunder_protocol nn_module_forward context_manager; do
  # ABBA pattern: A1 B1 B2 A2
  a1=$(CINDERX_ENABLE_SPECIALIZED_OPCODES=1 "$PYTHON" "$BENCHMARKS_DIR/${bench}.py" 2>/dev/null)
  b1=$(CINDERX_ENABLE_SPECIALIZED_OPCODES=0 "$PYTHON" "$BENCHMARKS_DIR/${bench}.py" 2>/dev/null)
  b2=$(CINDERX_ENABLE_SPECIALIZED_OPCODES=0 "$PYTHON" "$BENCHMARKS_DIR/${bench}.py" 2>/dev/null)
  a2=$(CINDERX_ENABLE_SPECIALIZED_OPCODES=1 "$PYTHON" "$BENCHMARKS_DIR/${bench}.py" 2>/dev/null)

  # Use inner pair minimums for ABBA
  spec_on=$(python3 -c "print(f'{min(float(\"$a1\"), float(\"$a2\")):.3f}')")
  spec_off=$(python3 -c "print(f'{min(float(\"$b1\"), float(\"$b2\")):.3f}')")
  ratio=$(python3 -c "a=float(\"$spec_on\"); b=float(\"$spec_off\"); print(f'{b/a:.2f}x')")

  printf "%-25s %11s  %11s  %7s\n" "$bench" "$spec_on" "$spec_off" "$ratio"
done

rm -rf "$BENCHMARKS_DIR"
echo ""
echo "A/B > 1.0 means spec ON is faster"
echo "If regressions disappear (A/B >= 0.95), warmup was the cause"
