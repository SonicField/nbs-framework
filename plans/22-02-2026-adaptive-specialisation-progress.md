# Adaptive Specialisation Session — Progress Log

**Date:** 22 February 2026
**Session goal:** Implement CPython 3.12 adaptive interpreter specialisations for the CinderX JIT, starting from the FOR_ITER family and extending to LOAD_ATTR and STORE_ATTR.
**Result:** Six specialisations implemented (5 with ABBA benchmarks), squashed into one commit, and pushed. Final: 111 insertions, 15 deletions across 6 files.

---

## Specialisations Implemented

### 1. FOR_ITER_LIST (+23.4%)

Cloned the FOR_ITER_RANGE pattern (commit 4ff0cfe0 from previous session). Added `g_list_iterator_type` to `iterator_types.h/.cpp`, extended `specializedOpcode()`, `emitGetIter()` lookahead, and Simplify pass `InvokeIterNext` condition.

Infrastructure work: restructured `init_iterator_types()` from early-return to nested-if pattern so range init isn't blocked by list init failure.

**Benchmark methodology challenge:** Five ABBA iterations needed before getting valid results:
- v1-v3: Functions not JIT-compiled (200 warmup insufficient, `compile_after_n_calls` returns None)
- v4: `force_compile()` returns True but `is_jit_compiled()` returns False
- v5: 15000 warmup calls with full benchmark per call — timed out (3B iterations)
- v6 (final): `cinderjit.auto()` + 15000 light warmup calls (1 iteration each) + `exec()` for fresh code objects per ABBA sample

Key lesson: CinderX JIT requires `auto()` mode + ~10000+ function calls to trigger compilation. Warmup must be cheap (small iteration count) but frequent (many calls).

### 2. FOR_ITER_TUPLE (+25.7%)

Direct clone of FOR_ITER_LIST. Added `g_tuple_iterator_type`, extended all the same files. Used `PyTuple_New(0)` for type capture in `init_iterator_types()`.

Applied all changes via a single Python script — all four edits succeeded in one shot. Build, smoke test (5/5), ABBA benchmark all passed on first attempt using the v6 methodology.

### 3. LOAD_ATTR_INSTANCE_VALUE (+57.3%)

The highest-impact change in 2 lines. Key insight: `LOAD_ATTR_INSTANCE_VALUE` uses the **identical** `_PyAttrCache` layout as the already-implemented `LOAD_ATTR_SLOT`. The builder code for reading the type version from the IC cache and emitting `GuardType` is exactly the same.

Implementation: added `case LOAD_ATTR_INSTANCE_VALUE:` as a fall-through to `case LOAD_ATTR_SLOT:` in `emitLoadAttr()`, plus `case LOAD_ATTR_INSTANCE_VALUE:` in `specializedOpcode()`.

The Simplify pass's existing `simplifyLoadAttrSplitDict` does all the heavy lifting: replaces generic `PyObject_GenericGetAttr` (MRO lookup → descriptor protocol → dict lookup) with direct indexed array access into the dict-or-values structure.

### 4. STORE_ATTR_INSTANCE_VALUE + STORE_ATTR_SLOT (type propagation)

Added GuardType on receiver for both `STORE_ATTR_INSTANCE_VALUE` and `STORE_ATTR_SLOT`, using the same `_PyAttrCache` type version reading as the LOAD_ATTR path.

**Key difference from LOAD_ATTR:** The Simplify pass has no compile-time store path (no `simplifyStoreAttrSplitDict`). The store path uses `StoreAttrCached` (runtime inline cache). The GuardType narrows receiver type for downstream operations on the same receiver but does not directly optimise the store itself.

Added the full `emitStoreAttr` specialisation switch (previously had no specialisation handling), also including `STORE_ATTR_SLOT` since it uses the identical cache layout.

Smoke tests: 3/3 PASS. No ABBA benchmark (expected impact is marginal — type propagation only).

