#!/usr/bin/env python3
"""TO_BOOL_INT ABBA benchmark — integer truthiness specialisation.

Measures the speedup from CinderX JIT's TO_BOOL_INT specialisation
(GuardType(TLongExact) + simplifyIsTruthy → PrimitiveCompare(x != 0))
compared to CPython's interpreter handler for generic TO_BOOL
(PyObject_IsTrue call).

INDEPENDENT VARIABLE:
  The truthiness function's compilation state.
  A = JIT-compiled truthiness (GuardType + Simplify → direct int != 0)
  B = Interpreter truthiness (CPython's generic PyObject_IsTrue path)

CONTROLLED VARIABLES:
  - Both CALLERS are JIT-compiled (same code object via single factory)
  - Both truthiness functions have identical logic (independent code objects)
  - Same input data in both conditions

DESIGN NOTE — single-factory callers:
  Both callers are created by make_caller(truth_func). They share the same
  code object and thus the same JIT code. The only difference is the closure
  variable (truth_func), which determines whether truthiness checking runs
  through JIT or interpreter. This eliminates code-object-level bias.

DESIGN NOTE — truthiness functions:
  truth_jit() and truth_interp() are separate def statements with identical
  logic but independent code objects. force_compile(truth_jit) cannot
  accidentally compile truth_interp.

  The workload is a tight truthiness-checking loop:
    count = 0
    for i in range(N):
        if i:
            count += 1
    return count
  This exercises TO_BOOL on every iteration via the 'if i:' branch.
  The loop also uses FOR_ITER_RANGE (already specialised), STORE_FAST,
  and a conditional BINARY_OP_ADD_INT, so the measurement includes their
  contribution. The truthiness check is the target operation; the
  conditional increment is cheap and constant across both conditions.

FALSIFICATION:
  - If TO_BOOL_INT specialisation provides no real benefit,
    delta ~ 0 and IQR spans zero
  - If real, IQR must not span zero
  - Control mode (no CinderX): both conditions identical, delta ~ 0

USAGE:
  # On devgpu004 with CinderX system Python:
  /usr/local/fbcode/platform010-aarch64/bin/python3 benchmark_to_bool_abba.py

  # Control (no CinderX, validates methodology):
  python3 benchmark_to_bool_abba.py
"""
import sys
import time
import statistics


# -- Configuration -----------------------------------------------------------

ABBA_BLOCKS = 15        # Number of ABBA blocks (each = 4 measurements)
BENCH_ITERS = 50_000    # Outer loop iterations per measurement
INNER_ITERS = 100       # Inner loop iterations per outer iteration
WARMUP_ITERS = 5000     # Warmup calls before measurement
COMPILE_THRESHOLD = 999_999_999  # Prevent auto-compilation


# -- Truthiness functions (two independent code objects, identical logic) -----
# These MUST be separate def statements so they have independent code objects.
# force_compile(truth_jit) must not cause truth_interp to be compiled.

def truth_jit(n):
    """Integer truthiness — will be JIT-compiled via force_compile.

    Exercises TO_BOOL on every iteration via 'if i:'.
    When JIT-compiled with GuardType(TLongExact), the Simplify pass
    replaces IsTruthy with PrimitiveCompare(i != PyLong_Zero).
    """
    count = 0
    for i in range(n):
        if i:
            count += 1
    return count


def truth_interp(n):
    """Integer truthiness — stays interpreter-only (never compiled).

    Identical logic to truth_jit. Uses CPython's generic PyObject_IsTrue.
    """
    count = 0
    for i in range(n):
        if i:
            count += 1
    return count


# -- Benchmark caller factory ------------------------------------------------
# CRITICAL: both callers come from the SAME factory. They share the same
# inner code object, so they get the same JIT code. The only difference
# is the closure variable truth_func, which determines whether truthiness
# runs through JIT (GuardType + Simplify) or interpreter.

def make_caller(truth_func):
    """Create a benchmark caller that invokes truth_func(INNER_ITERS)."""
    def bench(n):
        total = 0
        for _ in range(n):
            total += truth_func(INNER_ITERS)
        return total
    return bench


# -- ABBA engine (self-contained) -------------------------------------------

def time_one(func, n):
    """Time a single benchmark invocation. Returns seconds."""
    t0 = time.perf_counter()
    func(n)
    t1 = time.perf_counter()
    return t1 - t0


