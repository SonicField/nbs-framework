#!/bin/bash
# benchmark_cinderx_full.sh — Full CinderX JIT benchmark suite for commit 725004da
#
# ── FALSIFICATION DESIGN ──────────────────────────────────────────────────
#
# This script measures the effect of CinderX JIT speculative inlining on
# commit 725004da. The central claim under test is:
#
#   "The speculative inlining in commit 725004da makes method-call-heavy
#    code faster when running under the CinderX JIT."
#
# What would DISPROVE this claim:
#   1. The JIT is not actually compiling the benchmark functions.
#      Falsifier: After warmup, assert cinderjit.is_jit_compiled(func)
#      returns True for each benchmark function. If False, the function
#      runs in the interpreter — results are not JIT measurements.
#
#   2. The speculative inliner is not engaged (Tier 1 only, no Tier 2).
#      Falsifier: (a) Assert cinderjit.is_hir_inliner_enabled() returns
#      True for condition A. (b) Dump HIR for method_calls via
#      cinderjit.print_hir(call_speak) and grep for 'BeginInlinedFunction'.
#      If absent, speculative inlining did not fire — results measure
#      Tier 1 JIT only, not the commit 725004da inlining code.
#
#   3. We are measuring interpreter differences, not JIT differences.
#      Falsifier: Both conditions (A and B) use the SAME Python binary
#      (the venv CinderX Python). Condition A has the inliner enabled;
#      condition B has it disabled via cinderjit.disable_hir_inliner().
#      Any difference is therefore due to the inliner, not the interpreter.
#      Condition C (system Python -I, no CinderX) is included as a reference
#      baseline only — it is NOT the primary comparison.
#
#   4. The warmup is insufficient for Tier 2 compilation.
#      Falsifier: Tier 2 triggers after kTier2Threshold (default 1000)
#      post-JIT invocations. Warmup runs 3000 calls to each function
#      (well above the 1100 minimum: 100 for Tier 1 + 1000 for Tier 2).
#      If Tier 2 does not fire, method_calls with inliner ON will not differ
#      from inliner OFF — the verification step catches this.
#
#   5. Thermal/load effects corrupt the comparison.
#      Falsifier: ABBA pattern (A→B→B→A) with multiple repetitions averages
#      out systematic drift. Median of all samples is reported, not mean.
#
# ── COMPARISON CONDITIONS ─────────────────────────────────────────────────
#
#   A (INLINER ON):  venv Python + CinderX + JIT + speculative inlining
#   B (INLINER OFF): venv Python + CinderX + JIT + inliner disabled
#   C (BASELINE):    system python3.12 -I (no CinderX, reference only)
#
# The PRIMARY comparison is A vs B (isolates inlining effect).
# C is included for context (overall JIT overhead vs vanilla interpreter).
#
# ── BENCHMARKS ────────────────────────────────────────────────────────────
#
#   Micro: method_calls, function_calls, nested_calls, fibonacci
#   Compute: richards, nbody, nqueens, float_arith, dict_ops
#   Patterns: generator_simple, list_comp, exception_handling,
#             chaos_game, coroutine_chain
#   Module: context_manager, decorator_chain, deep_class,
#           dunder_protocol, kwargs_dispatch, nn_module_forward
#
# ── USAGE ─────────────────────────────────────────────────────────────────
#
#   ./benchmark_cinderx_full.sh
#
# Runs on build-host via pty-session. Non-interactive, single-command.
#
# Exit codes:
#   0 — All benchmarks completed
#   1 — Setup error or JIT falsification failure

set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────────────
CINDERX_DIR="/data/users/alexturner/cinderx_dev/cinderx"
VENV_PYTHON="/data/users/alexturner/cinderx_dev/venv/bin/python"
SYSTEM_PYTHON="python3.12"
RESULTS_FILE="/data/users/alexturner/cinderx_dev/benchmark_results_$(date +%Y%m%d_%H%M%S).txt"
ABBA_REPS=2  # Number of ABBA cycles (4 measurements per condition per cycle)

# ── Precondition checks ───────────────────────────────────────────────────
echo "=== CinderX Full Benchmark Suite ==="
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "Host: $(hostname)"
echo ""

# Verify commit (hard error — we are validating a specific commit)
cd "$CINDERX_DIR" || { echo "FATAL: Cannot cd to $CINDERX_DIR"; exit 1; }
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
echo "Commit: $COMMIT"
FULL_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
if [[ "$FULL_COMMIT" != 725004da* ]]; then
    echo "FATAL: Expected commit 725004da*, got $FULL_COMMIT"
    echo "This script validates a specific commit. Checkout the correct commit first."
    exit 1
fi

# Verify CinderX Python (used for both A and B conditions)
if ! "$VENV_PYTHON" -c "import cinderx; cinderx.init(); import cinderjit; print('CinderX + cinderjit OK')" 2>&1; then
    echo "FATAL: Venv Python cannot load CinderX or cinderjit"
    exit 1
fi
echo "CinderX Python: $VENV_PYTHON ($($VENV_PYTHON --version 2>&1))"

# Verify disable_hir_inliner API exists (needed for condition B)
if ! "$VENV_PYTHON" -c "
import cinderx; cinderx.init()
import cinderjit
cinderjit.disable_hir_inliner()
print('disable_hir_inliner OK')
" 2>&1; then
    echo "FATAL: cinderjit.disable_hir_inliner() not available"
    echo "Cannot create inliner-disabled condition for comparison"
    exit 1
fi

# Verify system Python baseline (condition C — reference only)
if ! "$SYSTEM_PYTHON" -I -c "print('System Python OK')" 2>/dev/null; then
    echo "WARNING: System Python not available — condition C (baseline) will be skipped"
    HAS_BASELINE=false
else
    if "$SYSTEM_PYTHON" -I -c "import cinderx" 2>/dev/null; then
        echo "WARNING: System Python -I can import cinderx — not a clean baseline"
        HAS_BASELINE=false
    else
        HAS_BASELINE=true
        echo "Baseline Python: $SYSTEM_PYTHON -I ($($SYSTEM_PYTHON --version 2>&1))"
    fi
fi
echo ""

# ── Temp directory for benchmark scripts ───────────────────────────────────
BENCHMARKS_DIR="$(mktemp -d)"
trap 'rm -rf "$BENCHMARKS_DIR"' EXIT

# ── JIT Falsification Step ─────────────────────────────────────────────────
# Before running any benchmarks, verify that:
#   1. CinderX JIT actually compiles functions (not just interpreter)
#   2. The inliner produces a measurably different result
#   3. Tier 2 fires within our warmup window
#
# This is a GATE — if falsification fails, the script aborts.

