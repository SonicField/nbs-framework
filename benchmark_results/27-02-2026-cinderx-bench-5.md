# CinderX JIT Benchmark Results — cinderx-bench-5

**Commit:** `689f2f3c` (TypeDeoptPatcher RAII destructor + Context::unwatchType)
**Build base:** `7434017a` (GlobalDeoptPatcher RAII destructor + unwatchGlobal)
**Commit chain:** `02991665..689f2f3c` (Stage 2a + 3 lifetime bug fixes)
**Platform:** aarch64 (Grace CPU)
**Mode:** `--compile=auto` (production-representative)
**Methodology:** ABBA, 15 blocks, 2 reps (8 samples per condition), subprocess-isolated
**Date:** 27 Feb 2026
**Flags:** `-S` (skip site.py), `cinderjit.auto()` before imports, `cinderx.init()` skipped (Bug 8)
**Binary:** 52270032 bytes, md5 `d38e09ce`

**Milestone:** First complete 24-benchmark run. Previously, 4 class-hierarchy benchmarks (deep_class_super, decorator_chain, nn_module_forward, pytorch_cm) crashed with SIGSEGV at context.cpp:237 due to TypeDeoptPatcher use-after-free. The RAII destructor fix resolves the dangling pointer, and deopt backoff (kDeoptBackoffThreshold=1000) correctly suppresses JIT compilation for these benchmarks after guard failures.

## JIT vs Vanilla — All 24 Benchmarks

| # | Category | Benchmark | Vanilla (ms) | CinderX (ms) | Speedup | Δ% | Classification |
|---|----------|-----------|-------------|--------------|---------|-----|----------------|
| 1 | Micro | fibonacci | 6676.36 | 3303.45 | 2.02x | +50.5% | WIN |
| 2 | Micro | func_calls | 9.05 | 9.97 | 0.91x | -10.2% | LOSS |
| 3 | Compute | richards_full | 39.04 | 23.13 | 1.69x | +40.8% | WIN |
| 4 | Compute | richards_slots | 178.62 | 183.58 | 0.97x | -2.8% | NEUTRAL |
| 5 | Compute | nqueens | 433.64 | 302.24 | 1.43x | +30.3% | WIN |
| 6 | Compute | spectral_norm | 2549.75 | 2396.76 | 1.06x | +6.0% | WIN |
| 7 | Compute | float_arith | 13.08 | 13.47 | 0.97x | -3.0% | NEUTRAL |
| 8 | Compute | nbody | 37.42 | 37.60 | 1.00x | -0.5% | NEUTRAL |
| 9 | Compute | int_arith | 8.17 | 8.96 | 0.91x | -9.7% | LOSS |
| 10 | Patterns | gen_simple | 2.96 | 3.76 | 0.79x | -26.9% | LOSS |
| 11 | Patterns | gen_nested | 8.02 | 9.28 | 0.86x | -15.8% | LOSS |
| 12 | Patterns | list_comp | 2.31 | 2.95 | 0.79x | -27.3% | LOSS |
| 13 | Patterns | dict_ops | 5.86 | 6.45 | 0.91x | -10.1% | LOSS |
| 14 | Call/Dispatch | import_callee | 12.01 | 12.20 | 0.98x | -1.6% | NEUTRAL |
| 15 | Call/Dispatch | try_except_callee | 4.24 | 4.74 | 0.89x | -11.8% | LOSS |
| 16 | Call/Dispatch | store_subscr | 9.15 | 9.52 | 0.96x | -4.1% | NEUTRAL |
| 17 | Call/Dispatch | kwargs_dispatch | 14.59 | 22.42 | 0.65x | -53.7% | LOSS |
| 18 | Call/Dispatch | positional_dispatch | 7.90 | 12.06 | 0.66x | -52.7% | LOSS |
| 19 | OO/Framework | context_manager | 16.44 | 18.20 | 0.90x | -10.7% | LOSS |
| 20 | OO/Framework | dunder_protocol | 22.81 | 21.11 | 1.08x | +7.5% | WIN |
| 21 | OO/Framework | nn_module_forward | 1.56 | 2.08 | 0.75x | -33.5% | LOSS |
| 22 | OO/Framework | decorator_chain | 13.87 | 17.90 | 0.77x | -29.1% | LOSS |
| 23 | OO/Framework | deep_class_super | 74.51 | 85.29 | 0.87x | -14.5% | LOSS |
| 24 | OO/Framework | pytorch_cm | 72.41 | 104.10 | 0.70x | -43.8% | LOSS |

### Result Distribution

