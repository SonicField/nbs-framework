#!/usr/bin/env python3
"""G1 fast path ABBA benchmark — JITRT_InvokeIterNext verification.

The G1 fast path fires when BOTH the caller AND the generator are
JIT-compiled. JITRT_InvokeIterNext uses the generator's resumeEntry
for a direct JIT-to-JIT transition, bypassing tp_iternext.

The existing benchmark_abba.py measured a DIFFERENT variable:
  - It varied the CALLER compilation state (JIT vs interpreter)
  - The GENERATOR was the same in both conditions
  - Result: -0.3%, not significant (JIT-compiling the caller alone
    does not help next())

This benchmark measures the CORRECT variable for the G1 claim:
  A = JIT caller + JIT generator    (G1 fast path fires)
  B = JIT caller + interp generator (G1 guard fails -> tp_iternext)

The caller is JIT-compiled in BOTH conditions. Only the generator
compilation state varies. This isolates the G1 fast path contribution.

DESIGN NOTE — single-factory callers:
  Both callers are created by the same make_caller(gen_func) factory.
  They share the same code object and thus the same JIT code. The only
  difference is the closure variable (gen_func), which determines
  whether the created generator has resumeEntry (JIT) or not (interp).
  This eliminates code-object-level bias that would produce false
  positives in control mode.

METHODOLOGY:
  1. Create two generator functions with identical behaviour
  2. Force-compile gen_jit; leave gen_interp uncompiled
  3. Create two callers from the SAME factory, force-compile both
  4. Run ABBA blocks: A uses gen_jit, B uses gen_interp
  5. Report median delta, IQR, significance

FALSIFICATION:
  - The sequential estimate was 16.5% (118.0 ns vs 141.3 ns)
  - If G1 provides no real benefit, delta ~ 0 and IQR spans zero
  - If real, IQR must not span zero
  - Control mode (no CinderX): both conditions identical, delta ~ 0

USAGE:
  # On build-host with CinderX system Python:
  /usr/local/internal-toolchain/platform010-aarch64/bin/python3 benchmark_g1_next_abba.py

  # Control (no CinderX, validates methodology):
  python3 benchmark_g1_next_abba.py
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


# -- Generator functions (two independent code objects, identical behaviour) --
# These MUST be separate def statements (not copies of one function) so they
# have independent code objects. force_compile(gen_jit) must not cause
# gen_interp to be compiled.

def gen_jit():
    """Generator function -- will be JIT-compiled via force_compile."""
    while True:
        yield 1


def gen_interp():
    """Generator function -- stays interpreter-only (never compiled)."""
    while True:
        yield 1


# -- Benchmark caller factory ------------------------------------------------
# CRITICAL: both callers come from the SAME factory. They share the same
# inner code object, so they get the same JIT code. The only difference
# is the closure variable gen_func, which determines the generator's
# compilation state at runtime. This eliminates code-object-level bias.

def make_caller(gen_func):
    """Create a benchmark caller that calls next() on gen_func's generator."""
    def bench(n):
        g = gen_func()
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += next(g)
        return total
    return bench


# -- ABBA engine (self-contained, no dependency on benchmark_abba.py) --------

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