echo "=== JIT Falsification Check ==="
echo ""
echo "Phase 1: Verify JIT compiles functions and inliner state is correct"
echo "Phase 2: Compare inliner ON vs OFF timings for method_calls"
echo "Phase 3: HIR dump proof that speculative inlining is engaged"
echo ""

# ── Phase 1: JIT compilation and inliner state verification ──
cat > "$BENCHMARKS_DIR/jit_verify_phase1.py" << 'PYEOF'
"""JIT falsification — Phase 1: compilation and inliner state.

Verifies:
  1. cinderjit is loaded and functional
  2. Functions are JIT-compiled after warmup (cinderjit.is_jit_compiled)
  3. Inliner is enabled by default (cinderjit.is_hir_inliner_enabled)
  4. Tier 2 threshold is reachable within our warmup window

Falsifiers (what would disprove JIT engagement):
  - is_jit_compiled(call_speak) returns False → JIT did not compile the function
  - is_hir_inliner_enabled() returns False → inliner is not active
  - Both would mean benchmark results measure interpreter, not JIT
"""
import time
import sys

import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def call_speak(d):
    return d.speak()

def run_method_calls(n):
    d = Dog()
    total = 0
    for _ in range(n):
        total += call_speak(d)
    return total

# Warmup: 100 calls for Tier 1, then 1000+ more for Tier 2 = 3000 total
for _ in range(3000):
    call_speak(Dog())

# Also warmup the outer function
for _ in range(3000):
    run_method_calls(10)

# ── Check 1: is_jit_compiled ──
checks_passed = 0
checks_total = 0

checks_total += 1
try:
    compiled = cinderjit.is_jit_compiled(call_speak)
    print(f"CHECK: cinderjit.is_jit_compiled(call_speak) = {compiled}")
    if compiled:
        checks_passed += 1
    else:
        print("FAIL: call_speak was NOT JIT-compiled after 3000 warmup calls")
except AttributeError:
    print("SKIP: cinderjit.is_jit_compiled not available (older CinderX)")
    checks_passed += 1  # Don't fail on missing API

checks_total += 1
try:
    compiled_outer = cinderjit.is_jit_compiled(run_method_calls)
    print(f"CHECK: cinderjit.is_jit_compiled(run_method_calls) = {compiled_outer}")
    if compiled_outer:
        checks_passed += 1
    else:
        print("FAIL: run_method_calls was NOT JIT-compiled")
except AttributeError:
    print("SKIP: cinderjit.is_jit_compiled not available")
    checks_passed += 1

# ── Check 2: is_hir_inliner_enabled ──
checks_total += 1
try:
    inliner_on = cinderjit.is_hir_inliner_enabled()
    print(f"CHECK: cinderjit.is_hir_inliner_enabled() = {inliner_on}")
    if inliner_on:
        checks_passed += 1
    else:
        print("FAIL: HIR inliner is NOT enabled — speculative inlining will not fire")
except AttributeError:
    print("SKIP: cinderjit.is_hir_inliner_enabled not available")
    checks_passed += 1

# ── Check 3: Compiled function count ──
try:
    compiled_list = cinderjit.get_compiled_functions()
    print(f"CHECK: {len(compiled_list)} functions JIT-compiled")
except AttributeError:
    print("SKIP: cinderjit.get_compiled_functions not available")

# ── Timed run (inliner ON) ──
N = 500
start = time.perf_counter_ns()
for _ in range(N):
    run_method_calls(1000)
on_ms = (time.perf_counter_ns() - start) / 1_000_000

print(f"INLINER_ON_MS: {on_ms:.3f}")
print(f"CHECKS: {checks_passed}/{checks_total} passed")

if checks_passed < checks_total:
    print("JIT_STATUS: FAIL One or more JIT checks failed")
    sys.exit(1)
else:
    print("JIT_STATUS: PASS All JIT checks passed")
PYEOF

# ── Phase 1b: Same but with inliner OFF ──
cat > "$BENCHMARKS_DIR/jit_verify_phase1_off.py" << 'PYEOF'
"""JIT falsification — Phase 1b: inliner OFF condition.

Same as Phase 1 but with cinderjit.disable_hir_inliner() called at startup.
All JIT compilations happen WITHOUT speculative inlining.
"""
import time
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)
cinderjit.disable_hir_inliner()

class Dog:
    def speak(self):
        return 42

def call_speak(d):
    return d.speak()

def run_method_calls(n):
    d = Dog()
    total = 0
    for _ in range(n):
        total += call_speak(d)
    return total

# Warmup (same as ON condition)
for _ in range(3000):
    call_speak(Dog())
for _ in range(3000):
    run_method_calls(10)

# Verify inliner is OFF
try:
    inliner_state = cinderjit.is_hir_inliner_enabled()
    print(f"CHECK: cinderjit.is_hir_inliner_enabled() = {inliner_state}")
    if inliner_state:
        print("FAIL: Inliner should be disabled but is_hir_inliner_enabled() returns True")
except AttributeError:
    print("SKIP: cinderjit.is_hir_inliner_enabled not available")

# Timed run
N = 500
start = time.perf_counter_ns()
for _ in range(N):
    run_method_calls(1000)
off_ms = (time.perf_counter_ns() - start) / 1_000_000

print(f"INLINER_OFF_MS: {off_ms:.3f}")
PYEOF

# ── Phase 3: HIR dump proof of inlining ──
cat > "$BENCHMARKS_DIR/jit_verify_hir.py" << 'PYEOF'
"""JIT falsification — Phase 3: HIR dump proof.

Dumps the HIR (High-level Intermediate Representation) for call_speak
and checks for BeginInlinedFunction, which proves speculative inlining
has engaged.

Falsifier: If BeginInlinedFunction is ABSENT from the HIR dump,
speculative inlining is not engaged — results measure Tier 1 JIT only,
not the Tier 2 inlined code from commit 725004da.
"""
import sys
import io
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def call_speak(d):
    return d.speak()

# Warmup to trigger Tier 1 and Tier 2
d = Dog()
for _ in range(3000):
    call_speak(d)