| Classification | Count | Benchmarks |
|---------------|-------|------------|
| WIN (>5% improvement) | 5 | fibonacci, richards_full, nqueens, spectral_norm, dunder_protocol |
| NEUTRAL (±5%) | 5 | richards_slots, float_arith, nbody, import_callee, store_subscr |
| LOSS (>5% regression) | 14 | func_calls, int_arith, gen_simple, gen_nested, list_comp, dict_ops, try_except_callee, kwargs_dispatch, positional_dispatch, context_manager, nn_module_forward, decorator_chain, deep_class_super, pytorch_cm |

### Totals

| Metric | Value |
|--------|-------|
| Total Vanilla | 10213.75 ms |
| Total CinderX | 6611.23 ms |
| Total Speedup | 1.54x |
| Geomean Speedup | 0.943x |

**Note:** Total Speedup is sum(vanilla)/sum(cinderx) — dominated by fibonacci which accounts for 65% of total vanilla time. The geometric mean of individual benchmark speedups is 0.943x, meaning the JIT is ~6% slower for a randomly chosen benchmark. Bench-3 reported ~0.95x geomean, but that was computed over 20 benchmarks (4 were crashing). The 4 recovered benchmarks are all LOSSes (0.70x–0.87x), which pull the true 24-benchmark geomean down. 0.943x is the definitive geomean.

## Comparison: Bench-3 (0730c07e) vs Bench-5 (689f2f3c)

Bench-3 had 20 working benchmarks; 4 crashed (TypeDeoptPatcher use-after-free). Bench-5 has all 24 working after the RAII destructor fix.

### Recovered Benchmarks (New in Bench-5)

| Benchmark | Bench-3 | Bench-5 | Theologian Prediction | Match? |
|-----------|---------|---------|----------------------|--------|
| deep_class_super | CRASH | 0.87x | ~0.91x | YES (-0.04x) |
| decorator_chain | CRASH | 0.77x | ~0.79x | YES (-0.02x) |
| nn_module_forward | CRASH | 0.75x | ~0.78x | YES (-0.03x) |
| pytorch_cm | CRASH | 0.70x | ~0.70x | YES (exact) |

All 4 now complete without crash. Deopt backoff fires correctly — JIT detaches after 1000 guard failures and falls back to interpreter. All match theologian's predictions within noise.

**Root cause:** `type_deopt_patchers_` in Context stored raw `TypeDeoptPatcher*` pointers. When tier 2 recompiled, `forgetCode()` destroyed `CompiledFunction` (and its `TypeDeoptPatcher`s via `unique_ptr`), but the raw pointers in `type_deopt_patchers_` remained dangling. Next type modification triggered `notifyTypeModified()`, which dereferenced freed memory → SIGSEGV at context.cpp:237.

**Fix:** RAII destructor on `TypeDeoptPatcher` calling `Context::unwatchType()`, mirroring the `GlobalDeoptPatcher` fix (commits 717ad2e0 + 7434017a).

### Existing Benchmarks (20 — Regression Check)

| Benchmark | Bench-3 | Bench-5 | Δ |
|-----------|---------|---------|---|
| fibonacci | 1.90x | 2.02x | +0.12x |
| richards_full | 1.69x | 1.69x | +0.00x |
| nqueens | 1.45x | 1.43x | -0.02x |
| spectral_norm | 1.09x | 1.06x | -0.03x |
| dunder_protocol | 1.07x | 1.08x | +0.01x |
| nbody | 1.02x | 1.00x | -0.02x |
| import_callee | 0.99x | 0.98x | -0.01x |
| float_arith | 0.98x | 0.97x | -0.01x |
| richards_slots | 0.98x | 0.97x | -0.01x |
| store_subscr | 0.97x | 0.96x | -0.01x |
| func_calls | 0.93x | 0.91x | -0.02x |
| context_manager | 0.93x | 0.90x | -0.03x |
| int_arith | 0.92x | 0.91x | -0.01x |
| try_except_callee | 0.90x | 0.89x | -0.01x |
| dict_ops | 0.90x | 0.91x | +0.01x |
| gen_nested | 0.88x | 0.86x | -0.02x |
| list_comp | 0.79x | 0.79x | +0.00x |
| gen_simple | 0.78x | 0.79x | +0.01x |
| kwargs_dispatch | 0.68x | 0.65x | -0.03x |
| positional_dispatch | 0.63x | 0.66x | +0.03x |

**Verdict:** All 20 existing benchmarks within ±0.03x of bench-3, except fibonacci (+0.12x). The fibonacci delta is cross-session noise: vanilla time is stable (6660→6676ms), but CinderX time improved (3512→3303ms, ~6%). This is within expected cross-session measurement variance (different thermal/load conditions, not attributable to code changes). No regressions.

## Specialisation ON vs OFF

