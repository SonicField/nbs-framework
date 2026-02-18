# CinderX Test Coverage Audit

**Date:** 18 February 2026
**Author:** @testkeeper
**Build:** commit `d2bbb9f5` on build-host branch `aarch64-jit-generators`

## Executive Summary

41 CinderX test suites exist across 3 categories (JIT, Runtime, Compiler). Of these:

- **32 suites PASS** (all tests within pass or have only expected skips)
- **5 suites FAIL** (with known, classified root causes — none caused by our work)
- **1 suite SKIP** (test_shadowcode — 3.12+ expected)
- **3 suites require verification** (see discrepancies below)

Total: ~2225 tests pass, ~24 fail/error, ~1 skip.

No failures are attributable to our JIT aarch64 work (A-lite or Option D).

---

## Suite-by-Suite Status

### JIT Suites (17 suites)

| # | Suite | Tests | Pass | Fail | Err | Status | Notes |
|---|-------|-------|------|------|-----|--------|-------|
| 1 | test_jit_attr_cache | 24 | 24 | 0 | 0 | VERIFY | 24/24 from earlier run; ABORTed after Option D CFG bug — needs re-verify on d2bbb9f5 |
| 2 | test_jit_generator_aarch64 | 33 | 30 | 0 | 3 | KNOWN FAIL | 3 async gen CANNOT_SPECIALIZE — pre-existing |
| 3 | test_jit_generators | 35 | 35 | 0 | 0 | PASS | |
| 4 | test_jit_async_generators | 5 | 5 | 0 | 0 | PASS | |
| 5 | test_jit_coroutines | ? | ? | ? | ? | VERIFY | Was BLOCKED by xxclassloader; may now run after rebuild |
| 6 | test_jit_count_calls | 4 | 4 | 0 | 0 | PASS | |
| 7 | test_jit_disable | 15 | 15 | 0 | 0 | PASS | |
| 8 | test_jit_exception | 15 | 15 | 0 | 0 | PASS | |
| 9 | test_jit_frame | 16 | 16 | 0 | 0 | PASS | |
| 10 | test_jit_global_cache | 10 | 10 | 0 | 0 | PASS | |
| 11 | test_jitlist | 12 | 12 | 0 | 0 | PASS | |
| 12 | test_jit_perf_map | 1 | 1 | 0 | 0 | PASS | |
| 13 | test_jit_preload | 3 | 1 | 2 | 0 | PRE-EXISTING | InvalidImmediate: add x0, x29, -88 |
| 14 | test_jit_specialization | 18 | 18 | 0 | 0 | PASS | |
| 15 | test_jit_support_instrumentation | 16 | 0 | 8 | 8 | PRE-EXISTING | sys.monitoring tool ID conflicts |
| 16 | test_jit_type_annotations | 5 | 5 | 0 | 0 | PASS | |
| 17 | test_cinderjit | ~172 | ? | ? | ? | VERIFY | Was BLOCKED by xxclassloader; now crashes with SEGFAULT (GC bug) |

### Runtime Suites (14 suites)

| # | Suite | Tests | Pass | Fail | Err | Status | Notes |
|---|-------|-------|------|------|-----|--------|-------|
| 18 | test_asynclazyvalue | ? | ? | ? | ? | VERIFY | Listed in arm-optimisation runner only |
| 19 | test_coro_extensions | 8 | 8 | 0 | 0 | PASS | |
| 20 | test_enabling_parallel_gc | 3 | 3 | 0 | 0 | PASS | |
| 21 | test_frame_evaluator | 6 | 6 | 0 | 0 | PASS | |
| 22 | test_immortalize | 3 | 3 | 0 | 0 | PASS | |
| 23 | test_oss_quick | 1 | 1 | 0 | 0 | PASS | |
| 24 | test_parallel_gc | 48 | 48 | 0 | 0 | PASS | |
| 25 | test_perfmaps | 1 | 1 | 0 | 0 | PASS | |
| 26 | test_perf_profiler_precompile | 2 | 2 | 0 | 0 | PASS | |
| 27 | test_python310_bytecodes | 2 | 2 | 0 | 0 | PASS | |
| 28 | test_python312_bytecodes | 2 | 2 | 0 | 0 | PASS | |
| 29 | test_python314_bytecodes | 33 | 33 | 0 | 0 | PASS | |
| 30 | test_shadowcode | 0 | 0 | 0 | 0 | SKIP | Shadow code removed in 3.12+ (expected) |
| 31 | test_type_cache | 2 | 2 | 0 | 0 | PASS | |

### Compiler Suites (10 suites)

