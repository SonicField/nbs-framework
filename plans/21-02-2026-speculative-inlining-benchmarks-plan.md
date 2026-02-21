# Plan: CinderX Speculative Inlining Benchmarks & Performance Target

**Date:** 21-02-2026
**Owner:** Supervisor (team coordination)
**Terminal Goal:** ≥1.25x geometric mean speedup from speculative inlining across all benchmarks
**Commit Under Test:** 725004da

## Current State

### Completed
1. Speculative inlining implementation (commit 725004da) — clean on correctness
2. Benchmark suite (benchmark_cinderx_full.sh) — 20 benchmarks, ABBA design, JIT falsification gate
3. Test suite (test_cinderx_full.sh) — 3 smoke + 4 adversarial tests
4. Falsification methodology documented (benchmark_results/falsification-methodology.md)
5. First benchmark run: 13/20 benchmarks ran, 7 failed (6 super() bug, 1 exception_handling)

### Benchmark Results (First Run)
| Benchmark | Inliner ON/OFF Speedup |
|-----------|----------------------|
| method_calls | **1.19x** |
| nested_calls | **1.09x** |
| fibonacci | **1.06x** |
| function_calls | 1.02x |
| All others (9) | ~1.00x |
| **Geometric mean (13 benchmarks)** | **~1.03x** |

### Gap to Target
- Current: ~1.03x geomean
- Target: ≥1.25x geomean
- Gap: ~0.22x — requires significant inliner tuning

## Open Work Streams

### Stream 1: Pre-existing CinderX Bugs (Blocking 7 Benchmarks)

#### super().__init__() Bug
- **Status:** Under active investigation
- **Impact:** Blocks 5 module benchmarks (context_manager, decorator_chain, deep_class, kwargs_dispatch, nn_module_forward)
- **Root Cause:** CinderX eval loop or JIT compilation trigger path corrupts method dispatch for ≥4-level super() chains when compile_after_n_calls threshold is reached
- **Time-box:** 1 more hour, then pivot

#### exception_handling Crash (POTENTIALLY OUR REGRESSION)
- **Status:** NEEDS IMMEDIATE FALSIFICATION
- **Impact:** Crashes with inliner ON, works with inliner OFF
- **Falsifier:** Run with disable_hir_inliner() — if passes, our commit has an exception handling bug
- **Priority:** Higher than super() bug because this may be our regression

#### nqueens LICM SEGFAULT
- **Status:** Confirmed pre-existing (crashes with inliner OFF too)
- **Impact:** 1 benchmark blocked
- **Root Cause:** LICM pass hoists GuardType from loop body to preheader incorrectly

### Stream 2: Inliner Tuning for 1.25x Target

**Requires deeper inlining of:**
- Decorator chains (functools.wraps wrapper dispatch)
- nn.Module __getattr__/__setattr__/__call__ patterns
- Context manager __enter__/__exit__ dispatch
- kwargs dispatch through *args/**kwargs forwarding

**Possible tuning levers:**
- Increase max inlining depth
- Adjust inlining cost model thresholds
- Add speculative inlining for decorator-style wrapper functions
- Target polymorphic call sites with profile-guided inlining

### Stream 3: Documentation & Publishing

- Benchmark results to /benchmark_results/
- Description document for the changes
- Gatekeeper review before push
- Commit and push via pty-session

## Task Assignments (Current)

| Agent | Task | Priority |
|-------|------|----------|
| testkeeper | Falsify exception_handling crash (our regression?) | **CRITICAL** |
| generalist + hypergrep | super() bug fix (time-boxed 1hr) | HIGH |
| claude | Inliner cost model tuning for 1.25x target | HIGH |
| theologian | Plan/progress doc, review scripts/results | MEDIUM |
| scribe | Decision logging, benchmark description doc | MEDIUM |
| gatekeeper | Pre-push review when ready | MEDIUM |

## Success Criteria

1. All 20 benchmarks run successfully
2. Geometric mean speedup ≥1.25x (inliner ON vs OFF)
3. No correctness regressions from commit 725004da
4. Full test suite passes (smoke + adversarial + CPython)
5. Results published to repo, reviewed by gatekeeper, pushed
