# Adaptive Specialisation Session — Progress Log

**Date:** 22 February 2026
**Session goal:** Implement CPython 3.12 adaptive interpreter specialisations for the CinderX JIT, starting from the FOR_ITER family and extending to LOAD_ATTR.
**Result:** Four specialisations implemented, benchmarked, squashed, and pushed. Total: 85 insertions, 15 deletions across 6 files.

---

## Specialisations Implemented

### 1. FOR_ITER_LIST (+23.4%)

**Commit:** e3f1bccd (squashed into ed0771f1)

Cloned the FOR_ITER_RANGE pattern (commit 4ff0cfe0 from previous session). Added `g_list_iterator_type` to `iterator_types.h/.cpp`, extended `specializedOpcode()`, `emitGetIter()` lookahead, and Simplify pass `InvokeIterNext` condition.

Infrastructure work: restructured `init_iterator_types()` from early-return to nested-if pattern so range init isn't blocked by list init failure.

**Benchmark methodology challenge:** Five ABBA iterations needed before getting valid results:
- v1-v3: Functions not JIT-compiled (200 warmup insufficient, `compile_after_n_calls` returns None)
- v4: `force_compile()` returns True but `is_jit_compiled()` returns False
- v5: 15000 warmup calls with full benchmark per call — timed out (3B iterations)
- v6 (final): `cinderjit.auto()` + 15000 light warmup calls (1 iteration each) + `exec()` for fresh code objects per ABBA sample

Key lesson: CinderX JIT requires `auto()` mode + ~10000+ function calls to trigger compilation. Warmup must be cheap (small iteration count) but frequent (many calls).

### 2. FOR_ITER_TUPLE (+25.7%)

**Commit:** ad023268 (squashed into ed0771f1)

Direct clone of FOR_ITER_LIST. Added `g_tuple_iterator_type`, extended all the same files. Used `PyTuple_New(0)` for type capture in `init_iterator_types()`.

Applied all changes via a single Python script (`/tmp/fix_tuple.py`) — all four edits succeeded in one shot. Build, smoke test (5/5), ABBA benchmark all passed on first attempt using the v6 methodology.

### 3. LOAD_ATTR_INSTANCE_VALUE (+57.3%)

**Commit:** f7bcd472 (squashed into ed0771f1)

The highest-impact change in 2 lines. Key insight: `LOAD_ATTR_INSTANCE_VALUE` uses the **identical** `_PyAttrCache` layout as the already-implemented `LOAD_ATTR_SLOT`. The builder code for reading the type version from the IC cache and emitting `GuardType` is exactly the same.

Implementation: added `case LOAD_ATTR_INSTANCE_VALUE:` as a fall-through to `case LOAD_ATTR_SLOT:` in `emitLoadAttr()`, plus `case LOAD_ATTR_INSTANCE_VALUE:` in `specializedOpcode()`.

The Simplify pass's existing `simplifyLoadAttrSplitDict` does all the heavy lifting: replaces generic `PyObject_GenericGetAttr` (MRO lookup → descriptor protocol → dict lookup) with direct indexed array access into the dict-or-values structure.

### 4. Squash and Push

All four specialisation commits squashed into `ed0771f1` — "Add adaptive specialisation for FOR_ITER and LOAD_ATTR families". Force-pushed to SonicField fork.

---

## Summary Table

| Specialisation | ABBA Speedup | Lines | Mechanism |
|---------------|-------------|-------|-----------|
| FOR_ITER_RANGE | +25% | (prev session) | GuardType at GET_ITER → CallStatic(JITRT_InvokeIterNext) |
| FOR_ITER_LIST | +23.4% | +40/-14 | Same as range |
| FOR_ITER_TUPLE | +25.7% | +26/-1 | Same as range |
| LOAD_ATTR_INSTANCE_VALUE | +57.3% | +3/-1 | GuardType on receiver → simplifyLoadAttrSplitDict |

**Squashed commit:** ed0771f1 (6 files, 85 ins, 15 del)

---

## Files Modified

| File | Changes |
|------|---------|
| `cinderx/Jit/iterator_types.h` | +2: `g_list_iterator_type`, `g_tuple_iterator_type` declarations |
| `cinderx/Jit/iterator_types.cpp` | +28/-8: two new globals, two init blocks, restructured error handling |
| `cinderx/Jit/bytecode.cpp` | +3: FOR_ITER_LIST, FOR_ITER_TUPLE, LOAD_ATTR_INSTANCE_VALUE in `specializedOpcode()` |
| `cinderx/Jit/hir/builder.cpp` | +13/-5: emitGetIter extended for list/tuple; LOAD_ATTR_INSTANCE_VALUE fall-through |
| `cinderx/Jit/hir/simplify.cpp` | +4/-2: InvokeIterNext condition extended for list/tuple iterator types |
| `.gitignore` | +13: (from previous session cleanup) |

---

## Architectural Insights

### Guard placement is first-order for loops

Established in the FOR_ITER_RANGE session, confirmed here: per-iteration guards turn a 25% speedup into a 1.3% slowdown. The correct pattern is guard at iterator creation (GET_ITER), not per iteration (FOR_ITER).

### The Simplify pass is the optimisation engine

All four specialisations follow the same meta-pattern: the builder emits a `GuardType` to establish type information, then the *existing* Simplify pass does the actual optimisation. The Simplify pass already had:
- `InvokeIterNext` → `CallStatic(JITRT_InvokeIterNext)` for known non-generator iterators
- `simplifyLoadAttrSplitDict` for known instance types

These were dead code — never triggered because type info was never established. Our changes are purely information-providing: we give the Simplify pass the type info it needs via `GuardType`, using CPython's adaptive interpreter feedback as the source of truth.

### LOAD_ATTR is the high-value target

FOR_ITER specialisations give 23-26% on tight loops. LOAD_ATTR_INSTANCE_VALUE gives 57% — and `self.x` is the most common Python operation. The 2-line change has more real-world impact than the three FOR_ITER changes combined.

### CinderX JIT auto-compilation requires ~10000 calls

The JIT uses `auto()` mode with a high compilation threshold. `force_compile()` claims success but doesn't patch the function's vectorcall. Valid ABBA methodology requires: `auto()` + 15000+ cheap warmup calls + `exec()` for fresh code objects per sample.

---

## Commit Chain (on devgpu004, branch aarch64-jit-generators)

```
89e86eef  Eliminate redundant LoadMethodCached after speculative inlining
f44f531d  Fix LICM GuardType hoisting segfault
68c69a14  Fix speculative inlining deopt: correct FrameState for GuardType
ed0771f1  Add adaptive specialisation for FOR_ITER and LOAD_ATTR families  ← THIS SESSION
```

**Remote:** `github.com/SonicField/cinderx`, branch `aarch64-jit-generators`
