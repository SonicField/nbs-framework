# CinderX JIT Deopt Classification — Full Benchmark Suite

**Date:** 26 February 2026
**Author:** generalist
**Source:** aarch64 dev server, branch `aarch64-jit-generators` at `41c82288`
**Script:** `deopt_stats_measure.py` on aarch64 dev server
**Requested by:** Alex (20:15:18Z): "I believe we can get the jit as fast as the adaptive interpreter for all benchmarks — we are just missing optimisations or hitting deopts which are causing jit recompiles."

---

## Executive Summary

All 26 benchmarks in the JIT and specialisation suites were instrumented with `cinderjit.get_and_clear_runtime_stats()` to count deoptimisation events during measurement. Each benchmark received 3 warmup iterations (to reach Tier 2 compilation) followed by 5 measurement iterations with deopt stats capture.

**Result:** Benchmarks split into two distinct categories:

| Category | Count | Cause | Fix |
|----------|-------|-------|-----|
| **Deopt-caused** | 4 | Deopt/reopt loop — GuardType failures on every call | Exponential backoff or guard fix |
| **Structural** | 22 | Zero deopts — JIT code runs correctly but slower than adaptive interpreter | JIT code quality improvements |

---

## Methodology

### Parameters

- **N_ITER:** 100,000 iterations per benchmark call
- **N_WARMUP:** 3 calls (sufficient for inner methods to reach Tier 2 via `cinderjit.auto()` threshold of 1000)
- **N_MEASURE:** 5 calls with timing and deopt capture
- **JIT mode:** `cinderjit.auto()` — compile_after_n_calls=1000, Tier 1→2 at 1000 JIT invocations

### Protocol

1. Import all benchmark functions from `benchmark_cinderx.py`
2. Initialise CinderX with `cinderx.init()` and `cinderjit.auto()`
3. For each benchmark:
   a. Run N_WARMUP iterations (deopts during warmup are expected and discarded)
   b. Call `cinderjit.get_and_clear_runtime_stats()` to clear warmup stats
   c. Run N_MEASURE iterations with `time.perf_counter_ns()` timing
   d. Call `cinderjit.get_and_clear_runtime_stats()` to capture measurement deopts
   e. Parse deopt events: func_qualname, line number, reason, description, count

### Falsification

- **Control benchmark:** fibonacci (2.06x JIT improvement) — expected zero deopts. Confirmed: 0 deopts.
- **Known-deopt benchmark:** deep_class_super (0.52x regression, pilot study confirmed 1.1M deopts). Confirmed: 1,100,000 deopts.
- **Known-structural benchmark:** kwargs_dispatch (0.69x regression, pilot study confirmed 0 deopts). Confirmed: 0 deopts.

---

## Deopt-Caused Benchmarks (4 of 26)

### deep_class_super — 1,100,000 deopts

| Metric | Value |
|--------|-------|
| Mean | 141.10ms |
| JIT/Vanilla ratio | 0.52x |
| Total deopts | 1,100,000 |
| Deopt sites | 4 |
| Deopts per measurement call | 220,000 |

**Deopt sites:**

| Count | Function | Line | Reason |
|-------|----------|------|--------|
| 600,000 | `_DCLayer.__init__` | 1125 | GuardFailure: GuardType |
| 300,000 | `_DCLayer.forward` | 1128 | GuardFailure: dict values check (guilty: `_DCLayer`) |
| 150,000 | `_DCBlock.__init__` | 1134 | GuardFailure: GuardType |
| 50,000 | `_DCModel.__init__` | 1157 | GuardFailure: GuardType |

The deopt counts are proportional to the object hierarchy: 50,000 models × 1 `__init__` = 50K model deopts; 50,000 × 3 blocks = 150K block deopts; 50,000 × 3 × 4 layers = 600K layer deopts. Every `__init__` and `forward` call deopts.

### decorator_chain — 50,000 deopts