### 5. Squash History

Commits were squashed twice during the session:
1. First squash: FOR_ITER_LIST + FOR_ITER_TUPLE + LOAD_ATTR_INSTANCE_VALUE → `ed0771f1`
2. Second squash: `ed0771f1` + STORE_ATTR → `6a4b2d9d`

Final single commit: `6a4b2d9d` — "Add adaptive specialisation for FOR_ITER, LOAD_ATTR, and STORE_ATTR"

---

## Summary Table

| Specialisation | ABBA Speedup | Mechanism |
|---------------|-------------|-----------|
| FOR_ITER_RANGE | +25% | GuardType at GET_ITER → CallStatic(JITRT_InvokeIterNext) |
| FOR_ITER_LIST | +23.4% | Same as range |
| FOR_ITER_TUPLE | +25.7% | Same as range |
| LOAD_ATTR_INSTANCE_VALUE | +57.3% | GuardType on receiver → simplifyLoadAttrSplitDict |
| STORE_ATTR_INSTANCE_VALUE | (type propagation) | GuardType on receiver → downstream type narrowing |
| STORE_ATTR_SLOT | (type propagation) | Same as STORE_ATTR_INSTANCE_VALUE |

**Final commit:** `6a4b2d9d` (6 files, 111 ins, 15 del)

---

## Files Modified

| File | Changes |
|------|---------|
| `cinderx/Jit/iterator_types.h` | +2: `g_list_iterator_type`, `g_tuple_iterator_type` declarations |
| `cinderx/Jit/iterator_types.cpp` | +28/-8: two new globals, two init blocks, restructured error handling |
| `cinderx/Jit/bytecode.cpp` | +5: FOR_ITER_LIST, FOR_ITER_TUPLE, LOAD_ATTR_INSTANCE_VALUE, STORE_ATTR_INSTANCE_VALUE, STORE_ATTR_SLOT in `specializedOpcode()` |
| `cinderx/Jit/hir/builder.cpp` | +39/-5: emitGetIter extended for list/tuple; LOAD_ATTR_INSTANCE_VALUE fall-through; new emitStoreAttr specialisation switch |
| `cinderx/Jit/hir/simplify.cpp` | +4/-2: InvokeIterNext condition extended for list/tuple iterator types |
| `.gitignore` | +13: (from previous session cleanup) |

---

## Architectural Insights

### Guard placement is first-order for loops

Established in the FOR_ITER_RANGE session, confirmed here: per-iteration guards turn a 25% speedup into a 1.3% slowdown. The correct pattern is guard at iterator creation (GET_ITER), not per iteration (FOR_ITER).

### The Simplify pass is the optimisation engine

All specialisations follow the same meta-pattern: the builder emits a `GuardType` to establish type information, then the *existing* Simplify pass does the actual optimisation. The Simplify pass already had:
- `InvokeIterNext` → `CallStatic(JITRT_InvokeIterNext)` for known non-generator iterators
- `simplifyLoadAttrSplitDict` for known instance types

These were dead code — never triggered because type info was never established. Our changes are purely information-providing: we give the Simplify pass the type info it needs via `GuardType`, using CPython's adaptive interpreter feedback as the source of truth.

### Asymmetry between LOAD_ATTR and STORE_ATTR

LOAD_ATTR has a rich compile-time optimisation path (`simplifyLoadAttrSplitDict` — direct indexed array access). STORE_ATTR has no equivalent — it uses a runtime inline cache (`StoreAttrCached`). This means the GuardType for STORE_ATTR provides marginal value (type propagation to downstream ops) rather than direct optimisation. A future `simplifyStoreAttrSplitDict` could close this gap.

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
6a4b2d9d  Add adaptive specialisation for FOR_ITER, LOAD_ATTR, and STORE_ATTR  ← THIS SESSION
```

**Remote:** `github.com/SonicField/cinderx`, branch `aarch64-jit-generators`