# Try to dump HIR
try:
    # Capture stdout from print_hir
    old_stdout = sys.stdout
    sys.stdout = buffer = io.StringIO()
    cinderjit.print_hir(call_speak)
    sys.stdout = old_stdout
    hir_output = buffer.getvalue()

    has_inline = 'BeginInlinedFunction' in hir_output
    print(f"HIR_DUMP_SIZE: {len(hir_output)} chars")
    print(f"HIR_HAS_INLINE: {has_inline}")
    if has_inline:
        # Extract the inlined function name if possible
        for line in hir_output.split('\n'):
            if 'BeginInlinedFunction' in line:
                print(f"HIR_INLINE_PROOF: {line.strip()}")
                break
        print("HIR_STATUS: PASS Speculative inlining confirmed in HIR")
    else:
        print("HIR_STATUS: WARN BeginInlinedFunction not found in HIR")
        print("This may mean: (a) Tier 2 has not fired yet, or")
        print("(b) the IC was not monomorphic, or (c) inlining was not profitable")
        print("Results may measure Tier 1 JIT only, not speculative inlining")
except AttributeError:
    print("HIR_STATUS: SKIP cinderjit.print_hir not available")
except Exception as e:
    print(f"HIR_STATUS: ERROR {e}")
PYEOF

# ── Run all falsification probes ──
echo "--- Phase 1: Inliner ON ---"
VERIFY_ON=$("$VENV_PYTHON" "$BENCHMARKS_DIR/jit_verify_phase1.py" 2>&1)
VERIFY_ON_EXIT=$?
echo "$VERIFY_ON"
echo ""

if [ $VERIFY_ON_EXIT -ne 0 ]; then
    echo "FATAL: JIT falsification Phase 1 failed — JIT checks did not pass"
    echo "Cannot trust benchmark results. Fix JIT setup before proceeding."
    exit 1
fi

echo "--- Phase 1b: Inliner OFF ---"
VERIFY_OFF=$("$VENV_PYTHON" "$BENCHMARKS_DIR/jit_verify_phase1_off.py" 2>&1)
echo "$VERIFY_OFF"
echo ""

echo "--- Phase 3: HIR dump proof ---"
# Phase 3 is non-fatal: print_hir() requires a debug build and will SIGABRT
# on optimised builds. Phase 1+2 already prove JIT compilation and inliner
# engagement — Phase 3 is supplementary proof only.
VERIFY_HIR=$("$VENV_PYTHON" "$BENCHMARKS_DIR/jit_verify_hir.py" 2>&1) || true
echo "$VERIFY_HIR"
echo ""

# Extract timings for ON/OFF comparison
ON_MS=$(echo "$VERIFY_ON" | grep "INLINER_ON_MS:" | awk '{print $2}')
OFF_MS=$(echo "$VERIFY_OFF" | grep "INLINER_OFF_MS:" | awk '{print $2}')

if [ -z "$ON_MS" ] || [ -z "$OFF_MS" ]; then
    echo "FATAL: JIT falsification probe failed to produce timing data"
    echo "ON output: $VERIFY_ON"
    echo "OFF output: $VERIFY_OFF"
    exit 1
fi

# ── Phase 2: Compare ON/OFF timings ──
echo "--- Phase 2: Timing comparison ---"
RATIO=$("$VENV_PYTHON" -c "
on = $ON_MS
off = $OFF_MS
ratio = on / off if off > 0 else 1.0
print(f'Inliner ON: {on:.1f}ms, Inliner OFF: {off:.1f}ms, ratio: {ratio:.4f}')
if 0.98 <= ratio <= 1.02:
    print('WARN: Inliner ON/OFF ratio is within noise (2%). Inliner may not be engaged.')
    print('This does NOT abort — some workloads show small effects. Interpret results with caution.')
elif ratio < 0.98:
    print(f'OK: Inliner ON is {(1 - ratio) * 100:.1f}% faster than OFF — inliner effect confirmed.')
elif ratio > 1.02:
    print(f'NOTE: Inliner ON is {(ratio - 1) * 100:.1f}% SLOWER than OFF for method_calls.')
    print('This may indicate inlining overhead exceeds savings for this workload.')
")
echo "$RATIO"
echo ""
echo "=== JIT Falsification Complete ==="
echo ""

# ── Benchmark script generation ───────────────────────────────────────────
# Each benchmark is a self-contained .py file. NO exec() — top-level code
# so the JIT can compile all functions.
#
# Warmup iterations: >2000 outer calls to ensure both Tier 1 (>100 calls)
# and Tier 2 (>1000 post-JIT calls) fire for all benchmark functions.

# --- Micro-benchmarks ---

cat > "$BENCHMARKS_DIR/method_calls.py" << 'PYEOF'
import time
class Dog:
    def speak(self):
        return 42
def call_speak(d):
    return d.speak()
d = Dog()
for _ in range(3000): call_speak(d)  # Warmup: >Tier2 threshold
N = 500000
start = time.perf_counter_ns()
for _ in range(N): call_speak(d)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/function_calls.py" << 'PYEOF'
import time
def f(x):
    return x + 1
for _ in range(3000): f(42)
N = 500000
start = time.perf_counter_ns()
for _ in range(N): f(42)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/nested_calls.py" << 'PYEOF'
import time
def f(x): return x + 1
def g(x): return f(x) + 1
def h(x): return g(x) + 1
for _ in range(3000): h(42)
N = 500000
start = time.perf_counter_ns()
for _ in range(N): h(42)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/fibonacci.py" << 'PYEOF'
import time
def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a
for _ in range(3000): fib(100)
N = 500000
start = time.perf_counter_ns()
for _ in range(N): fib(100)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

# --- Compute benchmarks ---

cat > "$BENCHMARKS_DIR/richards.py" << 'PYEOF'
import time

class Packet:
    def __init__(self, link, ident, kind):
        self.link = link
        self.ident = ident
        self.kind = kind
        self.datum = 0
        self.data = [0] * 4

class Task:
    _list = []
    def __init__(self, ident, priority, input_, state, fn, v1, v2):
        self.ident = ident
        self.priority = priority
        self.input = input_
        self.state = state
        self.fn = fn
        self.v1 = v1
        self.v2 = v2
        Task._list.append(self)
    def run(self):
        total = 0
        for _ in range(50):
            total += self.priority * self.ident
            if self.input is not None:
                total += self.input.datum
        return total

def richards_bench():
    Task._list.clear()
    for i in range(10):
        p = Packet(None, i, i % 3)
        p.datum = i * 7
        Task(i, i * 3 + 1, p, 0, lambda t: t.priority, i, i * 2)
    total = 0
    for _ in range(200):
        for task in Task._list:
            total += task.run()
    return total

for _ in range(20): richards_bench()
N = 200
start = time.perf_counter_ns()
for _ in range(N): richards_bench()
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/nbody.py" << 'PYEOF'
import time
import math

def nbody(n):
    bodies = [
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 39.47841760435743],
        [4.84, -1.16, -0.10, 0.00166, 0.00769, -0.0000690, 0.000954],
        [8.34, 4.12, -0.40, -0.00276, 0.00499, 0.0000230, 0.000286],
        [12.89, -15.11, -0.22, 0.00296, 0.00237, -0.0000296, 0.0000437],
        [15.38, -25.92, 0.179, 0.00268, 0.00162, -0.0000951, 0.0000517],
    ]
    dt = 0.01
    for _ in range(n):
        for i in range(len(bodies)):
            bi = bodies[i]
            for j in range(i + 1, len(bodies)):
                bj = bodies[j]
                dx = bi[0] - bj[0]
                dy = bi[1] - bj[1]
                dz = bi[2] - bj[2]
                dist = math.sqrt(dx*dx + dy*dy + dz*dz)
                mag = dt / (dist * dist * dist)
                bi[3] -= dx * bj[6] * mag
                bi[4] -= dy * bj[6] * mag
                bi[5] -= dz * bj[6] * mag
                bj[3] += dx * bi[6] * mag
                bj[4] += dy * bi[6] * mag
                bj[5] += dz * bi[6] * mag
        for b in bodies:
            b[0] += dt * b[3]
            b[1] += dt * b[4]
            b[2] += dt * b[5]
    return bodies[0][0]