| Metric | Value |
|--------|-------|
| Mean | 20.89ms |
| Total deopts | 50,000 |
| Deopt sites | 1 |

**Deopt site:**

| Count | Function | Line | Reason |
|-------|----------|------|--------|
| 50,000 | `_DecoratedCompute.class_op` | 1070 | GuardFailure: GuardType |

### pytorch_cm — 50,000 deopts

| Metric | Value |
|--------|-------|
| Mean | 107.29ms |
| Total deopts | 50,000 |
| Deopt sites | 1 |

**Deopt site:**

| Count | Function | Line | Reason |
|-------|----------|------|--------|
| 50,000 | `_GeneratorContextManager.__exit__` | 144 | UnhandledException: VectorCall |

Note: This is a different deopt reason — `UnhandledException` during VectorCall in a generator-based context manager, not a GuardType failure. This suggests the JIT-compiled `__exit__` path encounters an exception it cannot handle inline.

### nn_module_forward — 40,000 deopts

| Metric | Value |
|--------|-------|
| Mean | 4.17ms |
| JIT/Vanilla ratio | 0.38x |
| Total deopts | 40,000 |
| Deopt sites | 3 |

**Deopt sites:**

| Count | Function | Line | Reason |
|-------|----------|------|--------|
| 20,000 | `bench_deep_class.<locals>.Layer.__init__` | 979 | GuardFailure: GuardType |
| 15,000 | `bench_deep_class.<locals>.Layer.forward` | 982 | GuardFailure: GuardType |
| 5,000 | `bench_deep_class.<locals>.Network.forward` | 989 | GuardFailure: GuardType |

Note: These are inner-class methods (defined inside `bench_deep_class`), unlike `deep_class_super` which uses module-level classes. The deopts fire on every call regardless.

---

## Structural Benchmarks (22 of 26)

All benchmarks below had **zero deopts** during measurement. Their JIT performance characteristics are determined by compiled code quality, not deopt overhead. JIT/Vanilla ratios from the ABBA benchmark run (--compile=auto, --reps=2) are included for context.

| Benchmark | Mean (ms) | Deopts | JIT/Vanilla | Classification |
|-----------|-----------|--------|-------------|----------------|
| list_comp | 2.91 | 0 | 0.80x | STRUCTURAL (regression) |
| module_attr | 3.60 | 0 | — | STRUCTURAL |
| gen_simple | 3.74 | 0 | 0.78x | STRUCTURAL (regression) |
| try_except_callee | 4.67 | 0 | 0.90x | STRUCTURAL (regression) |
| attr_access | 6.26 | 0 | — | STRUCTURAL |
| dict_ops | 6.48 | 0 | 0.90x | STRUCTURAL (regression) |
| gen_nested | 9.02 | 0 | 0.88x | STRUCTURAL (regression) |
| int_arith | 9.05 | 0 | 0.91x | STRUCTURAL (regression) |
| store_subscr | 9.44 | 0 | 0.96x | STRUCTURAL (neutral) |
| func_calls | 9.95 | 0 | 0.90x | STRUCTURAL (regression) |
| positional_dispatch | 11.87 | 0 | 0.66x | STRUCTURAL (regression) |
| import_callee | 12.01 | 0 | 0.99x | STRUCTURAL (neutral) |
| float_arith | 13.47 | 0 | 0.97x | STRUCTURAL (neutral) |
| context_manager | 18.20 | 0 | 0.90x | STRUCTURAL (regression) |
| kwargs_dispatch | 21.26 | 0 | 0.69x | STRUCTURAL (regression) |
| dunder_protocol | 21.27 | 0 | 1.06x | STRUCTURAL (win) |
| richards_full | 23.24 | 0 | 1.68x | STRUCTURAL (win) |
| nbody | 38.06 | 0 | 1.01x | STRUCTURAL (neutral) |
| richards_slots | 183.37 | 0 | 0.99x | STRUCTURAL (neutral) |
| nqueens | 298.36 | 0 | 1.43x | STRUCTURAL (win) |
| spectral_norm | 2335.03 | 0 | 1.08x | STRUCTURAL (win) |
| fibonacci | 3476.96 | 0 | 2.06x | STRUCTURAL (win) |

