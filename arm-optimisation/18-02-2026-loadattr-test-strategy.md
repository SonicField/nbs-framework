# LOAD_ATTR Codegen Test Strategy

**Date:** 18-02-2026
**Author:** @testkeeper
**Scope:** Test coverage for Approach A — inline cache fast path in `lir/generator.cpp`

## What Is Changing

The `kLoadAttrCached` case in `lir/generator.cpp` (line 1338) currently emits a single `BL` (function call) to `LoadAttrCache::invoke`. Approach A replaces this with:

1. Inline type check: `LDR Py_TYPE(obj)` → `CMP` against cached type
2. On match: inline accessor (direct `LDR` at cached offset for `__slots__`)
3. On mismatch: fall through to `BL LoadAttrCache::invokeSlowPath`

This affects **every dynamically-typed attribute load** that goes through the inline cache path. It does NOT affect:
- `LoadField` (statically-typed, exact type known at compile time)
- `LoadAttrSpecial` (dunder lookups)
- `LoadTypeAttrCacheEntryType` (type attribute lookups)
- `StoreAttrCached` (attribute stores — separate codegen)

## Risk Analysis

| Risk | Severity | Likelihood | Mitigation |
|------|----------|------------|------------|
| Type check emitted with wrong offset | Critical | Medium | Test with known ob_type offset (8 bytes on 64-bit) |
| Cache entry layout mismatch | Critical | Medium | Test cache hit AND cache miss paths |
| Slow path not taken when it should be | Critical | High | Test polymorphic types, type mutation |
| Descriptor protocol bypassed | Critical | Medium | Test data descriptors, `__get__`/`__set__` |
| Split/combined dict confusion | High | Medium | Test dict reassignment, dict→combined transition |
| MRO resolution incorrect after guard | High | Low | Test `__bases__` reassignment |
| Register clobbering in inline sequence | Critical | Medium | Test functions that use many registers around attr access |
| NULL return not handled | Critical | Medium | Test attribute that doesn't exist (AttributeError) |
| Refcount leak in fast path | High | Medium | Run under gc.collect() pressure |
| Generator interaction | Medium | Low | Test attr access inside generators |

## Test Layers

### Layer 1: Existing Test Suite (Regression Gate)

**Files:** All 16 JIT test files in `test_cinderx/`
**Baseline:** Must maintain current pass rate (30/33 generators, plus all other suites)
**Critical subset for LOAD_ATTR:**

- `test_jit_attr_cache.py` — 20 tests covering LoadAttr/StoreAttr inline cache behaviour
  - `LoadMethodCacheTests` (8 tests): type mutation, base mutation, `__bases__` reassignment, instance dict manipulation
  - `LoadAttrCacheTests` (7 tests): dict reassignment, dict mutation, split/combined dicts, descriptor mutation, type destruction
  - `StoreAttrCacheTests` (4 tests): data descriptor attachment, split↔combined dict swaps
  - `LoadModuleMethodCacheTests` (2 tests): module and strict module method loading

- `test_jit_specialization.py` — includes `test_load_attr_module` (line 205)
- `test_cinderjit.py` — 172 tests, includes LOAD_ATTR opcode test (line 558)
- `test_jit_generator_aarch64.py` — 33 tests (30 pass, 3 async fail)

**Import caveat:** `test_jit_attr_cache.py` imports from `test_compiler.test_strict.test_loader` which depends on `cinderx.compiler.opcode`. This module is NOT available in the current build. The tests must be run via a standalone script that extracts the test logic without the problematic import chain.

### Layer 2: New Standalone Correctness Tests (Fast Path Specific)

These tests target the specific codegen change — the inline type check + direct load. Each test is designed to exercise a specific failure mode of the new code.

#### 2a. Fast Path Hit Tests (type matches cache)

1. **Simple `__slots__` access** — the primary fast path target
2. **Regular instance attribute access** (dict-based)
3. **Inherited attribute access** (MRO resolution, cached type is the derived class)
4. **Multiple attributes on same object** (different offsets)
5. **Repeated access (cache warm)** — verify fast path is stable over many calls

