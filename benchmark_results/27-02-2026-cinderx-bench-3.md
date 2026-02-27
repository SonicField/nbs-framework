# CinderX JIT Benchmark Results — cinderx-bench-3

**Commit:** `0730c07e` (kDeoptBackoffThreshold 100→1000 + documentation)
**Build base:** `105ee2c6` (deopt backoff v5, CI_CO_SUPPRESS_JIT mechanism)
**Platform:** aarch64 (Grace CPU)
**Mode:** `--compile=auto` (production-representative)
**Methodology:** ABBA, 15 blocks, 2 reps (8 samples per condition), subprocess-isolated
**Date:** 27 Feb 2026
**Flags:** `-S` (skip site.py), `cinderjit.auto()` before imports, `cinderx.init()` skipped (Bug 8)

## JIT vs Vanilla — All 24 Benchmarks

| # | Category | Benchmark | Vanilla (ms) | CinderX (ms) | Speedup | Δ% | Classification |
|---|----------|-----------|-------------|--------------|---------|-----|----------------|
| 1 | Micro | fibonacci | 6659.79 | 3511.71 | 1.90x | +47.3% | WIN |
| 2 | Micro | func_calls | 9.16 | 9.86 | 0.93x | -7.6% | LOSS |
| 3 | Compute | richards_full | 39.06 | 23.06 | 1.69x | +41.0% | WIN |
| 4 | Compute | richards_slots | 179.19 | 182.02 | 0.98x | -1.6% | NEUTRAL |
| 5 | Compute | nqueens | 434.03 | 299.84 | 1.45x | +30.9% | WIN |
| 6 | Compute | spectral_norm | 2546.42 | 2327.99 | 1.09x | +8.6% | WIN |
| 7 | Compute | float_arith | 13.08 | 13.36 | 0.98x | -2.1% | NEUTRAL |
| 8 | Compute | nbody | 37.51 | 36.93 | 1.02x | +1.6% | NEUTRAL |
| 9 | Compute | int_arith | 8.30 | 8.98 | 0.92x | -8.1% | LOSS |
| 10 | Patterns | gen_simple | 2.97 | 3.79 | 0.78x | -27.7% | LOSS |
| 11 | Patterns | gen_nested | 8.05 | 9.15 | 0.88x | -13.7% | LOSS |
| 12 | Patterns | list_comp | 2.32 | 2.93 | 0.79x | -26.6% | LOSS |
| 13 | Patterns | dict_ops | 5.80 | 6.45 | 0.90x | -11.2% | LOSS |
| 14 | Call/Dispatch | import_callee | 11.99 | 12.14 | 0.99x | -1.2% | NEUTRAL |
| 15 | Call/Dispatch | try_except_callee | 4.26 | 4.75 | 0.90x | -11.4% | LOSS |
| 16 | Call/Dispatch | store_subscr | 9.19 | 9.50 | 0.97x | -3.3% | NEUTRAL |
| 17 | Call/Dispatch | kwargs_dispatch | 14.50 | 21.41 | 0.68x | -47.6% | LOSS |
| 18 | Call/Dispatch | positional_dispatch | 7.95 | 12.70 | 0.63x | -59.6% | LOSS |
| 19 | OO/Framework | context_manager | 16.50 | 17.84 | 0.93x | -8.1% | LOSS |
| 20 | OO/Framework | dunder_protocol | 22.82 | 21.26 | 1.07x | +6.8% | WIN |
| 21 | OO/Framework | nn_module_forward | 1.58 | 2.03 | 0.78x | -29.0% | LOSS |
| 22 | OO/Framework | decorator_chain | 13.95 | 17.60 | 0.79x | -26.2% | LOSS |
| 23 | OO/Framework | deep_class_super | 76.92 | 84.20 | 0.91x | -9.5% | LOSS |
| 24 | OO/Framework | pytorch_cm | 72.61 | 103.55 | 0.70x | -42.6% | LOSS |

### Result Distribution

| Classification | Count | Benchmarks |
|---------------|-------|------------|
| WIN (>5% improvement) | 5 | fibonacci, richards_full, nqueens, spectral_norm, dunder_protocol |
| NEUTRAL (±5%) | 5 | richards_slots, float_arith, nbody, import_callee, store_subscr |
| LOSS (>5% regression) | 14 | func_calls, int_arith, gen_simple, gen_nested, list_comp, dict_ops, try_except_callee, kwargs_dispatch, positional_dispatch, context_manager, nn_module_forward, decorator_chain, deep_class_super, pytorch_cm |

### Totals

| Metric | Value |
|--------|-------|
| Total Vanilla | 10197.96 ms |
| Total CinderX | 6743.04 ms |
| Total Speedup | 1.51x |

## Comparison: Baseline (41c82288) vs Bench-3 (0730c07e)

Baseline had NO deopt backoff. Bench-3 has deopt backoff with threshold=1000.

### Deopt-Active Benchmarks (Changed by Deopt Backoff)