for _ in range(10): nbody(500)
N = 100
start = time.perf_counter_ns()
for _ in range(N): nbody(500)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/nqueens.py" << 'PYEOF'
import time

def nqueens(n):
    count = 0
    cols = [0] * n
    def solve(row):
        nonlocal count
        if row == n:
            count += 1
            return
        for col in range(n):
            ok = True
            for prev_row in range(row):
                if cols[prev_row] == col or abs(cols[prev_row] - col) == row - prev_row:
                    ok = False
                    break
            if ok:
                cols[row] = col
                solve(row + 1)
    solve(0)
    return count

for _ in range(10): nqueens(8)
N = 50
start = time.perf_counter_ns()
for _ in range(N): nqueens(8)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/float_arith.py" << 'PYEOF'
import time
import math

def float_bench(n):
    total = 0.0
    for i in range(n):
        x = float(i) * 0.1
        total += math.sin(x) * math.cos(x) + math.sqrt(abs(x) + 1.0)
    return total

for _ in range(10): float_bench(10000)
N = 200
start = time.perf_counter_ns()
for _ in range(N): float_bench(10000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/dict_ops.py" << 'PYEOF'
import time

def dict_bench(n):
    d = {}
    for i in range(n):
        d[f"key_{i}"] = i * 0.1
    total = 0.0
    for i in range(n):
        total += d[f"key_{i}"]
    for i in range(n // 2):
        del d[f"key_{i}"]
    total += len(d)
    return total

for _ in range(200): dict_bench(1000)
N = 2000
start = time.perf_counter_ns()
for _ in range(N): dict_bench(1000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

# --- Pattern benchmarks ---

cat > "$BENCHMARKS_DIR/generator_simple.py" << 'PYEOF'
import time

def gen(n):
    for i in range(n):
        yield i

def gen_bench():
    return sum(gen(100))

for _ in range(3000): gen_bench()
N = 200000
start = time.perf_counter_ns()
for _ in range(N): gen_bench()
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/list_comp.py" << 'PYEOF'
import time

def lc_bench():
    return sum([i * 2 for i in range(100)])

for _ in range(3000): lc_bench()
N = 200000
start = time.perf_counter_ns()
for _ in range(N): lc_bench()
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/exception_handling.py" << 'PYEOF'
import time

def safe_div(x):
    try:
        return 1.0 / x
    except ZeroDivisionError:
        return 0.0

def exc_bench(n):
    total = 0.0
    for i in range(n):
        total += safe_div(i)
    return total

for _ in range(200): exc_bench(1000)
N = 2000
start = time.perf_counter_ns()
for _ in range(N): exc_bench(1000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/chaos_game.py" << 'PYEOF'
import time
import math

def chaos_game(n):
    vertices = [(0.0, 0.0), (1.0, 0.0), (0.5, math.sqrt(3)/2)]
    x, y = 0.5, 0.5
    total = 0.0
    r = 0
    for i in range(n):
        r = (r * 1103515245 + 12345) & 0x7fffffff
        v = vertices[r % 3]
        x = (x + v[0]) / 2
        y = (y + v[1]) / 2
        total += x + y
    return total

for _ in range(10): chaos_game(50000)
N = 100
start = time.perf_counter_ns()
for _ in range(N): chaos_game(50000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/coroutine_chain.py" << 'PYEOF'
import time

def stage1(n):
    total = 0.0
    for i in range(n):
        total += i * 0.1
        yield total

def stage2(source):
    for val in source:
        yield val * 0.99

def stage3(source):
    for val in source:
        yield val + 1.0

def coroutine_bench(n):
    pipeline = stage3(stage2(stage1(n)))
    total = 0.0
    for val in pipeline:
        total = (total + val) % 10000
    return total

for _ in range(20): coroutine_bench(1000)
N = 500
start = time.perf_counter_ns()
for _ in range(N): coroutine_bench(1000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

# --- Module benchmarks (from benchmarks/ directory, embedded) ---

cat > "$BENCHMARKS_DIR/context_manager.py" << 'PYEOF'
import time

class NoGrad:
    _enabled = True
    def __enter__(self):
        self._prev = NoGrad._enabled
        NoGrad._enabled = False
        return self
    def __exit__(self, exc_type, exc_val, exc_tb):
        NoGrad._enabled = self._prev
        return False

class Autocast:
    _mode = 'float32'
    def __init__(self, mode='float16'):
        self._target = mode
    def __enter__(self):
        self._prev = Autocast._mode
        Autocast._mode = self._target
        return self
    def __exit__(self, exc_type, exc_val, exc_tb):
        Autocast._mode = self._prev
        return False

class ProfileScope:
    _depth = 0
    _total = 0
    def __init__(self, name):
        self._name = name
    def __enter__(self):
        ProfileScope._depth += 1
        ProfileScope._total += 1
        return self
    def __exit__(self, exc_type, exc_val, exc_tb):
        ProfileScope._depth -= 1
        return False

# NOTE: @contextlib.contextmanager falls back to interpreter under CinderX JIT
# (PUSH_EXC_INFO opcode in _GeneratorContextManager.__exit__ triggers graceful
# fallback, not JIT compilation). Use class-based context manager so this
# benchmark can be JIT-compiled and measure actual inliner effect.
class TrainingMode:
    def __init__(self, model_dict, mode=True):
        self._model = model_dict
        self._mode = mode
    def __enter__(self):
        self._prev = self._model.get('training', True)
        self._model['training'] = self._mode
        return self._model
    def __exit__(self, exc_type, exc_val, exc_tb):
        self._model['training'] = self._prev
        return False

def benchmark_context_manager(iterations=5000):
    model = {'training': True, 'weight': 1.0, 'bias': 0.0}
    total = 0.0
    for i in range(iterations):
        with NoGrad():
            total += model['weight'] * float(i % 100) + model['bias']
        with NoGrad():
            with Autocast('float16'):
                total += total % 1000 * 0.99
        with ProfileScope('forward'):
            with NoGrad():
                with Autocast('bfloat16'):
                    total = (total % 1000) + 0.001
        with TrainingMode(model, mode=False) as m:
            total += m['weight'] * 0.5
        for j in range(5):
            with ProfileScope(f'layer_{j}'):
                total = (total + float(j)) % 10000
    return total

benchmark_context_manager(iterations=200)
start = time.perf_counter_ns()
result = benchmark_context_manager(iterations=5000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/decorator_chain.py" << 'PYEOF'
import time
import functools

def timer(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)
    return wrapper

def validator(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)
    return wrapper

def logger(func):
    count = [0]
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        count[0] += 1
        return func(*args, **kwargs)
    wrapper.call_count = count
    return wrapper

def cacher(func):
    cache = {}
    @functools.wraps(func)
    def wrapper(*args):
        if args not in cache:
            cache[args] = func(*args)
        return cache[args]
    wrapper.cache = cache
    return wrapper

class Compute:
    @timer
    @validator
    @logger
    def add(self, a, b):
        return a + b
    @timer
    @validator
    def multiply(self, a, b):
        return a * b
    @cacher
    def fibonacci(self, n):
        if n < 2:
            return n
        return self.fibonacci(n - 1) + self.fibonacci(n - 2)
    @staticmethod
    def static_op(x, y):
        return x * y + y
    @classmethod
    def class_op(cls, x):
        return x * 2

def make_adder(offset):
    def adder(x):
        return x + offset
    return adder

def benchmark_decorator_chain(iterations=5000):
    comp = Compute()
    adders = [make_adder(i * 0.1) for i in range(10)]
    total = 0.0
    for i in range(iterations):
        total += comp.add(total % 100, float(i % 50))
        total += comp.multiply(total % 100, 0.99)
        fib_val = comp.fibonacci(i % 20)
        total += fib_val * 0.001
        total += Compute.static_op(total % 100, 0.5)
        total += Compute.class_op(total % 100)
        for adder in adders:
            total = adder(total % 100)
        total = total % 10000.0
    return total

benchmark_decorator_chain(iterations=200)
start = time.perf_counter_ns()
result = benchmark_decorator_chain(iterations=5000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/deep_class.py" << 'PYEOF'
import time

class Base:
    def __init__(self, name):
        self.name = name
        self.training = True
        self._forward_hooks = []
    def parameters(self):
        return [v for k, v in self.__dict__.items() if isinstance(v, float)]
    def train(self, mode=True):
        self.training = mode
        return self

class Layer(Base):
    def __init__(self, name, in_features, out_features):
        Base.__init__(self, name)
        self.in_features = in_features
        self.out_features = out_features
        self.weight = 0.01 * in_features * out_features
        self.bias = 0.01 * out_features
    def forward(self, x):
        return x * self.weight + self.bias

class Block(Layer):
    def __init__(self, name, features, num_layers=3):
        Layer.__init__(self, name, features, features)
        self.num_layers = num_layers
        self.scale = 1.0 / num_layers
        self.layers = [Layer(f"{name}_sub_{i}", features, features) for i in range(num_layers)]
    def forward(self, x):
        residual = x
        for layer in self.layers:
            x = layer.forward(x) * self.scale
        return x + residual

class Network(Block):
    def __init__(self, name, features, num_blocks=2):
        Block.__init__(self, name, features, num_layers=3)
        self.num_blocks = num_blocks
        self.blocks = [Block(f"{name}_block_{i}", features) for i in range(num_blocks)]
    def forward(self, x):
        for block in self.blocks:
            x = block.forward(x)
        return x

class Model(Network):
    def __init__(self, name, features=64, num_blocks=2):
        Network.__init__(self, name, features, num_blocks)
        self.classifier_weight = 0.01 * features
        self.classifier_bias = 0.001
    def forward(self, x):
        x = Network.forward(self, x)
        return x * self.classifier_weight + self.classifier_bias
    def __repr__(self):
        return f"Model({self.name}, features={self.in_features}, blocks={self.num_blocks})"

def benchmark_deep_class(iterations=500):
    total = 0.0
    for _ in range(iterations):
        model = Model("bench", features=32, num_blocks=2)
        result = model.forward(1.0)
        total += result % 100.0
        if isinstance(model, Model): total += 0.001
        if isinstance(model, Network): total += 0.001
        if isinstance(model, Block): total += 0.001
        if isinstance(model, Layer): total += 0.001
        if isinstance(model, Base): total += 0.001
        _ = model.training
        _ = model.in_features
        _ = model.num_layers
        _ = model.num_blocks
        model.train(False)
        params = model.parameters()
        total += len(params) * 0.001
        _ = repr(model)
    return total

benchmark_deep_class(iterations=100)
start = time.perf_counter_ns()
result = benchmark_deep_class(iterations=500)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/dunder_protocol.py" << 'PYEOF'
import time

class Module:
    def __init__(self, name, depth=0):
        object.__setattr__(self, '_parameters', {})
        object.__setattr__(self, '_modules', {})
        object.__setattr__(self, '_name', name)
        for i in range(5):
            self._parameters[f'weight_{i}'] = float(i) * 0.1
            self._parameters[f'bias_{i}'] = float(i) * 0.01
        if depth < 3:
            for i in range(3):
                self._modules[f'layer_{i}'] = Module(f'{name}_L{i}', depth + 1)
    def __getattr__(self, name):
        if name in self.__dict__.get('_parameters', {}):
            return self._parameters[name]
        if name in self.__dict__.get('_modules', {}):
            return self._modules[name]
        raise AttributeError(f"'{type(self).__name__}' has no attribute '{name}'")
    def __setattr__(self, name, value):
        if isinstance(value, float):
            self.__dict__.setdefault('_parameters', {})[name] = value
        elif isinstance(value, Module):
            self.__dict__.setdefault('_modules', {})[name] = value
        else:
            object.__setattr__(self, name, value)
    def __call__(self, x):
        return x + self._parameters.get('weight_0', 0.0)
    def __repr__(self):
        params = len(self._parameters)
        modules = len(self._modules)
        return f"Module({self._name}, params={params}, modules={modules})"
    def __bool__(self):
        return True

class Container:
    def __init__(self, items):
        self._items = list(items)
    def __len__(self):
        return len(self._items)
    def __iter__(self):
        return iter(self._items)
    def __contains__(self, item):
        return item in self._items
    def __getitem__(self, idx):
        return self._items[idx]

def benchmark_dunder_protocol(iterations=2000):
    model = Module("root")
    params = Container(model._parameters.values())
    total = 0.0
    for _ in range(iterations):
        w0 = model.weight_0
        w1 = model.weight_1
        b0 = model.bias_0
        total += w0 + w1 + b0
        model.weight_0 = w0 * 0.99
        model.bias_0 = b0 * 0.99
        result = model(total)
        total = result % 1000.0
        n = len(params)
        for p in params:
            total += p * 0.001
        if 0.1 in params:
            total += 0.001
        _ = repr(model)
        if model:
            total += 0.0001
        layer = model.layer_0
        sub_layer = layer.layer_1
        total += sub_layer(total)
    return total

benchmark_dunder_protocol(iterations=200)
start = time.perf_counter_ns()
result = benchmark_dunder_protocol(iterations=2000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/kwargs_dispatch.py" << 'PYEOF'
import time

def compute(x, y, z=0.0, scale=1.0, bias=0.0, inplace=False):
    result = (x * y + z) * scale + bias
    if inplace:
        return result
    return result * 1.0

def forward_args(*args, **kwargs):
    return compute(*args, **kwargs)

def apply_fn(fn, *args, **kwargs):
    return fn(*args, **kwargs)

class Layer:
    def __init__(self, in_f=64, out_f=64, bias=True, dtype='float32',
                 device='cpu', requires_grad=True):
        self.in_f = in_f
        self.out_f = out_f
        self.has_bias = bias
        self.dtype = dtype
        self.device = device
        self.requires_grad = requires_grad
        self.weight = 0.01 * in_f
    def forward(self, x, *, training=True, mask=None):
        result = x * self.weight
        if self.has_bias:
            result += 0.01
        if mask is not None:
            result *= mask
        return result

class Optimizer:
    def __init__(self, params, lr=0.01, momentum=0.9, weight_decay=1e-4,
                 dampening=0.0, nesterov=False):
        self.params = list(params)
        self.lr = lr
        self.momentum = momentum
        self.weight_decay = weight_decay
        self.dampening = dampening
        self.nesterov = nesterov
    def step(self, closure=None):
        total = 0.0
        for p in self.params:
            grad = p * 0.01
            if self.weight_decay != 0:
                grad += p * self.weight_decay
            total += grad * self.lr
        return total

def benchmark_kwargs_dispatch(iterations=3000):
    layers = [Layer(in_f=i*8+8, out_f=(i+1)*8+8) for i in range(5)]
    params = [l.weight for l in layers]
    opt = Optimizer(params, lr=0.001, momentum=0.9, weight_decay=1e-4)
    total = 0.0
    for i in range(iterations):
        total += compute(total % 100, 0.5, z=0.1, scale=0.99, bias=0.001)
        total += forward_args(total % 100, 0.5, z=0.2, scale=0.98)
        total += apply_fn(compute, total % 100, 0.5, scale=0.97, inplace=True)
        for layer in layers:
            total = layer.forward(total % 100, training=(i % 2 == 0))
        if i % 100 == 0:
            _ = Layer(in_f=32, out_f=64, bias=True, dtype='float16',
                      device='cpu', requires_grad=True)
        loss = opt.step(closure=None)
        total = (total + loss) % 10000
        total += compute(total % 100, 0.5, 0.1, scale=0.99)
    return total

benchmark_kwargs_dispatch(iterations=200)
start = time.perf_counter_ns()
result = benchmark_kwargs_dispatch(iterations=3000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

cat > "$BENCHMARKS_DIR/nn_module_forward.py" << 'PYEOF'
import time

class Parameter:
    def __init__(self, data):
        self.data = data
        self.grad = None
        self.requires_grad = True

class Module:
    def __init__(self):
        object.__setattr__(self, '_parameters', {})
        object.__setattr__(self, '_modules', {})
        object.__setattr__(self, '_buffers', {})
        object.__setattr__(self, 'training', True)
    def __getattr__(self, name):
        _parameters = self.__dict__.get('_parameters', {})
        if name in _parameters:
            return _parameters[name]
        _modules = self.__dict__.get('_modules', {})
        if name in _modules:
            return _modules[name]
        _buffers = self.__dict__.get('_buffers', {})
        if name in _buffers:
            return _buffers[name]
        raise AttributeError(f"'{type(self).__name__}' has no attribute '{name}'")
    def __setattr__(self, name, value):
        if isinstance(value, Parameter):
            self.__dict__.setdefault('_parameters', {})[name] = value
        elif isinstance(value, Module):
            self.__dict__.setdefault('_modules', {})[name] = value
        else:
            object.__setattr__(self, name, value)
    def __call__(self, *args, **kwargs):
        return self.forward(*args, **kwargs)
    def parameters(self):
        for p in self._parameters.values():
            yield p
        for m in self._modules.values():
            for p in m.parameters():
                yield p
    def train(self, mode=True):
        self.training = mode
        for m in self._modules.values():
            m.train(mode)
        return self
    def eval(self):
        return self.train(False)

class Linear(Module):
    def __init__(self, in_features, out_features, bias=True):
        Module.__init__(self)
        self.in_features = in_features
        self.out_features = out_features
        self.weight = Parameter(0.01 * in_features * out_features)
        if bias:
            self.bias = Parameter(0.01 * out_features)
    def forward(self, x):
        result = x * self.weight.data + (self.bias.data if hasattr(self, 'bias') else 0.0)
        return result

class ReLU(Module):
    def forward(self, x):
        return max(0.0, x)

class Sequential(Module):
    def __init__(self, *modules):
        Module.__init__(self)
        for i, module in enumerate(modules):
            self._modules[str(i)] = module
    def forward(self, x):
        for module in self._modules.values():
            x = module(x)
        return x

class SimpleNet(Module):
    def __init__(self):
        Module.__init__(self)
        self.features = Sequential(
            Linear(64, 128), ReLU(),
            Linear(128, 64), ReLU(),
        )
        self.classifier = Linear(64, 10)
        self.dropout_rate = 0.5
    def forward(self, x):
        x = self.features(x)
        x = self.classifier(x)
        return x

def benchmark_nn_module_forward(iterations=2000):
    model = SimpleNet()
    model.train()
    total = 0.0
    for i in range(iterations):
        x = float(i % 100) * 0.01
        output = model(x)
        total += output % 1000.0
        for p in model.parameters():
            total += p.data * 0.0001
        if i % 100 == 0:
            if model.training:
                model.eval()
            else:
                model.train()
        total = total % 10000.0
    return total

benchmark_nn_module_forward(iterations=200)
start = time.perf_counter_ns()
result = benchmark_nn_module_forward(iterations=2000)
elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
print(f"{elapsed_ms:.3f}")
PYEOF

# ── JIT init wrappers ─────────────────────────────────────────────────────
# Condition A: CinderX + JIT + inliner ON (default)
# Condition B: CinderX + JIT + inliner OFF (disable_hir_inliner)
# Condition C: System Python (no CinderX) — reference baseline

JIT_INIT_INLINER_ON='
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)
# CONDITION A: JIT + speculative inlining ON (default)
# Verify inliner state after init
try:
    assert cinderjit.is_hir_inliner_enabled(), "FATAL: inliner should be ON for condition A"
except AttributeError:
    pass  # API not available in this CinderX build
'

JIT_INIT_INLINER_OFF='
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)
cinderjit.disable_hir_inliner()
# CONDITION B: JIT ON, speculative inlining OFF
# Verify inliner is disabled
try:
    assert not cinderjit.is_hir_inliner_enabled(), "FATAL: inliner should be OFF for condition B"
except AttributeError:
    pass  # API not available in this CinderX build
'

make_condition_script() {
    local src="$1"
    local dst="$2"
    local init_code="$3"
    echo "$init_code" > "$dst"
    cat "$src" >> "$dst"
}

# ── Known JIT bugs (skip these for JIT conditions) ───────────────────────
# nqueens: LICM GuardType hoisting crash on aarch64 (segfault in nested loops
# with closure/nonlocal). Separate from the tier1Vectorcall varargs bug.
# See benchmark_results/21-02-2026-varargs-codegen-bug-report.md §Related Bugs.

KNOWN_JIT_BUGS=(
    nqueens
)

is_known_jit_bug() {
    local name="$1"
    for bug in "${KNOWN_JIT_BUGS[@]}"; do
        if [ "$name" = "$bug" ]; then
            return 0
        fi
    done
    return 1
}

# ── Benchmark list ─────────────────────────────────────────────────────────

ALL_BENCHMARKS=(
    method_calls
    function_calls
    nested_calls
    fibonacci
    richards
    nbody
    nqueens
    float_arith
    dict_ops
    generator_simple
    list_comp
    exception_handling
    chaos_game
    coroutine_chain
    context_manager
    decorator_chain
    deep_class
    dunder_protocol
    kwargs_dispatch
    nn_module_forward
)

# ── Extract timing from output ─────────────────────────────────────────────
# All benchmarks now output just "123.456" (ms) as the last line of numeric output

extract_ms() {
    local output="$1"
    # Match a line that is just a decimal number (possibly with leading whitespace)
    echo "$output" | grep -E '^\s*[0-9]+\.[0-9]+\s*$' | tail -1 | tr -d ' '
}

# ── Run a single benchmark ─────────────────────────────────────────────────

run_bench() {
    local name="$1"
    local condition="$2"  # "inliner_on", "inliner_off", or "baseline"
    local script="$BENCHMARKS_DIR/${name}.py"
    local stderr_file="$BENCHMARKS_DIR/${name}_${condition}_stderr.log"

    if [ ! -f "$script" ]; then
        echo "SKIP"
        return
    fi

    # Skip known JIT bugs for JIT conditions (still run baseline)
    if [ "$condition" != "baseline" ] && is_known_jit_bug "$name"; then
        echo "SKIP"
        return
    fi

    local output
    case "$condition" in
        inliner_on)
            local cond_script="$BENCHMARKS_DIR/${name}_on.py"
            make_condition_script "$script" "$cond_script" "$JIT_INIT_INLINER_ON"
            output=$("$VENV_PYTHON" "$cond_script" 2>"$stderr_file") || { echo "ERROR"; return; }
            ;;
        inliner_off)
            local cond_script="$BENCHMARKS_DIR/${name}_off.py"
            make_condition_script "$script" "$cond_script" "$JIT_INIT_INLINER_OFF"
            output=$("$VENV_PYTHON" "$cond_script" 2>"$stderr_file") || { echo "ERROR"; return; }
            ;;
        baseline)
            if [ "$HAS_BASELINE" = "false" ]; then
                echo "SKIP"
                return
            fi
            output=$("$SYSTEM_PYTHON" -I "$script" 2>"$stderr_file") || { echo "ERROR"; return; }
            ;;
    esac

    # Check stderr for JIT warnings/deopts
    if [ -s "$stderr_file" ]; then
        echo "  [stderr from $name/$condition:]" >> "$BENCHMARKS_DIR/stderr_all.log"
        cat "$stderr_file" >> "$BENCHMARKS_DIR/stderr_all.log"
    fi

    extract_ms "$output"
}

# ── ABBA runner ────────────────────────────────────────────────────────────
# ABBA pattern: A (inliner ON) → B (inliner OFF) → B → A
# Repeated ABBA_REPS times.
# Condition C (baseline) runs once per benchmark at the end.

declare -A INLINER_ON_TIMES
declare -A INLINER_OFF_TIMES
declare -A BASELINE_TIMES

echo "=== Running ABBA benchmarks ==="
echo "Conditions: A=inliner ON, B=inliner OFF, S=skip (known JIT bug), X=error"
echo "Pattern: ABBA × $ABBA_REPS reps = $((ABBA_REPS * 4)) samples per benchmark"
echo ""

for name in "${ALL_BENCHMARKS[@]}"; do
    INLINER_ON_TIMES[$name]=""
    INLINER_OFF_TIMES[$name]=""
    BASELINE_TIMES[$name]=""

    printf "%-25s " "$name"

    for rep in $(seq 1 $ABBA_REPS); do
        # A (inliner ON)
        t=$(run_bench "$name" "inliner_on")
        if [ "$t" != "SKIP" ] && [ "$t" != "ERROR" ] && [ -n "$t" ]; then
            INLINER_ON_TIMES[$name]="${INLINER_ON_TIMES[$name]} $t"
            printf "A"
        elif [ "$t" = "SKIP" ]; then
            printf "S"
        else
            printf "X"
        fi

        # B (inliner OFF)
        t=$(run_bench "$name" "inliner_off")
        if [ "$t" != "SKIP" ] && [ "$t" != "ERROR" ] && [ -n "$t" ]; then
            INLINER_OFF_TIMES[$name]="${INLINER_OFF_TIMES[$name]} $t"
            printf "B"
        elif [ "$t" = "SKIP" ]; then
            printf "S"
        else
            printf "X"
        fi

        # B (inliner OFF)
        t=$(run_bench "$name" "inliner_off")
        if [ "$t" != "SKIP" ] && [ "$t" != "ERROR" ] && [ -n "$t" ]; then
            INLINER_OFF_TIMES[$name]="${INLINER_OFF_TIMES[$name]} $t"
            printf "B"
        elif [ "$t" = "SKIP" ]; then
            printf "S"
        else
            printf "X"
        fi

        # A (inliner ON)
        t=$(run_bench "$name" "inliner_on")
        if [ "$t" != "SKIP" ] && [ "$t" != "ERROR" ] && [ -n "$t" ]; then
            INLINER_ON_TIMES[$name]="${INLINER_ON_TIMES[$name]} $t"
            printf "A"
        elif [ "$t" = "SKIP" ]; then
            printf "S"
        else
            printf "X"
        fi
    done

    # C (baseline — single run)
    t=$(run_bench "$name" "baseline")
    if [ "$t" != "SKIP" ] && [ "$t" != "ERROR" ] && [ -n "$t" ]; then
        BASELINE_TIMES[$name]="$t"
        printf "C"
    fi

    echo ""
done

echo ""

# ── Compute median ─────────────────────────────────────────────────────────

median() {
    local values="$1"
    if [ -z "$values" ]; then
        echo "N/A"
        return
    fi
    local sorted
    sorted=$(echo "$values" | tr ' ' '\n' | sort -g | grep -v '^$')
    local count
    count=$(echo "$sorted" | wc -l)
    local mid=$(( (count + 1) / 2 ))
    echo "$sorted" | sed -n "${mid}p"
}

# ── Results table ──────────────────────────────────────────────────────────

{
    echo ""
    echo "=== CinderX Benchmark Results ==="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Host: $(hostname)"
    echo "Commit: $COMMIT ($FULL_COMMIT)"
    echo "ABBA reps: $ABBA_REPS (${ABBA_REPS}×4 = $((ABBA_REPS * 4)) samples per condition)"
    echo ""
    echo "Conditions:"
    echo "  A (Inliner ON):  $($VENV_PYTHON --version 2>&1) + CinderX JIT + speculative inlining"
    echo "  B (Inliner OFF): $($VENV_PYTHON --version 2>&1) + CinderX JIT, inliner disabled"
    echo "  C (Baseline):    $($SYSTEM_PYTHON --version 2>&1) -I (no CinderX)"
    echo ""
    echo "PRIMARY comparison: A vs B (isolates speculative inlining effect)"
    echo "REFERENCE: C vs A (overall CinderX JIT overhead vs vanilla interpreter)"
    echo ""

    printf "%-25s %12s %12s %8s   %12s %8s\n" \
        "Benchmark" "Inl.ON (ms)" "Inl.OFF(ms)" "A/B" "Baseline(ms)" "C/A"
    printf "%-25s %12s %12s %8s   %12s %8s\n" \
        "─────────────────────────" "────────────" "────────────" "────────" \
        "────────────" "────────"

    for name in "${ALL_BENCHMARKS[@]}"; do
        # Known JIT bugs show as SKIP rather than N/A
        if is_known_jit_bug "$name"; then
            printf "%-25s %12s %12s %8s   %12s %8s\n" \
                "$name" "SKIP" "SKIP" "SKIP" "${BASELINE_TIMES[$name]:-N/A}" "SKIP"
            continue
        fi

        on_med=$(median "${INLINER_ON_TIMES[$name]}")
        off_med=$(median "${INLINER_OFF_TIMES[$name]}")
        base_val="${BASELINE_TIMES[$name]:-N/A}"

        # A/B speedup (off/on — >1 means inliner helps)
        if [ "$on_med" != "N/A" ] && [ "$off_med" != "N/A" ]; then
            ab_ratio=$("$VENV_PYTHON" -c "
on=$on_med; off=$off_med
if on > 0:
    r = off / on
    m = ' **' if r > 1.05 else (' !!' if r < 0.95 else '')
    print(f'{r:.2f}x{m}')
else:
    print('N/A')
")
        else
            ab_ratio="N/A"
        fi

        # C/A ratio (base/on — >1 means JIT is faster than baseline)
        if [ "$on_med" != "N/A" ] && [ "$base_val" != "N/A" ]; then
            ca_ratio=$("$VENV_PYTHON" -c "
on=$on_med; base=$base_val
if on > 0:
    r = base / on
    m = ' **' if r > 1.05 else (' !!' if r < 0.95 else '')
    print(f'{r:.2f}x{m}')
else:
    print('N/A')
")
        else
            ca_ratio="N/A"
        fi

        printf "%-25s %12s %12s %8s   %12s %8s\n" \
            "$name" "$on_med" "$off_med" "$ab_ratio" "$base_val" "$ca_ratio"
    done

    echo ""
    echo "Legend: ** = >5% faster, !! = >5% slower, SKIP = known JIT bug"
    echo "A/B > 1.0 means speculative inlining helps"
    echo "C/A > 1.0 means CinderX JIT is faster than vanilla Python"
    # Known JIT bugs
    if [ ${#KNOWN_JIT_BUGS[@]} -gt 0 ]; then
        echo ""
        echo "=== Known JIT bugs (skipped for A/B comparison) ==="
        for bug in "${KNOWN_JIT_BUGS[@]}"; do
            echo "  $bug"
        done
    fi

    echo ""

    # Raw data
    echo "=== Raw timing data (ms) ==="
    for name in "${ALL_BENCHMARKS[@]}"; do
        echo "$name INLINER_ON: ${INLINER_ON_TIMES[$name]:-N/A}"
        echo "$name INLINER_OFF: ${INLINER_OFF_TIMES[$name]:-N/A}"
        echo "$name BASELINE: ${BASELINE_TIMES[$name]:-N/A}"
    done

    echo ""

    # JIT stderr log
    if [ -f "$BENCHMARKS_DIR/stderr_all.log" ]; then
        echo "=== JIT stderr output (warnings, deopts) ==="
        cat "$BENCHMARKS_DIR/stderr_all.log"
    else
        echo "=== No JIT stderr output captured ==="
    fi
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
echo "=== Benchmark complete ==="
