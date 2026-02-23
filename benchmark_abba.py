#!/usr/bin/env python3
"""ABBA interleaved benchmark — noise-resistant A/B comparison.

On shared hardware (e.g. devgpu004), sequential A-then-B benchmarks are
confounded by co-located workload changes between runs. A noise spike in
the B phase looks like a real regression; a quiet A phase looks like a
real improvement. The 76% format regression (D-1771874900, later retracted
as noise in D-1771875200) demonstrated this failure mode.

ABBA interleaving controls for temporal drift by alternating conditions
within a single run:

    Trial: A  B  B  A  A  B  B  A  A  B  B  A  ...

Each ABBA block has the same wallclock exposure to both conditions.
Drift that is monotonic over the block cancels. Drift that is faster
than a single block shows up as within-block variance, which we measure.

METHODOLOGY:
  1. Warmup BOTH conditions (A = JIT, B = interpreter simulation)
  2. Run N ABBA blocks. Each block:
     - Time A, Time B, Time B, Time A
     - Compute block_delta = mean(A_times) - mean(B_times)
  3. Report: median delta, IQR, whether distributions overlap

USAGE:
  # Single-builtin ABBA comparison (JIT vs baseline):
  python benchmark_abba.py

  # Compare pre/post patch (requires two benchmark functions):
  # Set BENCHMARK_A and BENCHMARK_B via module-level config.

FALSIFICATION:
  - If A and B use the same code path (control experiment), delta should
    be ~0 and distributions should overlap completely.
  - If delta is significant, the IQR must not overlap zero.
  - Raw block deltas are printed for manual inspection.

NOTE: This script measures WITHIN-PROCESS differences. Both conditions
run in the same process, eliminating process-startup noise. For JIT vs
interpreter comparison, we use CinderX's per-function JIT enable/disable.
"""
import os
import sys
import time
import statistics


# ── Configuration ──────────────────────────────────────────────────────────

ABBA_BLOCKS = 15        # Number of ABBA blocks (each = 4 measurements)
BENCH_ITERS = 50_000    # Iterations per single measurement
INNER_ITERS = 100       # Inner loop per iteration
WARMUP_ITERS = 5000     # Warmup calls before any measurement


# ── Benchmark targets ─────────────────────────────────────────────────────

class Obj:
    __slots__ = ('x', 'y', 'z')
    def __init__(self):
        self.x = 1
        self.y = 2
        self.z = 3


class Animal:
    pass


class Dog(Animal):
    pass


def gen():
    while True:
        yield 1


# Each benchmark: (name, function_factory)
# function_factory returns (bench_func, correctness_value)
# We use factories so each condition gets its own function object,
# allowing independent JIT compilation state.

def make_isinstance():
    def bench(n):
        obj = Dog()
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += isinstance(obj, Animal)
        return total
    return bench, INNER_ITERS

def make_issubclass():
    def bench(n):
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += issubclass(Dog, Animal)
        return total
    return bench, INNER_ITERS

def make_hasattr():
    def bench(n):
        obj = Obj()
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += hasattr(obj, 'x')
        return total
    return bench, INNER_ITERS

def make_getattr():
    def bench(n):
        obj = Obj()
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += getattr(obj, 'x')
        return total
    return bench, INNER_ITERS

def make_next():
    def bench(n):
        g = gen()
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += next(g)
        return total
    return bench, INNER_ITERS

def make_next_default():
    def bench(n):
        g = gen()
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                total += next(g, 0)
        return total
    return bench, INNER_ITERS

def make_divmod():
    def bench(n):
        total = 0
        for _ in range(n):
            for _ in range(INNER_ITERS):
                q, r = divmod(1000007, 37)
                total += q
        return total
    return bench, None  # correctness varies


BENCHMARKS = [
    ("isinstance",   make_isinstance),
    ("issubclass",   make_issubclass),
    ("hasattr",      make_hasattr),
    ("getattr",      make_getattr),
    ("next",         make_next),
    ("next_default", make_next_default),
    ("divmod",       make_divmod),
]


# ── ABBA engine ───────────────────────────────────────────────────────────

def time_one(func, n):
    """Time a single benchmark invocation. Returns seconds."""
    t0 = time.perf_counter()
    func(n)
    t1 = time.perf_counter()
    return t1 - t0


def run_abba(func_a, func_b, label, n_blocks, bench_iters):
    """Run ABBA interleaved comparison.

    Returns dict with:
      a_times, b_times: raw measurement lists
      deltas: per-block (mean_a - mean_b) values
      median_delta: median of deltas
      iqr_delta: IQR of deltas
      significant: True if IQR does not span zero
    """
    a_times = []
    b_times = []
    deltas = []

    for block in range(n_blocks):
        # ABBA pattern: A, B, B, A
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
        'label': label,
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


# ── CinderX helpers ───────────────────────────────────────────────────────