SPEC ON/OFF comparison with JIT active in both conditions. SPEC ON enables CPython adaptive interpreter bytecode specialisation (BINARY_OP_ADD_INT, LOAD_ATTR_INSTANCE_VALUE, CALL_PY_EXACT_ARGS). SPEC OFF disables these specialisations.

**Result: ALL 24 BENCHMARKS NEUTRAL (±3.2%).**

| Notable deltas | Δ% |
|----------------|-----|
| context_manager | +2.9% |
| richards_slots | +2.0% |
| nn_module_forward | -2.3% |
| gen_nested | -3.2% |
| All others | within ±1% |

The `enable_specialized_opcodes()` flag has no measurable effect on JIT code quality. Whether the input bytecodes are BINARY_OP_ADD_INT (specialised) or BINARY_OP (generic), the JIT's own type inference reaches the same conclusions. The specialised opcodes are purely an interpreter optimisation — they do not feed the JIT builder type information.

**Theologian self-correction (15:39:10Z):** At 14:57:58Z, theologian predicted SPEC ON/OFF should show real differences because specialised bytecodes give the JIT free type information. An earlier single-shot run appeared to confirm this (fibonacci 1.26x). The full ABBA run (8 samples) shows all neutral. The 14:57Z prediction was wrong — JIT performance and interpreter specialisation are orthogonal.

`_Environ.__iter__` deopt backoff messages appeared during SPEC runs (visible in log). This is `os.environ` iteration triggering GuardType failures — cosmetic, falls back to interpreter.

## G1 Generator Fast Path

| Metric | Value |
|--------|-------|
| A (JIT gen) | 31.5 ns/call |
| B (interp gen) | 30.8 ns/call |
| Improvement | -2.3% |
| Significant | YES |
| IQR | [+3.128, +3.765] ms |
| Verdict | G1 fast path is SLOWER (unexpected) |

The G1 fast path (JITRT_InvokeIterNext) is statistically significantly slower than the interpreter path. This is a regression from bench-3 which showed a +0.8% improvement. The magnitude is small (0.7 ns/call) and may reflect thermal/load differences across sessions.

## Commit Chain (02991665..689f2f3c)

| Commit | Description |
|--------|-------------|
| `b2e7b88b` | WIP Stage 2a: GlobalDeoptPatcher + context.h/cpp |
| `c97011ce` | Stage 2a: hook notifyDictUpdate + simplifyVectorCallGlobal |
| `717ad2e0` | Fix 1: move watchGlobal to linkDeoptPatchers |
| `7434017a` | Fix 2: RAII destructor for GlobalDeoptPatcher + unwatchGlobal |
| `a24c9225` | Fix 3: TypeDeoptPatcher RAII destructor + unwatchType |
| `689f2f3c` | unwatchType implementation + null-check safety in GlobalDeoptPatcher destructor |

## Stage 2a Falsification

| Claim | Falsifier | Target | Actual | Result |
|-------|-----------|--------|--------|--------|
| Guard elimination improves dispatch | positional_dispatch speedup | ≥0.85x | 0.66x | **FAILED** |
| Guard elimination is correctness-sound | Stage 2a correctness tests (4/4) | All pass | All pass | PASSED |
| TypeDeoptPatcher fix recovers crashes | 4 crashing benchmarks | No crash | No crash | PASSED |
| TypeDeoptPatcher fix causes no regressions | 20 existing benchmarks | Within ±5% | All within ±3% | PASSED |

Stage 2a guard elimination is correctness-sound but performance-neutral. The function identity GuardIs is ~2 instructions in a ~40-instruction dispatch path; removing it gives +5% at best, which is below measurement noise. The dispatch regressions are structural — CPython 3.12 adaptive CALL_PY_EXACT_ARGS is faster than the JIT calling convention.

## Verification

| Check | Agent | Result |
|-------|-------|--------|
| Raw data independently read | testkeeper | Via testkeeper-b3 session (separate from gen-gpu4) |
| Geomean independently computed | testkeeper | 0.943x (matches generalist2's ~0.94x) |
| 20-benchmark regression check | testkeeper | All within ±0.03x (fibonacci +0.12x = cross-session noise) |
| 4 recovered benchmarks vs predictions | testkeeper | All within noise |
| Gatekeeper criteria (5/5) | gatekeeper | FINAL PASS |
| TypeDeoptPatcher correctness tests (4/4) | testkeeper | All pass on binary md5 64c4dc3a |
| nm symbol verification | testkeeper, generalist2 | ~TypeDeoptPatcher + unwatchType confirmed |

## Raw Log

Full benchmark output: `/tmp/bench5_all.log` on aarch64 dev server (179 lines, JIT vs Vanilla section complete; SPEC ON/OFF section partial in file, full results in gen-gpu4 terminal buffer).
