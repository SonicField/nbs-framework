# Plan: FOR_ITER_RANGE JIT Specialisation

**Author:** claude (implementer), with contributions from all agents
**Date:** 23 February 2026
**Status:** COMPLETE — committed as 4ff0cfe0 on build-host (branch aarch64-jit-generators)

---

## Goal

Add specialised HIR generation for `FOR_ITER_RANGE` in the CinderX JIT builder, enabling the existing Simplify pass optimisation for range iterators to fire reliably.

## Falsifiable Success Criterion

A tight `for i in range(n)` loop must execute measurably faster (>2% in ABBA methodology) with the specialisation enabled vs disabled.

**Falsifier:** If the ABBA benchmark shows <2% difference, the specialisation is not impactful and should be reconsidered.

---

## Background: What Already Exists

The Simplify pass (simplify.cpp:2100) already has a two-part range iterator optimisation:

1. **GetIter simplification** (line 2092): If input type ≤ `PyRange_Type`, narrows output to `range_iterator_type` via `UseType`.
2. **InvokeIterNext simplification** (line 2100): If iterator type == `range_iterator_type`, replaces generic `InvokeIterNext` with direct `CallStatic` to `JITRT_InvokeIterNext`, which skips the JitGen check.

**Problem:** These optimisations only fire when the iterator type is statically known. In practice, `emitForIter()` (builder.cpp:4376) emits a generic `InvokeIterNext` without any type information. The iterator register has type `TObject` (unknown), so the Simplify pass cannot prove it's a range_iterator.

