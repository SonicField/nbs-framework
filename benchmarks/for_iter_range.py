"""
for_iter_range — Targeted benchmark for FOR_ITER range iteration overhead.

Targets: for i in range(n) loop dispatch — specifically the tp_iternext
         call on range_iterator objects.

Motivation: range() is the most common Python loop pattern. Every numeric
loop uses it. The CinderX JIT currently emits generic InvokeIterNext for
ALL iterator types (builder.cpp:emitForIter), dispatching through
tp_iternext on every iteration. CPython 3.12 specialises FOR_ITER_RANGE
to avoid this dispatch entirely, directly incrementing the iterator's
internal counter and checking the stop value.

This benchmark is designed to make the tp_iternext dispatch overhead the
DOMINANT cost — loop bodies perform minimal arithmetic so that iteration
overhead is a large fraction of total runtime.

Expected JIT behaviour:
  - Without FOR_ITER_RANGE specialisation: generic tp_iternext dispatch
    per iteration (~15-25 cycles overhead per iteration)
  - With specialisation: direct counter increment + bounds check
    (~3-5 cycles per iteration)

Design: ABBA methodology (A=specialisation ON, B=OFF, 4 samples minimum).
Warmup ensures Tier 2 JIT compilation before timing.
"""

import time


def bench_tight_range(iterations):
    """Tight loop — range iteration is the dominant cost.

    Loop body is a single integer addition. The tp_iternext dispatch
    overhead should be clearly visible relative to the body cost.
    """
    total = 0
    for _ in range(iterations):
        for i in range(1000):
            total += i
    return total


def bench_short_ranges(iterations):
    """Many short range loops — maximises per-loop setup/teardown cost.

    Short ranges (10 elements) mean the iterator creation and first
    tp_iternext call are a larger fraction of total loop cost.
    """
    total = 0
    for _ in range(iterations):
        for _ in range(100):
            s = 0
            for i in range(10):
                s += i
            total += s
    return total


def bench_nested_range(iterations):
    """Nested range loops — compounds iteration overhead.

    Two levels of range iteration, each paying the tp_iternext cost.
    Inner loop body is minimal (multiply + accumulate).
    """
    total = 0
    for _ in range(iterations):
        for i in range(50):
            for j in range(50):
                total += i * j
        total = total % 1_000_000_000
    return total


def bench_range_with_step(iterations):
    """Range with step argument — tests non-trivial range_iterator state.

    range(start, stop, step) uses the same tp_iternext but with
    different internal arithmetic. Specialisation must handle all
    three range() signatures.
    """
    total = 0
    for _ in range(iterations):
        for i in range(0, 500, 2):
            total += i
        for i in range(999, 0, -3):
            total += i
        total = total % 1_000_000_000
    return total


def bench_range_accumulate(iterations):
    """Range loop with float accumulation — mimics numeric computation.

    Slightly heavier body (float multiply + add) but still light enough
    that iteration overhead is measurable. This pattern mimics simple
    numeric kernels (dot products, reductions).
    """
    total = 0.0
    for _ in range(iterations):
        acc = 0.0
        for i in range(500):
            acc += float(i) * 0.001
        total += acc
        total = total % 1_000_000.0
    return total


def benchmark_for_iter_range(iterations=200):
    """Exercise all range iteration patterns. Returns dict of sub-results."""
    results = {}

    results['tight'] = bench_tight_range(iterations)
    results['short'] = bench_short_ranges(iterations)
    results['nested'] = bench_nested_range(iterations)
    results['stepped'] = bench_range_with_step(iterations)
    results['accumulate'] = bench_range_accumulate(iterations)

    return results


def main():
    # Warmup — ensure JIT compilation (Tier 2) before timing
    benchmark_for_iter_range(iterations=50)

    # Timed run
    start = time.perf_counter_ns()
    results = benchmark_for_iter_range(iterations=200)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    # Checksum for correctness verification
    checksum = sum(int(v) % 999983 for v in results.values())
    print(f"for_iter_range: {elapsed_ms:.3f}ms (checksum={checksum})")

    # Sub-benchmark breakdown (for analysis, not ABBA)
    for name, val in results.items():
        print(f"  {name}: result={val}")


if __name__ == "__main__":
    main()
