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

## Iteration 2: Approach B (GuardType + LoadField) — NOT STARTED

### Blocked on

1. **HintType/FixedTypeProfiler is dead code**: Never emitted by the builder. Cannot "just wire up" profiled types.
2. **No version→type reverse lookup**: CPython 3.12 has no API for this.
3. **Agent context exhaustion**: Claude and theologian both at 0-2% context.

### Design Options

| Option | Description | Pros | Cons |
|--------|-------------|------|------|
| A | Scan live types for version match at compile time | Simple prototype | GC safety risk, O(n) scan |
| C | Wire up FixedTypeProfiler emission in builder | Correct architecture | Invasive, large scope |
| D | Version-tag guard (new guard kind in LIR) | Matches interpreter approach, no type lookup needed | Requires new LIR guard type |

### Awaiting Alex's decision on which option.

## Architecture Insights

- x86 and aarch64 LoadAttrCached handling is identical (both emit plain CALL)
- Any LOAD_ATTR fix benefits both architectures
- CinderX's GuardType + LoadField codegen pattern exists and works on aarch64 (used by kHasType)
- The interpreter's approach (version-tag guard + cached offset) is the gold standard

## Remaining Benchmark Gaps (after A-lite)

| Benchmark | Ratio | Root Cause |
|-----------|-------|------------|
| slot_read | 0.80x | Function call overhead (LOAD_ATTR) |
| nbody | 0.74x | Float arithmetic codegen (separate issue) |
| yield_from | 0.79x | Generator yield_from overhead (separate issue) |
| regular_read | 0.74x | SplitMutator path (separate optimisation) |
| gen_parameterised | 0.84x | Generator overhead (separate issue) |
| Richards | 0.93x | Composite of LOAD_ATTR + STORE_ATTR |