Note: JIT/Vanilla ratios from supervisor (19:19:42Z). `module_attr` and `attr_access` were added after the ABBA run and lack ratios. Benchmarks marked "win" (ratio >1.0) have zero deopts AND outperform the adaptive interpreter — the JIT is working well for these. Benchmarks marked "regression" (ratio <0.95) have zero deopts but the JIT code is slower — these are the structural code quality issues.

---

## The Deopt/Reopt Loop Mechanism

The 4 deopt-caused benchmarks share a common mechanism, confirmed by code analysis (theologian, 20:22:16Z; gatekeeper, 20:22:57Z):

```
1. JIT compiles inner method with GuardType checks
   (e.g., _DCLayer.__init__ guarding on self type)

2. New function object created (class instantiation or inner-class re-creation)

3. scheduleJitCompile(func) fires automatically
   (pyjit.cpp:3634, registered as function creation hook)

4. reoptFunc(func) re-attaches SAME compiled code
   (pyjit.cpp:786-813, does NOT recompile)

5. JIT code runs → GuardType fails → deopt to interpreter

6. Function finishes in interpreter

7. Next call → goto step 2
```

**Why this loops:** `reoptFunc()` reinstalls the identical compiled code with the same guards that failed previously. There is no mechanism to:
- Count how many times a function has deopted
- Increase the threshold before re-JIT-ing
- Blacklist functions that deopt repeatedly
- Recompile with updated type information

**Why exponential backoff was never implemented:** Confirmed via `git log --all -p -S "backoff"` — the term never appeared in any commit. Alex confirmed (20:26:43Z) it was "probably discussed but not implemented."

---

## Implications

### For the 4 Deopt-Caused Benchmarks

Two remediation paths:

1. **Exponential backoff (deopt management):** After N deopts, stop reinstalling JIT code for that function. Let it run interpreted. Double the threshold each time: 1 deopt → wait 1000 calls, 2 deopts → wait 2000 calls, etc. This would immediately eliminate the 220K deopts/call in `deep_class_super`.

2. **Guard fix (root cause):** Investigate why GuardType fails on same-class instances. The guards should pass for monomorphic call sites where the receiver type is stable. If the guard is checking something that changes per-instance (e.g., `__dict__` version), it may be too strict.

### For the 22 Structural Benchmarks

These split into three sub-categories:
- **5 wins** (fibonacci 2.06x, richards_full 1.68x, nqueens 1.43x, spectral_norm 1.08x, dunder_protocol 1.06x) — JIT working well, no issues
- **5 neutral** (store_subscr, import_callee, float_arith, nbody, richards_slots) — within noise
- **12 regressions** (0.66x–0.91x) — zero deopts but JIT code slower than adaptive interpreter

The 12 structural regressions need JIT code quality improvements. Potential causes:
- Speculative inlining (commit `725004da`) not applying to these call sites (non-monomorphic ICs, or not `CallMethod` instructions)
- Guard overhead on every adapted operation even when types are stable
- Prologue/epilogue cost not amortised for short function bodies
- Dynamic C→C fast path (commit `725004da`) present but effectiveness unclear for these call sites — under investigation in optimisation audit

### Ratio of Problems

The deopt problem is **real but narrow** (4/26 = 15%). The structural problem is **dominant** (22/26 = 85%). Both need fixing, but the structural regressions affect more benchmarks and represent the larger challenge for matching adaptive interpreter performance.

---

## Raw Data

Script: `deopt_stats_measure.py` on aarch64 dev server
Log: `/tmp/deopt_classification.log` on aarch64 dev server
Branch: `aarch64-jit-generators` at `41c82288`
Build: includes Step 6 cold block marking + JIT default-on (`compile_after_n_calls{0}`, overridden by `cinderjit.auto()` to 1000)
