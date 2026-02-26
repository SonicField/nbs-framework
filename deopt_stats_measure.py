"""Deopt stats measurement v2 — full classification of ALL benchmarks.

Classifies each benchmark as:
  DEOPT-CAUSED: deopts > 0 during measurement (fixable with backoff/guard fix)
  STRUCTURAL:   deopts = 0 during measurement (JIT code quality issue)

Runs all 24 JIT benchmarks + 6 specialisation benchmarks (some overlap).
"""
import sys
import json
import time

sys.path.insert(0, "/data/users/alexturner/cinderx_dev/cinderx")

import cinderx
cinderx.init()
import cinderjit
cinderjit.auto()

from benchmark_cinderx import (
    bench_fibonacci, bench_richards_full, bench_richards_slots,
    bench_nqueens, bench_spectral_norm, bench_float_arith,
    bench_gen_simple, bench_gen_nested, bench_list_comp,
    bench_dict_ops, bench_func_calls, bench_import_callee,
    bench_try_except_callee, bench_store_subscr, bench_int_arith,
    bench_context_manager, bench_kwargs_dispatch, bench_positional_dispatch,
    bench_dunder_protocol, bench_deep_class, bench_nbody,
    bench_decorator_chain, bench_deep_class_super, bench_pytorch_cm,
    bench_attr_access, bench_module_attr,
)

N_ITER = 100_000
N_WARMUP = 3
N_MEASURE = 5


def measure_with_deopt_stats(func, name, n_iter):
    """Run benchmark and capture deopt stats during measurement phase."""
    print(f"\n--- {name} ---")

    # Warmup — let JIT compile and reach Tier 2
    for _ in range(N_WARMUP):
        try:
            func(n_iter)
        except Exception as e:
            print(f"  WARMUP ERROR: {e}")
            return {"name": name, "mean_ms": -1, "min_ms": -1,
                    "total_deopts": -1, "deopt_sites": -1, "error": str(e)}

    # Clear stats accumulated during warmup
    cinderjit.get_and_clear_runtime_stats()

    # Measure — capture timing AND deopt stats
    times = []
    for i in range(N_MEASURE):
        t0 = time.perf_counter_ns()
        try:
            func(n_iter)
        except Exception as e:
            print(f"  MEASURE ERROR on iteration {i}: {e}")
            return {"name": name, "mean_ms": -1, "min_ms": -1,
                    "total_deopts": -1, "deopt_sites": -1, "error": str(e)}
        t1 = time.perf_counter_ns()
        times.append((t1 - t0) / 1e6)

    # Get deopt stats accumulated during measurement
    stats = cinderjit.get_and_clear_runtime_stats()
    deopt_events = stats.get("deopt", [])

    mean_ms = sum(times) / len(times)
    min_ms = min(times)

    total_deopts = 0
    deopt_details = []
    for event in deopt_events:
        normals = event.get("normal", {})
        ints = event.get("int", {})
        count = ints.get("count", 0)
        total_deopts += count
        qualname = normals.get("func_qualname", "?")
        reason = normals.get("reason", "?")
        desc = normals.get("description", "?")
        lineno = ints.get("lineno", "?")
        deopt_details.append(f"    [{count:>8}x] {qualname}:{lineno} — {reason}: {desc}")

    classification = "DEOPT-CAUSED" if total_deopts > 0 else "STRUCTURAL"
    print(f"  {mean_ms:>10.2f}ms  deopts={total_deopts:>10}  sites={len(deopt_events):>3}  [{classification}]")
    for detail in deopt_details[:5]:  # Show top 5 deopt sites
        print(detail)
    if len(deopt_details) > 5:
        print(f"    ... and {len(deopt_details) - 5} more sites")

    return {
        "name": name,
        "mean_ms": mean_ms,
        "min_ms": min_ms,
        "total_deopts": total_deopts,
        "deopt_sites": len(deopt_events),
        "classification": classification,
    }


def main():
    print("=" * 70)
    print("DEOPT CLASSIFICATION — ALL BENCHMARKS")
    print(f"N_ITER={N_ITER}, N_WARMUP={N_WARMUP}, N_MEASURE={N_MEASURE}")
    print(f"JIT mode: cinderjit.auto() (compile_after_n_calls=1000)")
    print("=" * 70)

    # All unique benchmarks from JIT_BENCHMARKS + SPEC_BENCHMARKS
    benchmarks = [
        ("fibonacci",           bench_fibonacci),
        ("richards_full",       bench_richards_full),
        ("richards_slots",      bench_richards_slots),
        ("nqueens",             bench_nqueens),
        ("spectral_norm",       bench_spectral_norm),
        ("float_arith",         bench_float_arith),
        ("gen_simple",          bench_gen_simple),
        ("gen_nested",          bench_gen_nested),
        ("list_comp",           bench_list_comp),
        ("dict_ops",            bench_dict_ops),
        ("func_calls",          bench_func_calls),
        ("import_callee",       bench_import_callee),
        ("try_except_callee",   bench_try_except_callee),
        ("store_subscr",        bench_store_subscr),
        ("int_arith",           bench_int_arith),
        ("context_manager",     bench_context_manager),
        ("kwargs_dispatch",     bench_kwargs_dispatch),
        ("positional_dispatch", bench_positional_dispatch),
        ("dunder_protocol",     bench_dunder_protocol),
        ("nn_module_forward",   bench_deep_class),
        ("nbody",               bench_nbody),
        ("decorator_chain",     bench_decorator_chain),
        ("deep_class_super",    bench_deep_class_super),
        ("pytorch_cm",          bench_pytorch_cm),
        ("attr_access",         bench_attr_access),
        ("module_attr",         bench_module_attr),
    ]

    results = []
    for name, func in benchmarks:
        result = measure_with_deopt_stats(func, name, N_ITER)
        results.append(result)

    # Summary tables
    deopt_caused = [r for r in results if r.get("classification") == "DEOPT-CAUSED"]
    structural = [r for r in results if r.get("classification") == "STRUCTURAL"]
    errors = [r for r in results if r.get("error")]

    print(f"\n{'='*70}")
    print("CLASSIFICATION SUMMARY")
    print(f"{'='*70}")

    if deopt_caused:
        print(f"\nDEOPT-CAUSED ({len(deopt_caused)} benchmarks):")
        print(f"  {'Benchmark':<25} {'Mean ms':>10} {'Deopts':>12} {'Sites':>6}")
        print(f"  {'-'*55}")
        for r in sorted(deopt_caused, key=lambda x: x["total_deopts"], reverse=True):
            print(f"  {r['name']:<25} {r['mean_ms']:>10.2f} {r['total_deopts']:>12} {r['deopt_sites']:>6}")

    if structural:
        print(f"\nSTRUCTURAL ({len(structural)} benchmarks):")
        print(f"  {'Benchmark':<25} {'Mean ms':>10}")
        print(f"  {'-'*37}")
        for r in sorted(structural, key=lambda x: x["mean_ms"]):
            print(f"  {r['name']:<25} {r['mean_ms']:>10.2f}")

    if errors:
        print(f"\nERRORS ({len(errors)} benchmarks):")
        for r in errors:
            print(f"  {r['name']}: {r['error']}")

    print(f"\nTOTALS: {len(deopt_caused)} deopt-caused, {len(structural)} structural, {len(errors)} errors")

    # Machine-readable output
    print(f"\n--- JSON ---")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