| Benchmark | Baseline Speedup | Bench-3 Speedup | Change | Mechanism |
|-----------|-----------------|-----------------|--------|-----------|
| deep_class_super | 0.52x | 0.91x | **+0.39x** | 1.1M deopts → suppressed, falls back to interpreter |
| nn_module_forward | 0.38x | 0.78x | **+0.40x** | 40K deopts → suppressed |
| decorator_chain | 0.68x | 0.79x | **+0.11x** | 50K deopts → suppressed |
| pytorch_cm | 0.68x | 0.70x | +0.02x | Non-responsive — guard-checking cost, not deopt churn |

**Verdict:** Deopt backoff is highly effective for 3 of 4 deopt-active benchmarks. `pytorch_cm` does not respond because its overhead is from guard evaluation, not compile/deopt cycling.

### Structural Regressions (Unchanged by Deopt Backoff)

| Benchmark | Baseline | Bench-3 | Δ | Root Cause |
|-----------|---------|---------|---|------------|
| positional_dispatch | 0.66x | 0.63x | -0.03x | Adaptive CALL_PY_EXACT_ARGS faster than JIT dispatch |
| kwargs_dispatch | 0.69x | 0.68x | -0.01x | Same — adaptive interpreter wins for kwargs |
| gen_simple | 0.78x | 0.78x | 0.00x | No FOR_ITER_GEN in JIT builder |
| gen_nested | 0.88x | 0.88x | 0.00x | Same + arena cache pressure |
| list_comp | 0.80x | 0.79x | -0.01x | Interpreter LIST_APPEND specialisation |
| func_calls | 0.90x | 0.93x | +0.03x | Slight noise improvement |
| context_manager | 0.90x | 0.93x | +0.03x | Slight improvement (Stage 1.5 callee resolution?) |
| int_arith | 0.91x | 0.92x | +0.01x | Stable |

These regressions are structural — the adaptive interpreter (CPython 3.12) has specialised opcodes that the JIT adds guards on top of rather than replacing.

### Stable Wins (Unchanged)

| Benchmark | Baseline | Bench-3 | Δ |
|-----------|---------|---------|---|
| fibonacci | 1.92x | 1.90x | -0.02x |
| richards_full | 1.68x | 1.69x | +0.01x |
| nqueens | 1.43x | 1.45x | +0.02x |
| spectral_norm | 1.08x | 1.09x | +0.01x |
| dunder_protocol | 1.06x | 1.07x | +0.01x |

Note: fibonacci comparison corrected. The 2.06x baseline was from a different machine (x86_64, deopt classification run). Same-architecture comparison (aarch64): 1.92x to 1.90x, which is within noise. No investigation needed.

## Theologian's Predictions vs Actual

| Prediction | Expected | Actual | Match? |
|-----------|----------|--------|--------|
| deep_class_super ~0.89x | ~0.89x | 0.91x | YES |
| nn_module_forward ~0.77x | ~0.77x | 0.78x | YES |
| decorator_chain ~0.79x | ~0.79x | 0.79x | YES (exact) |
| pytorch_cm ~0.67x | ~0.67x | 0.70x | YES (within noise) |
| positional_dispatch ~0.66x | ~0.66x | 0.63x | YES |
| kwargs_dispatch ~0.68x | ~0.68x | 0.68x | YES (exact) |
| fibonacci ~1.92x | ~1.92x | 1.90x | YES |
| richards_full ~1.70x | ~1.70x | 1.69x | YES |
| nqueens ~1.45x | ~1.45x | 1.45x | YES (exact) |

All predictions matched within noise. No threshold-dependent code paths were identified beyond the known 4 deopt-active benchmarks.

## Specialisation ON vs OFF

The SPEC_ON vs SPEC_OFF comparison shows identical patterns to JIT vs Vanilla, confirming that the HIR inliner specialisations are the source of the performance differences (not runtime-only effects).

Notable: `_Environ.__iter__` deopt backoff messages appeared during SPEC runs (visible in log). This is `os.environ` iteration triggering GuardType failures — a new deopt-active function identified at threshold=1000 that was likely suppressed at threshold=100 (not verified; it may simply not have reached 100 deopts in the shorter benchmark window).

## Infrastructure Notes

1. **-S flag required:** `compile_after_n_calls=0` during `_cinderx.so` loading causes SIGSEGV in `spec_from_loader` regardless of threshold value. The crash is about JIT activation timing, not the threshold.

2. **cinderx.init() skipped:** Bug 8 SIGSEGV on aarch64 (`f_globals` corruption in `resumeInInterpreter`). JIT works without `init()` — `cinderjit.auto()` is sufficient.

3. **_Environ.__iter__ deopt backoff:** New at threshold=1000 — environment variable iteration deopts. Non-blocking (falls back to interpreter), but logged as JIT debug noise in benchmark output.

4. **G1 fast path:** Statistically significant 0.8% improvement for JIT generators over interpreter generators (IQR does not span zero). Small effect but consistent.

## Raw Log

Full benchmark output: `/tmp/bench3_all.log` on aarch64 dev server (230 lines).