def run_abba(func_a, func_b, n_blocks, bench_iters):
    """Run ABBA interleaved comparison.

    Each block: A, B, B, A. Monotonic drift within a block cancels.

    Returns dict with raw times, deltas, median, IQR, significance.
    """
    a_times = []
    b_times = []
    deltas = []

    for block in range(n_blocks):
        ta1 = time_one(func_a, bench_iters)
        tb1 = time_one(func_b, bench_iters)
        tb2 = time_one(func_b, bench_iters)
        ta2 = time_one(func_a, bench_iters)

        a_times.extend([ta1, ta2])
        b_times.extend([tb1, tb2])

        block_a_mean = (ta1 + ta2) / 2
        block_b_mean = (tb1 + tb2) / 2
        deltas.append(block_a_mean - block_b_mean)

    a_times.sort()
    b_times.sort()
    deltas.sort()

    median_a = statistics.median(a_times)
    median_b = statistics.median(b_times)
    median_delta = statistics.median(deltas)

    q1_idx = len(deltas) // 4
    q3_idx = 3 * len(deltas) // 4
    iqr_lo = deltas[q1_idx]
    iqr_hi = deltas[q3_idx]

    # Significant if IQR does not span zero
    significant = (iqr_lo > 0 and iqr_hi > 0) or (iqr_lo < 0 and iqr_hi < 0)

    total_calls = bench_iters * INNER_ITERS
    ns_a = median_a / total_calls * 1e9
    ns_b = median_b / total_calls * 1e9
    pct = (median_b - median_a) / median_b * 100 if median_b > 0 else 0

    return {
        'a_times': a_times,
        'b_times': b_times,
        'deltas': deltas,
        'median_a': median_a,
        'median_b': median_b,
        'ns_a': ns_a,
        'ns_b': ns_b,
        'median_delta': median_delta,
        'iqr_lo': iqr_lo,
        'iqr_hi': iqr_hi,
        'significant': significant,
        'pct_improvement': pct,
    }


# -- CinderX helpers --------------------------------------------------------

def init_cinderjit():
    """Initialise CinderX JIT. Returns cinderjit module or None."""
    try:
        import cinderx
        cinderx.init()
        import cinderjit
        # Set very high threshold to prevent auto-compilation.
        # We will selectively force_compile only what we want.
        try:
            cinderjit.compile_after_n_calls(COMPILE_THRESHOLD)
        except (AttributeError, TypeError):
            pass
        return cinderjit
    except (ImportError, AttributeError):
        return None


def force_compile(func, cinderjit_mod):
    """Force JIT-compile a function. Returns True if compiled."""
    if not cinderjit_mod:
        return False
    try:
        cinderjit_mod.force_compile(func)
        return func in cinderjit_mod.get_compiled_functions()
    except Exception:
        return False


def is_compiled(func, cinderjit_mod):
    """Check if function is JIT-compiled."""
    if not cinderjit_mod:
        return False
    try:
        return func in cinderjit_mod.get_compiled_functions()
    except Exception:
        return False


# -- Main --------------------------------------------------------------------