#### 2b. Fast Path Miss Tests (type doesn't match → slow path)

6. **Polymorphic receiver** — call with type A then type B
7. **Type mutation between calls** — `Type.attr = new_value`
8. **Base class mutation** — modify base class dict, verify cache invalidation
9. **`__bases__` reassignment** — change inheritance hierarchy
10. **`__class__` reassignment** — change object's type at runtime

#### 2c. Descriptor Protocol Tests

11. **Data descriptor** (`__get__` + `__set__`) — must NOT be bypassed by fast path
12. **Non-data descriptor** (`__get__` only) — instance dict shadows it
13. **Descriptor added after cache warm** — cache must invalidate
14. **Descriptor removed after cache warm** — cache must invalidate
15. **Descriptor `__class__` changed** — data descriptor becomes non-data

#### 2d. Edge Cases

16. **AttributeError** — access non-existent attribute, verify exception raised
17. **`__getattr__` fallback** — custom attribute lookup
18. **`__getattribute__` override** — custom attribute interception
19. **None/bool/int attribute access** — builtin types with immutable layouts
20. **Inner class pattern** — fresh class each call (cache always misses)
21. **Property access** — `@property` is a data descriptor
22. **Metaclass with custom `__getattr__`**
23. **Thread safety** — concurrent attr access from multiple threads

#### 2e. Interaction Tests

24. **Attr access inside generator** — generator resumes, attr access still correct
25. **Attr access across JIT/interpreter boundary** — JIT calls interpreter function that modifies type, JIT reads attr
26. **Attr access after GC** — object survives GC, attr still accessible
27. **Attr access with `__del__`** — destructor runs during attr access? (unlikely but test)

### Layer 3: Performance Regression Tests

28. **Richards benchmark (module-level classes)** — target: ≥ 0.82x (no regression from baseline)
29. **Micro-benchmark: slot access loop** — measure raw attr access throughput
30. **Micro-benchmark: dict access loop** — same for dict-based attrs
31. **Micro-benchmark: polymorphic access** — measure cache miss overhead

### Layer 4: Crash/Safety Tests

32. **Segfault regression** — run all tests under `PYTHONFAULTHANDLER=1`
33. **Assertion checks** — build with assertions enabled, run tests
34. **Memory leak check** — run attr access loop, check RSS doesn't grow

## Test Execution Plan

### Before Changes (Baseline)

```bash
# 1. Run existing test_jit_attr_cache.py tests (standalone extraction)
CINDERJIT_ENABLE=1 python3 /tmp/test_loadattr_baseline.py

# 2. Run generator suite
CINDERJIT_ENABLE=1 python3 test_jit_generator_aarch64.py

# 3. Run micro-benchmark (3 runs, record median)
CINDERJIT_ENABLE=1 python3 /tmp/test_loadattr_perf.py
```

### After Changes

```bash
# 1. Same tests — compare pass/fail count
# 2. New fast-path tests — must all pass
# 3. Micro-benchmark — compare against baseline
```

### Gate Criteria

| Criterion | Threshold | Blocking? |
|-----------|-----------|-----------|
| Existing tests regress | Any new failure | YES |
| New tests fail | Any failure | YES |
| Crash (SIGSEGV/SIGBUS) | Any crash | YES |
| Performance regression | > 5% slowdown on any micro-benchmark | YES |
| Performance improvement | < 3% improvement on slot access | WARNING (not blocking) |

## Falsifiers

- **If the inline type check doesn't improve slot access:** The function call overhead is NOT the bottleneck. Look at the accessor dispatch (switch on cache kind) or the cache entry loop.
- **If polymorphic tests crash:** The slow path fallback has a register/stack corruption bug.
- **If descriptor tests fail:** The fast path bypasses descriptor protocol checks. The inline code must check for data descriptors before doing the direct load.
- **If type mutation tests fail:** The cache invalidation mechanism (PyType_Modified → TypeWatcher) is not correctly handled in the new codegen.
