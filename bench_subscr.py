"""
bench_subscr.py — Benchmark for BINARY_SUBSCR specialisations.

Measures SPEC_ON vs SPEC_OFF gains for:
  - BINARY_SUBSCR_LIST_INT   (list[int_index])
  - BINARY_SUBSCR_TUPLE_INT  (tuple[int_index])
  - BINARY_SUBSCR_DICT       (dict[key])

Each benchmark runs a tight loop doing subscript operations.
The functions are designed to be JIT-compiled by CinderX.

Usage:
  # With specialised opcodes (SPEC_ON):
  PYTHONJITLISTSIZE=1 PYTHONJITENABLE=1 python3 bench_subscr.py

  # Without specialised opcodes (SPEC_OFF — set in CinderX config):
  PYTHONJITLISTSIZE=1 PYTHONJITENABLE=1 CINDERJIT_DISABLE_SPECIALIZED_OPCODES=1 python3 bench_subscr.py
"""

import sys
import time


def bench_list_subscr(data, indices, iterations):
    """Tight loop: list[int] subscript.
    CPython specialises to BINARY_SUBSCR_LIST_INT.
    JIT should lower to IndexUnbox + CheckSequenceBounds + LoadArrayItem."""
    total = 0
    for _ in range(iterations):
        for idx in indices:
            total += data[idx]
    return total


def bench_tuple_subscr(data, indices, iterations):
    """Tight loop: tuple[int] subscript.
    CPython specialises to BINARY_SUBSCR_TUPLE_INT.
    JIT should lower to IndexUnbox + CheckSequenceBounds + LoadArrayItem."""
    total = 0
    for _ in range(iterations):
        for idx in indices:
            total += data[idx]
    return total


def bench_dict_subscr(data, keys, iterations):
    """Tight loop: dict[key] subscript.
    CPython specialises to BINARY_SUBSCR_DICT.
    JIT should lower to DictSubscr."""
    total = 0
    for _ in range(iterations):
        for k in keys:
            total += data[k]
    return total


def run_benchmark(name, func, *args, warmup=5, trials=5):
    """Run benchmark with warmup and multiple trials. Return median time."""
    # Warmup — ensure JIT compilation
    for _ in range(warmup):
        func(*args)

    times = []
    for _ in range(trials):
        t0 = time.perf_counter_ns()
        func(*args)
        t1 = time.perf_counter_ns()
        times.append((t1 - t0) / 1e6)  # ms

    times.sort()
    median = times[len(times) // 2]
    print(f"  {name:30s}  median={median:8.2f}ms  "
          f"all={[f'{t:.2f}' for t in times]}")
    return median


def main():
    print("=== BINARY_SUBSCR Benchmark ===")
    print()

    # Try to init CinderX JIT
    # NOTE: Do NOT call enable_specialized_opcodes() here — that overrides
    # CINDERJIT_DISABLE_SPECIALIZED_OPCODES env var.  Let CinderX respect
    # the environment so SPEC_ON/SPEC_OFF A/B testing works correctly.
    import os
    spec_disabled = os.environ.get("CINDERJIT_DISABLE_SPECIALIZED_OPCODES", "")
    try:
        import cinderx
        cinderx.init()
        import cinderjit
        cinderjit.auto()
        if not spec_disabled:
            try:
                cinderjit.enable_specialized_opcodes()
                print("CinderX JIT: ON, specialized_opcodes: ON")
            except AttributeError:
                print("CinderX JIT: ON, specialized_opcodes: N/A (older build)")
        else:
            print("CinderX JIT: ON, specialized_opcodes: OFF (env override)")
    except ImportError:
        print("CinderX JIT: NOT AVAILABLE (running on CPython)")

    print()

    # --- Setup data ---
    N = 1000
    iterations = 5000

    # List subscript data
    list_data = list(range(N))
    list_indices = list(range(N))

    # Tuple subscript data
    tuple_data = tuple(range(N))
    tuple_indices = list(range(N))

    # Dict subscript data
    dict_data = {i: i for i in range(N)}
    dict_keys = list(range(N))

    # --- Warmup all functions to trigger JIT ---
    print("Warming up JIT...")
    for _ in range(100):
        bench_list_subscr(list_data, list_indices, 1)
        bench_tuple_subscr(tuple_data, tuple_indices, 1)
        bench_dict_subscr(dict_data, dict_keys, 1)
    print()

    # --- Run benchmarks ---
    print("Running benchmarks:")
    t_list = run_benchmark("BINARY_SUBSCR_LIST_INT",
                           bench_list_subscr, list_data, list_indices, iterations)
    t_tuple = run_benchmark("BINARY_SUBSCR_TUPLE_INT",
                            bench_tuple_subscr, tuple_data, tuple_indices, iterations)
    t_dict = run_benchmark("BINARY_SUBSCR_DICT",
                           bench_dict_subscr, dict_data, dict_keys, iterations)

    print()
    print(f"Summary:  list={t_list:.2f}ms  tuple={t_tuple:.2f}ms  dict={t_dict:.2f}ms")


if __name__ == "__main__":
    main()