def main():
    print("=" * 72)
    print("TO_BOOL_INT ABBA Benchmark")
    print("=" * 72)
    print(f"Python:       {sys.version}")
    print(f"ABBA_BLOCKS:  {ABBA_BLOCKS} (= {ABBA_BLOCKS * 4} measurements)")
    print(f"BENCH_ITERS:  {BENCH_ITERS}")
    print(f"INNER_ITERS:  {INNER_ITERS}")
    print(f"WARMUP_ITERS: {WARMUP_ITERS}")
    print()

    cinderjit = init_cinderjit()

    if not cinderjit:
        print("MODE: CONTROL (no CinderX JIT)")
        print("Both conditions use interpreter truthiness.")
        print("Expected: delta ~ 0, IQR spans zero.")
        print("This validates the methodology produces no false positives.")
        print()
    else:
        print("MODE: TO_BOOL_INT SPECIALISATION TEST")
        print("  A = JIT truthiness  (GuardType(TLongExact) + Simplify)")
        print("  B = Interp truthiness (CPython generic PyObject_IsTrue)")
        print("  Positive improvement% = A faster = JIT specialisation wins.")
        print()

    # -- Create callers from SAME factory -------------------------------------
    caller_a = make_caller(truth_jit)
    caller_b = make_caller(truth_interp)

    # Verify they share the same code object (same JIT code)
    same_code = caller_a.__code__ is caller_b.__code__
    print(f"Caller code objects identical: {same_code}")
    if not same_code:
        print("WARNING: callers have different code objects.")
        print("This should not happen with the single-factory design.")
        print("Results may include code-object-level bias.")
    print()

    # -- Correctness check ----------------------------------------------------
    # truth_func(100) counts non-zero values in range(100) = 99 (0 is falsy)
    expected = 99  # range(100): 0 is falsy, 1..99 are truthy
    assert caller_a(1) == expected, f"caller_a: got {caller_a(1)}, expected {expected}"
    assert caller_b(1) == expected, f"caller_b: got {caller_b(1)}, expected {expected}"
    print(f"Correctness: PASS (both callers return {expected})")
    print()

    # -- Compilation setup ----------------------------------------------------
    if cinderjit:
        print("Compilation setup:")

        # Warmup all functions
        for _ in range(WARMUP_ITERS):
            caller_a(1)
            caller_b(1)

        # Force-compile: both callers + truth_jit (NOT truth_interp)
        # Since callers share a code object, compiling one compiles both.
        a_ok = force_compile(caller_a, cinderjit)
        b_ok = force_compile(caller_b, cinderjit)
        truth_jit_ok = force_compile(truth_jit, cinderjit)

        # Verify truth_interp is NOT compiled
        truth_interp_compiled = is_compiled(truth_interp, cinderjit)

        print(f"  caller_a (JIT truth):     {'JIT' if a_ok else 'INTERP'}")
        print(f"  caller_b (interp truth):  {'JIT' if b_ok else 'INTERP'}")
        print(f"  truth_jit:                {'JIT' if truth_jit_ok else 'INTERP'}")
        print(f"  truth_interp:             {'JIT' if truth_interp_compiled else 'INTERP'}")
        print()

        # Validate preconditions
        if not a_ok or not b_ok:
            print("ERROR: Callers not JIT-compiled. Cannot test specialisation.")
            print("Both callers must be compiled so call overhead is identical.")
            sys.exit(1)

        if not truth_jit_ok:
            print("ERROR: truth_jit not compiled. Specialisation cannot fire.")
            sys.exit(1)

        if truth_interp_compiled:
            print("WARNING: truth_interp auto-compiled despite high threshold!")
            print("The B condition should use interpreter truthiness.")
            print("Results may not reflect JIT vs interpreter difference.")
            print("Try increasing COMPILE_THRESHOLD.")
            print()

        # Precondition summary
        if a_ok and b_ok and truth_jit_ok and not truth_interp_compiled:
            print("Preconditions: ALL MET")
            print("  - Both callers JIT-compiled (identical call overhead)")
            print("  - truth_jit compiled (GuardType + Simplify active)")
            print("  - truth_interp NOT compiled (CPython interpreter path)")
            print()
        else:
            print("Preconditions: PARTIAL -- results may be unreliable")
            print()
    else:
        # Control mode: just warmup
        for _ in range(WARMUP_ITERS):
            caller_a(1)
            caller_b(1)
        print("Warmup complete (interpreter mode)")
        print()

    # -- Run ABBA -------------------------------------------------------------
    print(f"Running {ABBA_BLOCKS} ABBA blocks...")
    print()

    result = run_abba(caller_a, caller_b, ABBA_BLOCKS, BENCH_ITERS)

    # -- Results --------------------------------------------------------------
    print("=" * 72)
    print("RESULTS")
    print("=" * 72)
    print()

    print(f"  A (JIT truth):     {result['ns_a']:.1f} ns/iter")
    print(f"  B (interp truth):  {result['ns_b']:.1f} ns/iter")
    print(f"  Improvement:       {result['pct_improvement']:+.1f}%")
    print(f"  Significant:       {'YES' if result['significant'] else 'NO'}")
    print(f"  IQR:               [{result['iqr_lo']*1e3:+.3f}, "
          f"{result['iqr_hi']*1e3:+.3f}] ms")
    print()

    if result['significant'] and result['pct_improvement'] > 0:
        print("VERDICT: TO_BOOL_INT specialisation provides a REAL speedup.")
        print(f"  Integer truthiness is {result['pct_improvement']:.1f}% faster when")
        print("  JIT-compiled with GuardType(TLongExact) + Simplify (i != 0)")
        print("  vs CPython's generic PyObject_IsTrue interpreter path.")
    elif result['significant'] and result['pct_improvement'] < 0:
        print("VERDICT: JIT truthiness is SLOWER (unexpected).")
        print("  Investigate: GuardType + Simplify should not be slower than")
        print("  generic PyObject_IsTrue dispatch.")
    else:
        print("VERDICT: TO_BOOL_INT shows NO significant difference.")
        print("  The IQR spans zero -- cannot distinguish from noise.")
        if not cinderjit:
            print("  (Expected: control mode, no JIT available.)")
        else:
            print("  The JIT specialisation may not help for this workload,")
            print("  or the effect is too small for ABBA to detect.")

    print()

    # -- Raw block deltas -----------------------------------------------------
    print("Raw per-block deltas (ms):")
    deltas_ms = [f"{d*1e3:+.3f}" for d in result['deltas']]
    print(f"  [{', '.join(deltas_ms)}]")
    print()

    print("=" * 72)


if __name__ == "__main__":
    main()
