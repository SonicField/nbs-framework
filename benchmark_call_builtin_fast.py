#!/usr/bin/env python3
"""CALL_BUILTIN_FAST benchmark — isolate METH_FASTCALL builtin call overhead.

Measures the cost of calling pure METH_FASTCALL builtins in tight loops.
These are the targets for the CALL_BUILTIN_FAST JIT specialisation:

  isinstance, issubclass, hasattr, getattr, setattr, delattr,
  next, iter, divmod, format

The benchmark has two modes:
  A) JIT-compiled: each benchmark function runs enough times to trigger JIT
  B) Interpreter: same functions, no JIT (set CINDERX_DISABLE_JIT=1)

Each benchmark function calls a single METH_FASTCALL builtin in a tight
loop. The function structure forces the JIT to compile the CALL opcode
path, not just inline the builtin via simplifyVectorCall.

FALSIFICATION DESIGN:
  - If CinderX is present, verify each benchmark IS JIT-compiled via
    cinderjit.is_jit_compiled(). If not compiled, results are misleading.
  - Each benchmark includes a correctness check — if the result is wrong,
    the function is not measuring what we think.
  - Warmup runs 5000 calls to ensure both Tier 1 and Tier 2 fire.
  - Each benchmark is timed over 100k iterations to amortise call overhead.

Usage:
  # On devgpu004 with CinderX:
  /data/users/alexturner/cinderx_dev/venv/bin/python benchmark_call_builtin_fast.py

  # Baseline (no JIT, interpreter only):
  CINDERX_DISABLE_JIT=1 /data/users/alexturner/cinderx_dev/venv/bin/python benchmark_call_builtin_fast.py
"""
import os
import sys
import time


# ── Configuration ────────────────────────────────────────────────────────────

WARMUP_ITERS = 5000     # Enough for Tier 1 (100) + Tier 2 (1000) + margin
BENCH_ITERS = 100_000   # Per-benchmark measurement iterations
INNER_ITERS = 100       # Inner loop iterations per benchmark call
REPEATS = 5             # Number of timing samples per benchmark


# ── Helpers ──────────────────────────────────────────────────────────────────

class Obj:
    """Target object for hasattr/getattr/setattr/delattr benchmarks."""
    __slots__ = ('x', 'y', 'z', '_tmp')
    def __init__(self):
        self.x = 1
        self.y = 2
        self.z = 3


class Animal:
    """Target class for isinstance/issubclass benchmarks."""
    pass


class Dog(Animal):
    """Subclass for isinstance/issubclass benchmarks."""
    pass


def gen():
    """Generator for next() benchmark."""
    while True:
        yield 1


# ── Benchmark functions ─────────────────────────────────────────────────────
# Each function calls a single METH_FASTCALL builtin in a tight inner loop.
# The inner loop prevents the JIT from hoisting the call out of the function.

def bench_isinstance(n):
    """isinstance(obj, cls) — METH_FASTCALL, 2 args."""
    obj = Dog()
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            total += isinstance(obj, Animal)
    return total


def bench_issubclass(n):
    """issubclass(cls, base) — METH_FASTCALL, 2 args."""
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            total += issubclass(Dog, Animal)
    return total


def bench_hasattr(n):
    """hasattr(obj, name) — METH_FASTCALL, 2 args."""
    obj = Obj()
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            total += hasattr(obj, 'x')
    return total


def bench_getattr_2(n):
    """getattr(obj, name) — METH_FASTCALL, 2 args."""
    obj = Obj()
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            total += getattr(obj, 'x')
    return total


def bench_getattr_3(n):
    """getattr(obj, name, default) — METH_FASTCALL, 3 args."""
    obj = Obj()
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            v = getattr(obj, 'missing', 42)
            total += v
    return total


def bench_next(n):
    """next(iterator) — METH_FASTCALL, 1 arg."""
    g = gen()
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            total += next(g)
    return total


def bench_next_default(n):
    """next(iterator, default) — METH_FASTCALL, 2 args."""
    g = gen()
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            total += next(g, 0)
    return total


def bench_divmod(n):
    """divmod(a, b) — METH_FASTCALL, 2 args."""
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            q, r = divmod(1000007, 37)
            total += q
    return total


def bench_format(n):
    """format(value, spec) — METH_FASTCALL, 1-2 args."""
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            s = format(3.14159, '.2f')
            total += len(s)
    return total


def bench_iter(n):
    """iter(iterable) — METH_FASTCALL, 1 arg."""
    data = [1, 2, 3]
    total = 0
    for _ in range(n):
        for _ in range(INNER_ITERS):
            it = iter(data)
            total += 1
    return total


# ── Runner ───────────────────────────────────────────────────────────────────

BENCHMARKS = [
    ("isinstance",      bench_isinstance,   BENCH_ITERS * INNER_ITERS),
    ("issubclass",      bench_issubclass,   BENCH_ITERS * INNER_ITERS),
    ("hasattr",         bench_hasattr,      BENCH_ITERS * INNER_ITERS),
    ("getattr_2",       bench_getattr_2,    BENCH_ITERS * INNER_ITERS),
    ("getattr_3",       bench_getattr_3,    BENCH_ITERS * INNER_ITERS),
    ("next",            bench_next,         BENCH_ITERS * INNER_ITERS),
    ("next_default",    bench_next_default, BENCH_ITERS * INNER_ITERS),
    ("divmod",          bench_divmod,       BENCH_ITERS * INNER_ITERS),
    ("format",          bench_format,       BENCH_ITERS * INNER_ITERS),
    ("iter",            bench_iter,         BENCH_ITERS * INNER_ITERS),
]


