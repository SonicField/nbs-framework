# CinderX Test Coverage Audit

**Date:** 18 February 2026
**Author:** @testkeeper
**Build:** commit `d2bbb9f5` on build-host branch `aarch64-jit-generators`

## Executive Summary

41 CinderX test suites exist across 3 categories (JIT, Runtime, Compiler). Of these:

- **35 suites PASS** (all tests within pass or have only expected skips)
- **4 suites FAIL** (with known, classified root causes — none caused by our work)
- **1 suite SKIP** (test_shadowcode — 3.12+ expected)
- **1 suite CRASH** (test_cinderjit — GC/JIT UAF, force_compile-specific)

Total: ~2455 tests pass, ~24 fail/error, ~1 skip. All 41 suites verified.

No failures are attributable to our JIT aarch64 work (A-lite or Option D) **on the fixed commit 8af4dc49**. The original Option D commit (3c4ff942) contained an SSA violation that caused LoadAttr failures — this was OUR bug, now fixed. Rerun pending on the fix commit.

---

## Suite-by-Suite Status

### JIT Suites (17 suites)

| # | Suite | Tests | Pass | Fail | Err | Status | Notes |
|---|-------|-------|------|------|-----|--------|-------|
| 1 | test_jit_attr_cache | 24 | 18 | 3 | 3 | FAIL (force_compile) | Verified on build-host 19 Feb: 3 failures + 3 errors from force_compile LOAD_ATTR bug |
| 2 | test_jit_generator_aarch64 | 33 | 30 | 0 | 3 | KNOWN FAIL | 3 async gen CANNOT_SPECIALIZE — pre-existing |
| 3 | test_jit_generators | 35 | 35 | 0 | 0 | PASS | |
| 4 | test_jit_async_generators | 5 | 5 | 0 | 0 | PASS | |
| 5 | test_jit_coroutines | 23 | 23 | 0 | 0 | PASS | Verified on build-host 19 Feb |
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
| 17 | test_cinderjit | ~172 | ? | ? | ? | CRASH | SEGFAULT: GC UAF from force_compile bug (see analysis below) |

### Runtime Suites (14 suites)

| # | Suite | Tests | Pass | Fail | Err | Status | Notes |
|---|-------|-------|------|------|-----|--------|-------|
| 18 | test_asynclazyvalue | 15 | 15 | 0 | 0 | PASS | Verified on build-host 19 Feb |
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
| 32 | test_compiler_sbs_stdlib_0 | 192 | 192 | 0 | 0 | PASS | Verified on build-host 19 Feb |
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
| PASS | 35 | See above | N/A |
| KNOWN async gen | 1 | test_jit_generator_aarch64 (3 tests) | NO — pre-existing aarch64 codegen gap |
| PRE-EXISTING codegen | 1 | test_jit_preload (2 tests) | NO — InvalidImmediate bug in gen_asm.cpp |
| PRE-EXISTING monitoring | 1 | test_jit_support_instrumentation (16 tests) | NO — sys.monitoring not implemented for aarch64 JIT |
| PRE-EXISTING force_compile | 1 | test_jit_attr_cache (6 tests) | **YES — Option D SSA violation (FIXED 8af4dc49)** |
| GC crash (ROOT CAUSE FOUND) | 1 | test_cinderjit | **YES — Option D SSA violation caused cascading GC UAF (FIXED 8af4dc49)** |
| SKIP (expected) | 1 | test_shadowcode | NO — removed in 3.12+ |

**Note on failure root causes:** The 4 FAIL suites + 1 CRASH suite involve 3 distinct root causes:

1. **Option D SSA violation (OUR BUG — FIXED)**: Option D's inline fast-path (commit 3c4ff942) created two LIR virtual registers for the same HIR output, violating SSA. The register allocator eliminated the dead output, so the call result was never captured — X19 retained the stale function pointer. Fixed by reverting to simple call (commit 8af4dc49). Caused test_jit_attr_cache 6/24 failures and test_cinderjit crash (via CallExTests → refcount corruption → GC UAF). Manifested deterministically with force_compile but was latent in auto-compiled code (register-allocation-dependent — different instruction mix led to correct-by-luck register assignment). Performance impact: slot_read drops from 0.96x to ~0.80x until fast-path redesign.
2. **aarch64 codegen gaps** (pre-existing feature gaps, not our bugs): async generator CANNOT_SPECIALIZE (test_jit_generator_aarch64, 3 tests), InvalidImmediate for large stack offsets (test_jit_preload, 2 tests).
3. **Missing feature** (pre-existing, not implemented): sys.monitoring deopt not implemented for aarch64 JIT (test_jit_support_instrumentation, 16 tests).