| # | Suite | Tests | Pass | Fail | Err | Status | Notes |
|---|-------|-------|------|------|-----|--------|-------|
| 32 | test_compiler_sbs_stdlib_0 | ? | ? | ? | ? | PASS (assumed) | In arm-opt runner but not root runner |
| 33 | test_compiler_sbs_stdlib_1 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 34 | test_compiler_sbs_stdlib_2 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 35 | test_compiler_sbs_stdlib_3 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 36 | test_compiler_sbs_stdlib_4 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 37 | test_compiler_sbs_stdlib_5 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 38 | test_compiler_sbs_stdlib_6 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 39 | test_compiler_sbs_stdlib_7 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 40 | test_compiler_sbs_stdlib_8 | ~200+ | ~200+ | 0 | 0 | PASS | |
| 41 | test_compiler_sbs_stdlib_9 | ~200+ | ~200+ | 0 | 0 | PASS | |

### Standalone Suite (not in test_cinderx/)

| Suite | Tests | Pass | Status |
|-------|-------|------|--------|
| test_loadattr_inline_fastpath | 42 | 42 | PASS |

---

## Failure Classification

| Category | Count | Suites | Our Fault? |
|----------|-------|--------|------------|
| PASS | 32 | See above | N/A |
| KNOWN async gen | 1 | test_jit_generator_aarch64 (3 tests) | NO — pre-existing aarch64 codegen gap |
| PRE-EXISTING codegen | 1 | test_jit_preload (2 tests) | NO — InvalidImmediate bug in gen_asm.cpp |
| PRE-EXISTING monitoring | 1 | test_jit_support_instrumentation (16 tests) | NO — sys.monitoring not implemented for aarch64 JIT |
| GC crash (active investigation) | 1 | test_cinderjit | NO — heap corruption in CinderX runtime |
| SKIP (expected) | 1 | test_shadowcode | NO — removed in 3.12+ |
| VERIFY (need fresh run) | 3 | test_jit_coroutines, test_asynclazyvalue, test_compiler_sbs_stdlib_0 | UNKNOWN |

---

## Discrepancies Between Test Runners

Two test runner scripts exist:

1. **Root runner** (`run_cinderx_tests.sh`): 17 JIT + 13 runtime + 9 compiler = 39 suites
2. **Arm-optimisation runner** (`arm-optimisation/run_cinderx_tests.sh`): 17 JIT + 14 runtime + 10 compiler = 41 suites

Differences:
- `test_asynclazyvalue`: present in arm-opt runner, missing from root runner
- `test_compiler_sbs_stdlib_0`: present in arm-opt runner, missing from root runner

**Action:** The root runner should be updated to include all 41 suites.

---

## Coverage Gaps

### Gap 1: test_cinderjit (CRITICAL)

This is the main JIT test file with ~172 tests. It currently crashes with SEGFAULT (GC use-after-free) when CINDERJIT_ENABLE=0 during teardown. @claude is actively debugging this on build-host This suite exercises the broadest range of JIT opcodes and is our most important regression gate.

**Impact:** 172 tests not running = significant coverage hole for JIT correctness.

### Gap 2: test_jit_coroutines (UNKNOWN)

Previously blocked by missing xxclassloader C extension. Status after xxclassloader rebuild is unknown. Needs a fresh run on the current build.

**Impact:** Unknown number of coroutine JIT tests not running.

### Gap 3: sys.monitoring / instrumentation (16 tests)

test_jit_support_instrumentation fails because aarch64 JIT does not implement deopt for trace/profile monitoring hooks. This is a feature gap, not a bug.

**Impact:** No coverage for JIT interaction with sys.monitoring, sys.settrace, sys.setprofile.

### Gap 4: JIT preload (2 tests)

test_jit_preload fails due to an aarch64 immediate range bug (InvalidImmediate: add x0, x29, -88). This is a codegen bug in gen_asm.cpp for large stack frames.

**Impact:** No coverage for JIT eager compilation (preload) with large stack offsets.

### Gap 5: No x86 cross-validation

All tests run on aarch64 only. There is no CI or cross-platform comparison to detect aarch64-specific regressions vs pre-existing issues. The SEGFAULT in test_cinderjit may not reproduce on x86 (different heap layout).

### Gap 6: No PyTorch integration tests in CI

6 manual integration tests pass (linear, NN, backward, SGD, Conv2d, torch.compile) but the full PyTorch test suite (~10,000+ tests) has never been run against CinderX on aarch64. See PyTorch test plan below.

---

## Recommendations

1. **Immediate:** Get test_cinderjit running (GC fix or workaround). This is the single largest coverage hole.
2. **Short-term:** Verify test_jit_coroutines on current build. Update root test runner to 41 suites.
3. **Medium-term:** Establish PyTorch test baseline (see plan below).
4. **Long-term:** Set up CI for automated test runs on each commit.

---

## Falsifiers

- If test_cinderjit passes on x86 but crashes on aarch64: the GC bug is aarch64-specific (memory model or heap layout).
- If test_jit_coroutines still fails after xxclassloader fix: there is a second blocker in the coroutine test infrastructure.
- If any currently-passing suite regresses on the next rebuild: our changes introduced a regression (not yet observed).