def init_cinderjit():
    """Initialise CinderX JIT if available. Returns cinderjit module or None."""
    try:
        import cinderx
        if hasattr(cinderx, 'init'):
            cinderx.init()
        import cinderjit
        return cinderjit
    except (ImportError, AttributeError):
        return None


def warmup_and_compile(func, cinderjit_mod):
    """Warmup a function and force JIT compilation if possible."""
    for _ in range(WARMUP_ITERS):
        func(1)
    if cinderjit_mod:
        try:
            cinderjit_mod.force_compile(func)
        except Exception:
            pass


def is_compiled(func, cinderjit_mod):
    """Check if function is JIT-compiled."""
    if not cinderjit_mod:
        return False
    try:
        return func in cinderjit_mod.get_compiled_functions()
    except Exception:
        return False


# ── Main ──────────────────────────────────────────────────────────────────

def main():
    print("=" * 72)
    print("ABBA Interleaved Benchmark — Noise-Resistant A/B Comparison")
    print("=" * 72)
    print(f"Python:       {sys.version}")
    print(f"ABBA_BLOCKS:  {ABBA_BLOCKS} (= {ABBA_BLOCKS * 4} total measurements per benchmark)")
    print(f"BENCH_ITERS:  {BENCH_ITERS}")
    print(f"INNER_ITERS:  {INNER_ITERS}")
    print(f"WARMUP_ITERS: {WARMUP_ITERS}")
    print()

    cinderjit = init_cinderjit()

    if not cinderjit:
        print("MODE: No CinderX JIT available.")
        print("Running CONTROL experiment: A and B are identical (same code path).")
        print("Expected: delta ~ 0, distributions overlap.")
        print("This validates that the ABBA methodology does not produce")
        print("spurious significant results from measurement noise alone.")
        print()
        mode = "control"
    else:
        print("MODE: CinderX JIT available.")
        print("A = JIT-compiled function, B = interpreter-only duplicate.")
        print("Positive improvement% = A is faster than B = JIT wins.")
        print()
        mode = "jit_vs_interp"

    # ── Run each benchmark ────────────────────────────────────────────────
    all_results = []

    for bench_name, factory in BENCHMARKS:
        # Create two independent function objects
        func_a, expected = factory()
        func_b, _ = factory()

        # Correctness check
        if expected is not None:
            result_a = func_a(1)
            assert result_a == expected, (
                f"{bench_name} correctness: got {result_a}, expected {expected}"
            )

        if mode == "jit_vs_interp":
            # A = JIT compiled, B = interpreter only
            warmup_and_compile(func_a, cinderjit)
            # B: warmup but do NOT force-compile
            for _ in range(WARMUP_ITERS):
                func_b(1)
            # Verify A is compiled
            a_compiled = is_compiled(func_a, cinderjit)
            b_compiled = is_compiled(func_b, cinderjit)
            label = f"{bench_name} (A=JIT:{a_compiled}, B=JIT:{b_compiled})"
        else:
            # Control: both identical, no JIT
            for _ in range(WARMUP_ITERS):
                func_a(1)
                func_b(1)
            label = f"{bench_name} (control: A=B)"

        result = run_abba(func_a, func_b, label, ABBA_BLOCKS, BENCH_ITERS)
        all_results.append(result)

    # ── Summary table ─────────────────────────────────────────────────────
    print(f"{'Benchmark':20s} {'A ns/call':>10s} {'B ns/call':>10s} "
          f"{'Improv%':>8s} {'Signif':>7s} {'IQR':>20s}")
    print("-" * 80)

    for r in all_results:
        sig = "YES" if r['significant'] else "no"
        iqr_str = f"[{r['iqr_lo']*1e3:+.3f}, {r['iqr_hi']*1e3:+.3f}] ms"
        print(f"  {r['label'][:20]:20s} {r['ns_a']:8.1f}   {r['ns_b']:8.1f}   "
              f"{r['pct_improvement']:+6.1f}%  {sig:>7s}  {iqr_str}")

    print()

    # ── Interpretation guide ──────────────────────────────────────────────
    sig_count = sum(1 for r in all_results if r['significant'])
    print(f"Significant results: {sig_count}/{len(all_results)}")
    print()
    print("Interpretation:")
    print("  Signif=YES: IQR of per-block deltas does not span zero.")
    print("    The difference is unlikely to be measurement noise.")
    print("  Signif=no:  IQR spans zero. Cannot distinguish from noise.")
    print("  Improv%:    Positive = A faster than B. Negative = B faster.")
    print()

    # ── Raw block deltas ──────────────────────────────────────────────────
    print("Raw per-block deltas (ms) — inspect for drift patterns:")
    for r in all_results:
        deltas_ms = [f"{d*1e3:+.3f}" for d in r['deltas']]
        print(f"  {r['label'][:20]:20s} [{', '.join(deltas_ms)}]")

    print()
    print("=" * 72)


if __name__ == "__main__":
    main()
