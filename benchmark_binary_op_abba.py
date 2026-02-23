#!/usr/bin/env python3
"""BINARY_OP_ADD_INT ABBA benchmark — integer arithmetic specialisation.

Measures the speedup from CinderX JIT's BINARY_OP_ADD_INT specialisation
(GuardType(TLongExact) + Simplify pass) compared to CPython's interpreter
handler for the same specialised opcode.

INDEPENDENT VARIABLE:
  The arithmetic function's compilation state.
  A = JIT-compiled arithmetic (GuardType + Simplify → fast int path)
  B = Interpreter arithmetic (CPython's BINARY_OP_ADD_INT handler)

CONTROLLED VARIABLES:
  - Both CALLERS are JIT-compiled (same code object via single factory)
  - Both arithmetic functions have identical logic (independent code objects)
  - Same input data in both conditions

DESIGN NOTE — single-factory callers:
  Both callers are created by make_caller(arith_func). They share the same
  code object and thus the same JIT code. The only difference is the closure
  variable (arith_func), which determines whether arithmetic runs through
  JIT or interpreter. This eliminates code-object-level bias.

DESIGN NOTE — arithmetic functions:
  arith_jit() and arith_interp() are separate def statements with identical
  logic but independent code objects. force_compile(arith_jit) cannot
  accidentally compile arith_interp.

  The workload is a tight integer accumulation loop:
    total = 0
    for i in range(N):
        total += i
  This exercises BINARY_OP_ADD_INT on every iteration. The loop also
  uses FOR_ITER_RANGE (already specialised) and STORE_FAST, so the
  measurement includes their contribution. The addition is the dominant
  cost since range iteration and store are very cheap.

FALSIFICATION:
  - If BINARY_OP_ADD_INT specialisation provides no real benefit,
    delta ~ 0 and IQR spans zero
  - If real, IQR must not span zero
  - Control mode (no CinderX): both conditions identical, delta ~ 0

USAGE:
  # On devgpu004 with CinderX system Python:
  /usr/local/fbcode/platform010-aarch64/bin/python3 benchmark_binary_op_abba.py

  # Control (no CinderX, validates methodology):
  python3 benchmark_binary_op_abba.py
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


# -- Arithmetic functions (two independent code objects, identical logic) -----
# These MUST be separate def statements so they have independent code objects.
# force_compile(arith_jit) must not cause arith_interp to be compiled.

def arith_jit(n):
    """Integer accumulation — will be JIT-compiled via force_compile.

    Exercises BINARY_OP_ADD_INT on every iteration of the inner loop.
    """
    total = 0
    for i in range(n):
        total += i
    return total


def arith_interp(n):
    """Integer accumulation — stays interpreter-only (never compiled).

    Identical logic to arith_jit. Uses CPython's BINARY_OP_ADD_INT handler.
    """
    total = 0
    for i in range(n):
        total += i
    return total


# -- Benchmark caller factory ------------------------------------------------
# CRITICAL: both callers come from the SAME factory. They share the same
# inner code object, so they get the same JIT code. The only difference
# is the closure variable arith_func, which determines whether arithmetic
# runs through JIT (GuardType + Simplify) or interpreter.

def make_caller(arith_func):
    """Create a benchmark caller that invokes arith_func(INNER_ITERS)."""
    def bench(n):
        total = 0
        for _ in range(n):
            total += arith_func(INNER_ITERS)
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
    print("BINARY_OP_ADD_INT ABBA Benchmark")
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
        print("Both conditions use interpreter arithmetic.")
        print("Expected: delta ~ 0, IQR spans zero.")
        print("This validates the methodology produces no false positives.")
        print()
    else:
        print("MODE: BINARY_OP_ADD_INT SPECIALISATION TEST")
        print("  A = JIT arithmetic  (GuardType(TLongExact) + Simplify)")
        print("  B = Interp arithmetic (CPython BINARY_OP_ADD_INT handler)")
        print("  Positive improvement% = A faster = JIT specialisation wins.")
        print()

    # -- Create callers from SAME factory -------------------------------------
    caller_a = make_caller(arith_jit)
    caller_b = make_caller(arith_interp)

    # Verify they share the same code object (same JIT code)
    same_code = caller_a.__code__ is caller_b.__code__
    print(f"Caller code objects identical: {same_code}")
    if not same_code:
        print("WARNING: callers have different code objects.")
        print("This should not happen with the single-factory design.")
        print("Results may include code-object-level bias.")
    print()

    # -- Correctness check ----------------------------------------------------
    expected = sum(range(INNER_ITERS))  # sum(0..99) = 4950
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

        # Force-compile: both callers + arith_jit (NOT arith_interp)
        # Since callers share a code object, compiling one compiles both.
        a_ok = force_compile(caller_a, cinderjit)
        b_ok = force_compile(caller_b, cinderjit)
        arith_jit_ok = force_compile(arith_jit, cinderjit)

        # Verify arith_interp is NOT compiled
        arith_interp_compiled = is_compiled(arith_interp, cinderjit)

        print(f"  caller_a (JIT arith):     {'JIT' if a_ok else 'INTERP'}")
        print(f"  caller_b (interp arith):  {'JIT' if b_ok else 'INTERP'}")
        print(f"  arith_jit:                {'JIT' if arith_jit_ok else 'INTERP'}")
        print(f"  arith_interp:             {'JIT' if arith_interp_compiled else 'INTERP'}")
        print()

        # Validate preconditions
        if not a_ok or not b_ok:
            print("ERROR: Callers not JIT-compiled. Cannot test specialisation.")
            print("Both callers must be compiled so call overhead is identical.")
            sys.exit(1)

        if not arith_jit_ok:
            print("ERROR: arith_jit not compiled. Specialisation cannot fire.")
            sys.exit(1)

        if arith_interp_compiled:
            print("WARNING: arith_interp auto-compiled despite high threshold!")
            print("The B condition should use interpreter arithmetic.")
            print("Results may not reflect JIT vs interpreter difference.")
            print("Try increasing COMPILE_THRESHOLD.")
            print()

        # Precondition summary
        if a_ok and b_ok and arith_jit_ok and not arith_interp_compiled:
            print("Preconditions: ALL MET")
            print("  - Both callers JIT-compiled (identical call overhead)")
            print("  - arith_jit compiled (GuardType + Simplify active)")
            print("  - arith_interp NOT compiled (CPython interpreter path)")
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

    print(f"  A (JIT arith):     {result['ns_a']:.1f} ns/iter")
    print(f"  B (interp arith):  {result['ns_b']:.1f} ns/iter")
    print(f"  Improvement:       {result['pct_improvement']:+.1f}%")
    print(f"  Significant:       {'YES' if result['significant'] else 'NO'}")
    print(f"  IQR:               [{result['iqr_lo']*1e3:+.3f}, "
          f"{result['iqr_hi']*1e3:+.3f}] ms")
    print()

    if result['significant'] and result['pct_improvement'] > 0:
        print("VERDICT: BINARY_OP_ADD_INT specialisation provides a REAL speedup.")
        print(f"  Integer arithmetic is {result['pct_improvement']:.1f}% faster when")
        print("  JIT-compiled with GuardType(TLongExact) specialisation vs")
        print("  CPython's interpreter handler.")
    elif result['significant'] and result['pct_improvement'] < 0:
        print("VERDICT: JIT arithmetic is SLOWER (unexpected).")
        print("  Investigate: JIT + GuardType should not be slower than")
        print("  interpreter dispatch.")
    else:
        print("VERDICT: BINARY_OP_ADD_INT shows NO significant difference.")
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