def check_jit():
    """Check if CinderX JIT is available and enabled.

    Returns cinderjit module if JIT is available AND not disabled.
    CINDERX_DISABLE_JIT=1 means: import still works but we should not
    force-compile — return None to treat as interpreter baseline.
    """
    if os.environ.get("CINDERX_DISABLE_JIT"):
        return None
    try:
        import cinderx
        cinderx.init()
        import cinderjit
        return cinderjit
    except (ImportError, AttributeError):
        return None


def warmup(func):
    """Warmup function to trigger JIT compilation."""
    for _ in range(WARMUP_ITERS):
        func(1)


def time_benchmark(func, n):
    """Time a single benchmark run. Returns seconds."""
    t0 = time.perf_counter()
    result = func(n)
    t1 = time.perf_counter()
    return t1 - t0, result


def main():
    print("=" * 70)
    print("CALL_BUILTIN_FAST Benchmark — METH_FASTCALL Builtin Overhead")
    print("=" * 70)
    print(f"Python:       {sys.version}")
    print(f"Platform:     {sys.platform}")
    print(f"BENCH_ITERS:  {BENCH_ITERS}")
    print(f"INNER_ITERS:  {INNER_ITERS}")
    print(f"REPEATS:      {REPEATS}")
    print(f"WARMUP_ITERS: {WARMUP_ITERS}")
    print()

    cinderjit = check_jit()
    jit_mode = "JIT" if cinderjit else "INTERPRETER"
    print(f"Mode:         {jit_mode}")
    if os.environ.get("CINDERX_DISABLE_JIT"):
        print(f"              (CINDERX_DISABLE_JIT set)")
    print()

    # ── Correctness checks ───────────────────────────────────────────────
    print("Correctness checks:")
    assert bench_isinstance(1) == INNER_ITERS, "isinstance correctness"
    assert bench_issubclass(1) == INNER_ITERS, "issubclass correctness"
    assert bench_hasattr(1) == INNER_ITERS, "hasattr correctness"
    assert bench_getattr_2(1) == INNER_ITERS, "getattr_2 correctness"
    assert bench_getattr_3(1) == 42 * INNER_ITERS, "getattr_3 correctness"
    assert bench_next(1) == INNER_ITERS, "next correctness"
    assert bench_next_default(1) == INNER_ITERS, "next_default correctness"
    assert bench_divmod(1) > 0, "divmod correctness"
    assert bench_format(1) > 0, "format correctness"
    assert bench_iter(1) == INNER_ITERS, "iter correctness"
    print("  All passed.")
    print()

    # ── Warmup + Force JIT ────────────────────────────────────────────────
    print("Warmup + JIT compilation:")
    for name, func, _ in BENCHMARKS:
        warmup(func)
        if cinderjit:
            try:
                cinderjit.force_compile(func)
            except Exception as e:
                print(f"  {name:20s} force_compile failed: {e}")
        compiled = "?"
        if cinderjit:
            # is_jit_compiled() is broken on AArch64 — use identity in get_compiled_functions()
            in_compiled = func in cinderjit.get_compiled_functions()
            compiled = "JIT" if in_compiled else "INTERP"
        print(f"  {name:20s} [{compiled}]")
    print()

    # ── JIT compilation verification ─────────────────────────────────────
    if cinderjit:
        print("JIT compilation verification:")
        print("  (NOTE: is_jit_compiled() broken on AArch64 — using get_compiled_functions())")
        compiled_funcs = cinderjit.get_compiled_functions()
        all_compiled = True
        for name, func, _ in BENCHMARKS:
            in_compiled = func in compiled_funcs
            status = "PASS" if in_compiled else "FAIL"
            print(f"  {name:20s} {status}")
            if not in_compiled:
                all_compiled = False
        if not all_compiled:
            print("  WARNING: Some functions not JIT-compiled. Results may not")
            print("  reflect JIT CALL specialisation performance.")
        else:
            print("  All functions JIT-compiled.")
        print()

    # ── Benchmark ────────────────────────────────────────────────────────
    print(f"{'Benchmark':20s} {'Median (s)':>12s} {'calls/sec':>14s} {'ns/call':>10s}")
    print("-" * 60)

    results = {}
    for name, func, total_calls in BENCHMARKS:
        times = []
        for _ in range(REPEATS):
            elapsed, _ = time_benchmark(func, BENCH_ITERS)
            times.append(elapsed)

        times.sort()
        median = times[len(times) // 2]
        calls_per_sec = total_calls / median
        ns_per_call = median / total_calls * 1e9

        results[name] = {
            'median': median,
            'calls_per_sec': calls_per_sec,
            'ns_per_call': ns_per_call,
            'all_times': times,
        }

        print(f"  {name:20s} {median:10.4f}   {calls_per_sec:12,.0f}   {ns_per_call:8.1f}")

    print()
    print("=" * 70)
    print(f"Mode: {jit_mode}")
    print()

    # ── Summary for A/B comparison ───────────────────────────────────────
    print("To compute speedup, run this script twice:")
    print("  A) With JIT:     python benchmark_call_builtin_fast.py")
    print("  B) Without JIT:  CINDERX_DISABLE_JIT=1 python benchmark_call_builtin_fast.py")
    print("Compare ns/call values. Lower is better.")
    print()

    # ── Raw data ─────────────────────────────────────────────────────────
    print("Raw timing data (all repeats, seconds):")
    for name, func, _ in BENCHMARKS:
        r = results[name]
        times_str = ", ".join(f"{t:.4f}" for t in r['all_times'])
        print(f"  {name:20s} [{times_str}]")


if __name__ == "__main__":
    main()