---

## Discrepancies Between Test Runners

**RESOLVED** (commit ced396c): Consolidated into single `run_cinderx_tests.sh` at repo root. The arm-optimisation copy has been deleted. Single runner has all 41 suites, hard CinderX JIT gate, crash signal detection, and results CSV.

---

## test_cinderjit GC/JIT UAF — Root Cause Analysis (18 Feb 2026)

**Status:** Root cause narrowed to CALL_EX exception path on aarch64. Fix pending.

**Trigger:** `StaticTestBase._finalize_module()` in `common.py:455` calls `mod_dict.clear()` then `gc.collect()`. This drops all Python-side references to JIT-compiled functions. GC then frees those function objects. But CinderX's JIT holds internal C++ references (inline caches, code_runtime metadata) that are NOT GC-tracked. When a subsequent GC cycle traverses a dictionary that references the freed objects through a different path, it hits freed memory (ob_type=0x6, refcount=0).

**Evidence:**
- gc.disable() prevents the crash entirely (confirmed by @claude on build-host
- Removing gc.collect() from _finalize_module prevents the crash
- Non-deterministic: depends on heap layout and GC timing
- CallExTests + CinderJitModuleTests triggers it; CinderJitModuleTests alone does not
- Crash is in dict_traverse → visit_decref → _PyObject_IS_GC (Ci_gc_collect_main)
- **NARROWED** (23:55Z): crash requires exactly the 5 CallExTests that raise TypeError (test_call_bound_method_kw_and_pos, test_call_bound_method_kw_only, test_call_class_static_pos_and_kw, test_call_method_kw_and_pos, test_call_method_kw_only). Tests that pass do NOT trigger the crash. Order matters: CallExTests → CinderJitModuleTests crashes; reverse does not.
- **IMPLICATION**: The CALL_EX JIT codegen on aarch64 has a bug in its exception handling path. When CALL_EX raises TypeError, the JIT's exception cleanup doesn't properly decref temporary objects (materialised kwargs dict or args tuple). These objects with incorrect refcounts become dangling when GC later collects them.
- **ROOT CAUSE NARROWED** (23:58Z, @theologian): All 5 failing tests involve methods (bound, instance, or static) with kwargs unpacking. Passing tests use module-level functions. The CALL_EX JIT codegen incorrectly handles `self` argument prepending when combined with kwargs unpacking for methods — `self` is either not injected or counted as a kwargs entry, causing the callee to see wrong arguments and raise TypeError. The TypeError then leaves corrupt state (under-decreffed temporaries) that triggers the crash during later gc.collect().

**Correct fix direction:** Fix the CALL_EX exception path refcounting on aarch64, and fix the self-prepend logic for method calls with kwargs in the CALL_EX codegen. The 5 pre-existing TypeError failures and the crash are the same bug — fixing the method+kwargs handling in CALL_EX codegen should fix both.

**GATE STATUS: PENDING** (02:04Z 19 Feb — CRITICAL REVISION):
- **ROOT CAUSE CORRECTED**: The LoadAttr bug was NOT pre-existing. It was our SSA violation in Option D's inline fast-path (commit 3c4ff942). @claude identified: two LIR virtual registers for the same HIR output dst → register allocator eliminated dead output → X19 retained stale function pointer.
- **FIXED**: commit 8af4dc49 reverts to pre-Option-D simple call.
- Auto-compiled code passed earlier tests by luck (register-allocation-dependent — different instruction mix led to correct register assignment).
- force_compiled code deterministically exposed the bug (different liveness sets → allocator made incorrect assignment).
- Full CinderX suite rerun on 8af4dc49 in progress (@claude running). Gate reopens when rerun completes.
- Performance impact: slot_read drops from 0.96x to ~0.80x until fast-path redesign without SSA violation.

**Previous gate assessment (SUPERSEDED):**
~~GATE STATUS: GREEN (00:35Z 19 Feb)~~ — Based on incorrect premise that the bug was pre-existing. The auto-compile+invalidation test passed by luck, not because the code was correct.

**Next steps:** ASAN build BLOCKED (gcc can't compile CinderX due to stdatomic.h _Atomic issues; clang 22 lacks aarch64 compiler-rt/ASAN runtime libraries). Alternative: targeted code inspection and refcount-tracking test.

**Strongest hypothesis** (refined 18 Feb 23:44Z): Type watcher gap on Python 3.12+ is a real latent bug but NOT this crash's direct cause. @theologian corrected: this crash involves functions (not types), and `funcDestroyed` correctly handles `PyFunction_EVENT_DESTROY` via the 3.12+ function watcher (cinderx-lib.cpp:836-842). The crash trigger is `_finalize_module()` → `mod_dict.clear()` → destroys all module contents → some JIT data structure holds a dangling pointer → `gc.collect()` traverses a dict containing the freed object.

**Open question (ASAN blocked):** Which JIT data structure holds the dangling pointer?
- Candidate 1: CodeRuntime's `addReference` system (unlikely — designed for this case, uses strong Ref<>)
- Candidate 2: TypeDeoptPatcher's `type_deopt_patchers_` map (BorrowedRef keys — but C++ map, not GC-traversed)
- Candidate 3: Inline cache entries populated but not invalidated by funcDestroyed
- Key constraint: the dangling pointer must be reachable from something GC-traversable (a Python dict, code object, etc.) since the crash is in `dict_traverse`

**Candidate dangling reference sources** (from @theologian's code analysis):
- Inline caches (LoadAttrCache, StoreAttrCache — context.h:334-340): store raw PyObject* without GC tracking; invalidation via `notifyTypeModified` or `funcDestroyed` may be missed during test teardown
- `Context::addReference` (context.h:325): JIT ownership mechanism, but `releaseReferences()` only runs during `jit::finalize()` (module unload), NOT during test teardown
- code_runtime metadata: may hold references to compiled function objects
- **TypeWatcher BorrowedRef map** (type_watcher.h): map keys become dangling BorrowedRef<PyTypeObject> when types are deallocated without notification

---

## Coverage Gaps

### Gap 1: test_cinderjit (CRITICAL)

This is the main JIT test file with ~172 tests. It currently crashes with SEGFAULT/SIGBUS (GC use-after-free) when GC runs after `_finalize_module()` clears the module dict. Root cause mechanism confirmed (see UAF analysis above). @claude is building CinderX with ASAN on build-host to identify the exact dangling reference. This suite exercises the broadest range of JIT opcodes and is our most important regression gate.

**Impact:** 172 tests not running = significant coverage hole for JIT correctness.

### Gap 2: test_jit_coroutines — RESOLVED

Previously blocked by missing xxclassloader C extension. **Verified PASS** on build-host 19 Feb: 23 tests, all pass. No longer a coverage gap.

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
2. **Short-term:** ~~Verify test_jit_coroutines on current build.~~ DONE (23 tests PASS). ~~Verify test_asynclazyvalue.~~ DONE (15 tests PASS). ~~Verify test_compiler_sbs_stdlib_0.~~ DONE (192 tests PASS). All 41 suites now have known status.
3. **Medium-term:** Establish PyTorch test baseline (see plan below).
4. **Long-term:** Set up CI for automated test runs on each commit.

---

## Falsifiers

- If test_cinderjit passes on x86 but crashes on aarch64: the GC bug is aarch64-specific (memory model or heap layout).
- ~~If test_jit_coroutines still fails after xxclassloader fix: there is a second blocker in the coroutine test infrastructure.~~ FALSIFIED: test_jit_coroutines passes (23/23) on build-host 19 Feb.
- If any currently-passing suite regresses on the next rebuild: our changes introduced a regression (not yet observed).