# -- CinderX helpers ---------------------------------------------------------

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
    print("G1 Fast Path ABBA Benchmark -- JITRT_InvokeIterNext")
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
        print("Both conditions use interpreter generators.")
        print("Expected: delta ~ 0, IQR spans zero.")
        print("This validates the methodology produces no false positives.")
        print()
    else:
        print("MODE: G1 FAST PATH TEST")
        print("  A = JIT caller + JIT generator   (G1 fast path)")
        print("  B = JIT caller + interp generator (tp_iternext fallback)")
        print("  Positive improvement% = A faster = G1 wins.")
        print()

    # -- Create callers from SAME factory -------------------------------------
    caller_a = make_caller(gen_jit)
    caller_b = make_caller(gen_interp)

    # Verify they share the same code object (same JIT code)
    same_code = caller_a.__code__ is caller_b.__code__
    print(f"Caller code objects identical: {same_code}")
    if not same_code:
        print("WARNING: callers have different code objects.")
        print("This should not happen with the single-factory design.")
        print("Results may include code-object-level bias.")
    print()

    # -- Correctness check ----------------------------------------------------
    assert caller_a(1) == INNER_ITERS, "caller_a correctness failed"
    assert caller_b(1) == INNER_ITERS, "caller_b correctness failed"
    print("Correctness: PASS (both callers return correct values)")
    print()

    # -- Compilation setup ----------------------------------------------------
    if cinderjit:
        print("Compilation setup:")

        # Warmup all functions
        for _ in range(WARMUP_ITERS):
            caller_a(1)
            caller_b(1)

        # Force-compile: both callers + gen_jit (NOT gen_interp)
        # Since callers share a code object, compiling one compiles both.
        a_ok = force_compile(caller_a, cinderjit)
        b_ok = force_compile(caller_b, cinderjit)
        g_jit_ok = force_compile(gen_jit, cinderjit)

        # Verify gen_interp is NOT compiled
        g_interp_compiled = is_compiled(gen_interp, cinderjit)

        print(f"  caller_a (JIT gen):     {'JIT' if a_ok else 'INTERP'}")
        print(f"  caller_b (interp gen):  {'JIT' if b_ok else 'INTERP'}")
        print(f"  gen_jit:                {'JIT' if g_jit_ok else 'INTERP'}")
        print(f"  gen_interp:             {'JIT' if g_interp_compiled else 'INTERP'}")
        print()

        # Validate preconditions
        if not a_ok or not b_ok:
            print("ERROR: Callers not JIT-compiled. Cannot test G1 path.")
            print("Both callers must be compiled so next() goes through")
            print("JITRT_BuiltinNext in both conditions.")
            sys.exit(1)

        if not g_jit_ok:
            print("ERROR: gen_jit not compiled. G1 fast path cannot fire.")
            print("The A condition requires a JIT generator with resumeEntry.")
            sys.exit(1)

        if g_interp_compiled:
            print("WARNING: gen_interp auto-compiled despite high threshold!")
            print("The B condition should use an interpreter generator.")
            print("Results may not reflect G1 vs tp_iternext difference.")
            print("Try increasing COMPILE_THRESHOLD or running with fewer")
            print("warmup iterations.")
            print()

        # Precondition summary
        if a_ok and b_ok and g_jit_ok and not g_interp_compiled:
            print("Preconditions: ALL MET")
            print("  - Both callers JIT-compiled (JITRT_BuiltinNext active)")
            print("  - gen_jit compiled (resumeEntry set, G1 fires)")
            print("  - gen_interp NOT compiled (no resumeEntry, tp_iternext)")
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

    print(f"  A (JIT gen):     {result['ns_a']:.1f} ns/call")
    print(f"  B (interp gen):  {result['ns_b']:.1f} ns/call")
    print(f"  Improvement:     {result['pct_improvement']:+.1f}%")
    print(f"  Significant:     {'YES' if result['significant'] else 'NO'}")
    print(f"  IQR:             [{result['iqr_lo']*1e3:+.3f}, "
          f"{result['iqr_hi']*1e3:+.3f}] ms")
    print()

    if result['significant'] and result['pct_improvement'] > 0:
        print("VERDICT: G1 fast path provides a REAL speedup.")
        print(f"  next() is {result['pct_improvement']:.1f}% faster when the "
              "generator is JIT-compiled.")
        print("  The JITRT_InvokeIterNext direct resumeEntry path is faster")
        print("  than falling through to tp_iternext.")
    elif result['significant'] and result['pct_improvement'] < 0:
        print("VERDICT: G1 fast path is SLOWER (unexpected).")
        print("  Investigate: the direct resumeEntry path should not be")
        print("  slower than tp_iternext dispatch.")
    else:
        print("VERDICT: G1 fast path shows NO significant difference.")
        print("  The IQR spans zero -- cannot distinguish from noise.")
        if not cinderjit:
            print("  (Expected: control mode, no JIT available.)")
        else:
            print("  The sequential 16.5% claim is NOT confirmed by ABBA.")

    print()

    # -- Raw block deltas -----------------------------------------------------
    print("Raw per-block deltas (ms):")
    deltas_ms = [f"{d*1e3:+.3f}" for d in result['deltas']]
    print(f"  [{', '.join(deltas_ms)}]")
    print()

    # -- Comparison with sequential estimate ----------------------------------
    print("Context:")
    print("  Sequential estimate:  16.5% (118.0 ns vs 141.3 ns)")
    print(f"  ABBA result:          {result['pct_improvement']:+.1f}% "
          f"({result['ns_a']:.1f} ns vs {result['ns_b']:.1f} ns)")
    if result['significant']:
        if abs(result['pct_improvement'] - 16.5) < 5:
            print("  Assessment: ABBA confirms sequential estimate (within 5pp)")
        else:
            print("  Assessment: ABBA shows different magnitude than sequential")
    else:
        print("  Assessment: ABBA does not confirm sequential estimate")
    print()

    print("=" * 72)


if __name__ == "__main__":
    main()