**Solution:** When the bytecode is `FOR_ITER_RANGE` (i.e., CPython's adaptive interpreter has already observed that this loop iterates over a range), emit a `GuardType` on the iterator register to assert it's a `range_iterator`. This gives the Simplify pass the type information it needs to fire.

---

## Implementation Plan

### Step 1: Read the specialised opcode in emitForIter()

**File:** `cinderx/Jit/hir/builder.cpp`
**Function:** `HIRBuilder::emitForIter()`

Currently (line 4376):
```cpp
void HIRBuilder::emitForIter(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* iterator = ...;  // top of stack
  Register* next_val = temps_.AllocateStack();
  tc.emit<InvokeIterNext>(next_val, iterator, tc.frame);
  // ...
}
```

Add a `specializedOpcode()` check before the `InvokeIterNext` emission:
```cpp
void HIRBuilder::emitForIter(
    TranslationContext& tc,
    const jit::BytecodeInstruction& bc_instr) {
  Register* iterator = ...;  // top of stack

  // Specialise based on CPython's adaptive interpreter feedback
  if (getConfig().specialized_opcodes) {
    switch (bc_instr.specializedOpcode()) {
      case FOR_ITER_RANGE: {
        if (jit::g_range_iterator_type != nullptr) {
          Type range_iter_type =
              Type::fromTypeExact(jit::g_range_iterator_type);
          tc.emit<GuardType>(iterator, range_iter_type, iterator, tc.frame);
        }
        break;
      }
      default:
        break;
    }
  }

  Register* next_val = temps_.AllocateStack();
  tc.emit<InvokeIterNext>(next_val, iterator, tc.frame);
  // ... rest unchanged
}
```

### Step 2: Add `#include` for iterator_types.h

**File:** `cinderx/Jit/hir/builder.cpp`

Add `#include "cinderx/Jit/iterator_types.h"` if not already present (it may be transitively included).

### Step 3: Add FOR_ITER_RANGE to specializedOpcode()

**File:** `cinderx/Jit/bytecode.cpp`
**Function:** `BytecodeInstruction::specializedOpcode()` (line 73)

**VERIFIED:** `FOR_ITER_RANGE` is NOT in the current `specializedOpcode()` switch. The `default` case calls `unspecialize()`, which maps it back to generic `FOR_ITER`. Must add it:

```cpp
// In the switch statement at bytecode.cpp:78-96
    case STORE_SUBSCR_DICT:
    case UNPACK_SEQUENCE_LIST:
    case UNPACK_SEQUENCE_TUPLE:
    case UNPACK_SEQUENCE_TWO_TUPLE:
    case FOR_ITER_RANGE:          // ← ADD THIS LINE
      return opcode;
```

Without this change, `emitForIter()` will never see `FOR_ITER_RANGE` — it will always get `FOR_ITER`.

### Step 4: No changes needed to Simplify pass

The existing Simplify pass (simplify.cpp:2092-2115) will automatically fire because:
- The `GuardType` narrows the iterator register's type to `range_iterator_type`
- The `GetIter` UseType case is redundant (the guard provides the type directly)
- The `InvokeIterNext` case matches `iter_type == g_range_iterator_type` and emits the fast `CallStatic`

### Step 5: No changes needed to jit_rt.cpp

`JITRT_InvokeIterNext` already exists and handles the fast path. No new runtime helpers needed.

---

## What This Achieves

**Without specialisation:**
```
GET_ITER → iterator (type: TObject)
FOR_ITER → InvokeIterNext(iterator) → generic tp_iternext dispatch
```

**With specialisation:**
```
GET_ITER → iterator (type: TObject)
FOR_ITER_RANGE → GuardType(iterator, range_iterator_type) → iterator (type: range_iterator)
  → InvokeIterNext(iterator)
  → [Simplify] CallStatic(JITRT_InvokeIterNext, iterator) → skips JitGen check
```

The `GuardType` is a one-time check at the start of the loop. If it fails (iterator is not a range_iterator), we deopt to the interpreter. If it passes, every subsequent iteration benefits from the direct `JITRT_InvokeIterNext` call instead of the generic `tp_iternext` dispatch.

---

## Deoptimisation Safety

- **GuardType failure:** Falls back to interpreter at the `FOR_ITER` instruction. Correct because the FrameState from `tc.frame` points to the current bytecode offset.
- **No type_deopt_patcher needed:** `range_iterator` is a built-in type — it cannot be monkey-patched. Unlike user-defined classes, its `tp_iternext` slot is immutable.
- **No IC interaction:** `FOR_ITER_RANGE` doesn't use a LoadMethodCache or LoadAttrCache. The specialised opcode itself is the type feedback.

---

## Testing

### Pre-implementation test (falsification)

Before coding, verify that the existing Simplify optimisation does NOT fire for generic `for i in range(n)` loops:
```bash
# On build-host compile a range-loop function with JIT debug logging
CINDERJIT_LOG_INLINER=1 python3 -c "
import cinderx; cinderx.init()
import cinderjit
def f():
    s = 0
    for i in range(1000):
        s += i
    return s
for _ in range(200): f()
print(f())
"
```
Check HIR dump — `InvokeIterNext` should appear (not `CallStatic`).

### Post-implementation test

Same test — `CallStatic(JITRT_InvokeIterNext)` should appear in the HIR dump instead of generic `InvokeIterNext`.

### Benchmark

ABBA benchmark with tight range loop (helper is drafting this).

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| GuardType FrameState incorrect | Low | Same pattern as LOAD_ATTR_SLOT handler — uses `tc.frame` which is correct at FOR_ITER offset |
| `g_range_iterator_type` is null | Low | Guarded by null check. Falls through to generic path. |
| FOR_ITER_RANGE opcode not in bytecode.cpp | **CONFIRMED** | Must add it to `specializedOpcode()` switch — 1 line. Without this, the builder never sees the specialised opcode. |
| Benchmark shows <2% improvement | Medium | The optimisation saves ~5-10 instructions per iteration (skip JitGen check, skip type lookup). In a tight loop this should be measurable. If not, the loop body dominates and we need a purer micro-benchmark. |

---

## Files Modified

| File | Change |
|------|--------|
| `cinderx/Jit/bytecode.cpp` | Add `FOR_ITER_RANGE` to `specializedOpcode()` switch (1 line) |
| `cinderx/Jit/hir/builder.cpp` | Add `FOR_ITER_RANGE` case in `emitForIter()` (~10 lines) |
| `cinderx/Jit/hir/builder.cpp` | Add `#include "cinderx/Jit/iterator_types.h"` (if needed) |

**Total estimated change:** ~11-16 lines of new code.

---

## Final Implementation (differs from original plan)

### Key Insight: Guard Placement

The original plan placed `GuardType` inside `emitForIter()` — running the type check on **every loop iteration**. ABBA benchmarking revealed this was 1.3% **slower** due to per-iteration guard overhead exceeding the `JITRT_InvokeIterNext` savings.

**Three-variant analysis (ABBA results):**

| Variant | Placement | Speedup | Safety |
|---------|-----------|---------|--------|
| GuardType in FOR_ITER | Per iteration (1000x for range(1000)) | **-1.3%** (slower) | Safe |
| UseType in FOR_ITER | Zero cost (compile-time assertion) | **+23%** | Unsound for polymorphic sites |
| GuardType in GET_ITER | Once before loop | **+25%** | Safe |

The winning approach: emit `GuardType` in `emitGetIter()` (runs **once** at iterator creation), not `emitForIter()` (runs every iteration). Uses bytecode lookahead to peek at the next instruction — if it's `FOR_ITER_RANGE`, emits the guard.

### Actual Files Modified

| File | Change |
|------|--------|
| `cinderx/Jit/bytecode.cpp` | Add `FOR_ITER_RANGE` to `specializedOpcode()` switch (+1 line) |
| `cinderx/Jit/hir/builder.h` | `emitGetIter()` signature: add `bc_instr` parameter (+1/-1 line) |
| `cinderx/Jit/hir/builder.cpp` | `emitGetIter()` lookahead + GuardType + include (+18 lines) |
| `cinderx/Jit/hir/simplify.cpp` | Fix latent bug: `TOptObject` → `TObject` in `CallStatic` return type (+1/-1 line) |

**Total: 4 files, 21 insertions, 4 deletions.**

### Latent Bug Found and Fixed

The Simplify pass (simplify.cpp:2113) created `CallStatic(JITRT_InvokeIterNext)` with return type `TOptObject`, but `InvokeIterNext` declares output type `TObject`. The assertion at simplify.cpp:2171 requires `new_output->type() <= instr.output()->type()`. Since `TOptObject > TObject` (wider — includes nullptr), the assertion fired.

This was **dead code** — never triggered before our change because the range_iterator type was never established in the HIR. Our `GuardType` was the first thing to ever trigger this code path.

**Fix:** `TOptObject` → `TObject`. Correct because `CondBranchIterNotDone` handles the NULL/StopIteration sentinel downstream, not the type system.

### Commit

- **Hash:** 4ff0cfe0
- **Branch:** aarch64-jit-generators
- **Host:** build-host
- **Status:** Local commit, not pushed (full test suite must run before push)
- **Gatekeeper:** APPROVED
- **Testkeeper:** 10/11 criteria PASS (1 partial: full CPython test suite deferred to pre-push)

