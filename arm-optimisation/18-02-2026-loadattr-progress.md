# LOAD_ATTR Optimisation Progress Log

## Date: 18 February 2026

## Terminal Goal

Iteratively fix CinderX JIT aarch64 LOAD_ATTR performance regression. Richards benchmark started at 0.41x (with inner classes) / 0.82x (with module-level classes).

## Iteration 1: A-lite (C++ invoke optimisation)

### Status: COMPLETE

### What was done

**Part 1 (commit 9e78b1ad):**
- Inlined MemberDescrMutator::getAttr for T_OBJECT_EX in LoadAttrCache::invoke
- Eliminated one C++ function call (PyMember_GetOne) from the hot path
- Result: slots read 0.73x → 0.85x (+12pp)

**Part 2 (commit e4aca5b8):**
- Extended to StoreAttrCache::invoke + MemberDescrMutator::setAttr
- Inlined T_OBJECT_EX store path (bypass PyMember_SetOne)
- Result: slot write 0.94x (near parity), Richards 0.89x → 0.93x (+4pp)

### Test Verification

- 37-test standalone suite: all pass
- 30/33 existing generator tests: maintained (3 known async gen failures)
- No crashes, no regressions
- Gate criteria met (gatekeeper approved)

### Key Findings

1. LOAD_ATTR regression has two components:
   - Invoke body overhead (~12%): eliminated by A-lite
   - Function call overhead (~20%): irreducible without JIT codegen changes
2. Store operations nearly at parity (0.94x) because the store operation itself (Py_XDECREF + write) is heavy relative to call overhead
3. Dict-based attrs (SplitMutator path) unchanged — separate optimisation needed

## Iteration 2: Option D (LIR inline type guard + slot load) — COMPLETE

### Status: COMPLETE

### What was done

**Commit 3c4ff942 on build-host (aarch64-jit-generators branch):**
- Modified 5 files: bytecode.cpp, builder.cpp, inline_cache.cpp, inline_cache.h, generator.cpp
- Added version-tag lookup at compile time (builder.cpp reads CPython inline cache)
- Added fast_type_ / fast_offset_ fields to LoadAttrCache (inline_cache.h/cpp)
- Emits inline LIR type-pointer check + direct slot load, bypassing BLR to invoke() on cache hit
- Falls through to existing invoke() slow path on type mismatch
- Cache invalidation via typeChanged() resets fast fields

### Results

- slot_read: 0.80x → 0.96x (+16pp)
- 42/42 LOAD_ATTR tests PASS
- 30/33 generator tests PASS (same 3 async baseline)
- Polymorphic: -5.1% (documented trade-off from guard overhead)
- No crashes

### Key Findings

1. HintType/FixedTypeProfiler is dead code — never emitted by builder. Had to use version-tag lookup instead.
2. LIR phi-node limitation blocked full multi-block output definitions — worked around with fast-path fields in cache object.
3. The remaining 4% gap (0.96x vs 1.0x) is likely memory loads for fast_type_/fast_offset_ from cache object (vs embedding as immediates).

## Cumulative Progress (Iterations 1+2)

| Benchmark | Original | After A-lite | After Option D |
|-----------|----------|-------------|----------------|
| Richards | 0.82x | 0.93x | ~0.96x+ |
| Slot read | 0.73x | 0.80x | 0.96x |
| Slot write | ~0.73x | 0.94x | 0.94x |
| Regular write | ? | 0.95x | 0.95x |

## Architecture Insights

- x86 and aarch64 LoadAttrCached handling is identical (both emit plain CALL)
- Any LOAD_ATTR fix benefits both architectures
- CinderX's GuardType + LoadField codegen pattern exists and works on aarch64 (used by kHasType)
- The interpreter's approach (version-tag guard + cached offset) is the gold standard
- V1 Richards (inner classes) 0.41x is caused by inline cache misses from type-pointer identity mismatch, not JIT recompilation. Fix requires layout-based caching (future work).

## Remaining Benchmark Gaps (after Option D)

| Benchmark | Ratio | Root Cause |
|-----------|-------|------------|
| slot_read | 0.96x | Near parity — remaining gap is cache object memory loads |
| nbody | 0.74x | Float arithmetic codegen (separate issue) |
| yield_from | 0.79x | Generator yield_from overhead (separate issue) |
| regular_read | 0.74x | SplitMutator path (separate optimisation) |
| gen_parameterised | 0.84x | Generator overhead (separate issue) |

## NEW TERMINAL GOAL

See NEW-TERMINAL-GOAL.md — all CinderX tests running, single test runner script, fix cinderx.compiler.opcode import failure.
