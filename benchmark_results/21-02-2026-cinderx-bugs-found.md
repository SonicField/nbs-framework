# Pre-Existing CinderX JIT Bugs — 21–26 February 2026

Found during speculative inlining benchmark testing on devgpu004 (aarch64, Python 3.12.12+meta).

All bugs are pre-existing in the CinderX JIT infrastructure — none are caused by our speculative inlining commit (725004da). Confirmed via systematic falsification.

---

## Bug 1: super().__init__() Dispatch Corruption (FIXED)

**Severity:** Was blocking 5/20 benchmarks (context_manager, decorator_chain, deep_class, kwargs_dispatch, nn_module_forward). Now fixed.

**Symptom:** `TypeError: __init__() should return None, not 'function'`

**Minimal reproducer:**
```python
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n):
        super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3):
        super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2):
        super().__init__(name, n, num=3); self.nb = nb

for i in range(200):
    Model("test")  # TypeError at iter 100 (before fix)
```

**Root cause (confirmed by hypergrep):** The tier1Vectorcall wrapper (commit 725004da) overwrites `func->vectorcall` for all JIT-compiled functions, including `__init__` constructors. When a JIT-compiled caller dispatches `super().__init__()` and the arg count doesn't match `co_argcount` (due to default args), the JIT prologue calls `JITRT_CallWithIncorrectArgcount`. That helper computes `JITRT_GET_REENTRY(func->vectorcall)` = `tier1Vectorcall - 12` (aarch64: 3 instructions back), which points to garbage in the preceding function's epilogue. The garbage re-entry doesn't clobber X0 (which holds `PyFunctionObject*`), so it returns the function object as the call result. CPython sees `__init__()` returned a function, not `None` → `TypeError`.

**Fix:** Commit d23c1e53 — added `getJitReentry()` helper in `jit_rt.cpp` that looks up JIT entry via `CompiledFunction` instead of computing it from `func->vectorcall`. Replaced 4 `JITRT_GET_REENTRY(func->vectorcall)` call sites with `getJitReentry(func)`.

**Verification:** All 5 blocked benchmarks pass (500 iterations each past compile threshold). Verified by claude and testkeeper independently.

---

## Bug 2: LICM GuardType Hoisting SEGFAULT (FIXED)

**Severity:** Was blocking 1/20 benchmarks (nqueens). Now fixed.

**Symptom:** `Segmentation fault (core dumped)` after LICM log message

**Minimal reproducer:**
```python
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

def nqueens(n):
    count = 0
    cols = [0] * n
    def solve(row):
        nonlocal count
        if row == n:
            count += 1
            return
        for col in range(n):
            ok = True
            for prev_row in range(row):
                if cols[prev_row] == col or abs(cols[prev_row] - col) == row - prev_row:
                    ok = False
                    break
            if ok:
                cols[row] = col
                solve(row + 1)
    solve(0)
    return count

for _ in range(3): nqueens(8)
nqueens(8)  # SEGFAULT (before fix)
```

**Root cause:** LICM was hoisting `GuardType` instructions from loop bodies to preheaders based only on explicit operands (`GetOperand(i)`) being loop-invariant. However, `GuardType` inherits from `DeoptBase` and carries a `FrameState` with `live_regs` that reference registers defined inside the loop body. When the hoisted guard fired deoptimisation in the preheader, it tried to materialise values that were not yet defined → segfault.

**Fix:** Commit f44f531d — replaced `allOperandsOutsideLoop` (which only checks `GetOperand`) with `allUsesOutsideLoop` (which uses `visitUses()` to check ALL register references including `FrameState` stack/locals and `live_regs`). Guards whose deopt metadata references loop-body-defined registers are no longer hoisted.

**Verification:** nqueens(8) = 92 (PASS, was SEGFAULT). No regression on Bug 1 benchmarks. Gatekeeper approved. Pending testkeeper full suite verification.

**Key code locations:**
- `DeoptBase::visitUses`: hir.cpp:45 — traverses explicit operands, FrameState, live_regs, guilty_reg
- `FrameState::visitUses`: frame_state.h:84 — iterates stack, localsplus, parent frames
- `licm.cpp` — the LICM pass (fixed file)

---

## Bug 3: Inliner Exception Frame Crash (FIXED)

**Severity:** Was blocking 1/20 benchmarks (exception_handling). Now fixed.

**Symptom:** `JIT: frame.cpp:163 -- Abort: couldn't find non-inlined frame`

**Root cause:** The CinderX inliner's canInline() did not check for co_exceptiontable in the callee function. When a function with try/except was inlined, the frame metadata was not set up correctly for exception handler support (ENABLE_LIGHTWEIGHT_FRAMES path).

**Fix:** Added guard in canInline() to refuse inlining functions with non-empty co_exceptiontable.
```cpp
// inliner.cpp:192-194
if (code->co_exceptiontable != nullptr &&
    PyBytes_GET_SIZE(code->co_exceptiontable) > 0) {
  return fail(InlineFailureType::kHasExceptionHandlers);
}
```

**Commit:** 23c868ac "Refuse to inline functions with exception handlers (try/except)"

**Falsification:** Direct function call inlining (no try/except) passes. Function with try/except crashes. Crash occurs even without exceptions being thrown. LEA fix revert does not fix it. Pre-existing in the base CinderX inliner.

---

## Bug 4: Tight-Loop Type Mutation Returns Function Objects (FIXED)

**Severity:** Does not block benchmarks (adversarial test only). Now fixed.

**Symptom:** After exactly N=compile_after_n_calls type mutations, d.speak() returns the lambda function object instead of calling it.

**Minimal reproducer:**
```python
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 1

def caller(d): return d.speak()

d = Dog()
for _ in range(3000): caller(d)

for i in range(200):
    Dog.speak = lambda self, v=i: v
    result = caller(d)
    if result != i:
        print(f"BREAK at i={i}: expected {i}, got {result}")
        break
# Breaks at i=100 (before fix)
```

**Root cause:** Shared with Bug 1. `JITRT_GET_REENTRY(func->vectorcall)` on a type-mutated method's `tier1Vectorcall` produces garbage dispatch — the same mechanism as Bug 1. Both fixed by `getJitReentry()` (commit d23c1e53).

**Fix:** Commit d23c1e53 (same as Bug 1).

**Verification:** testkeeper confirmed PASS at 21:03:40Z. Shared mechanism hypothesis confirmed empirically by hypergrep.

---

## Bug 5: Varint Endianness Mismatch in parseExceptionTable (FIXED)

**Severity:** Was causing SIGSEGV in auto-mode (`cinderjit.auto()`) at ~45% rate across all benchmarks. Now fixed.

**Symptom:** `Segmentation fault (core dumped)` during JIT compilation of functions with exception tables.

**Minimal reproducer:**
```python
import cinderx; cinderx.init()
import cinderjit; cinderjit.auto()

def workload():
    for i in range(1000):
        try:
            pass
        except Exception:
            pass

workload()  # SIGSEGV at ~45% rate (before fix)
```

**Root cause:** CinderX's `parse_varint` lambda in `builder.cpp:572` uses LSB-first (little-endian) decoding: `val |= (b & 0x3F) << shift`. CPython 3.12's exception table encoder uses MSB-first (big-endian) encoding. The developer likely copied from CPython's `write_varint` (which is LSB-first, used for location tables) instead of `parse_varint` (which is MSB-first, used for exception tables).

**Proof:** Tokenizer.__next entry 4, target=154 → LSB-first produces 1666 → out of bounds (157 instructions) → SIGSEGV.

**Fix:** Commit 0974344a — replaced `val |= (b & 0x3F) << shift` with `val = (val << 6) | (b & 0x3F)` to match CPython's canonical MSB-first `parse_varint`. 1 file changed (builder.cpp), 13 insertions, 15 deletions.

**Verification:** 0/80 auto-mode crashes across 4 independent stress test runs (was 9/20 = 45%). 3373 correctness tests pass. ABBA benchmark clean (exit 0, 7/7 benchmarks).

**Key code locations:**
- `builder.cpp:572` — the `parse_varint` lambda (fixed)
- CPython `compile.c:parse_varint` — the canonical MSB-first reference implementation

---

## Bug 6: Shutdown SIGSEGV in notifyTypeModified (FIXED)

**Severity:** Was causing exit -11 on all JIT benchmark worker subprocess runs. Benchmark results were valid (emitted before crash) but exit code misled orchestrator into marking runs as FAILED. Now fixed.

**Symptom:** `Segmentation fault (core dumped)` during Python interpreter shutdown (`Py_FinalizeEx`).

**Root cause:** During `Py_FinalizeEx`, the final GC (`PyGC_Collect`) calls `type_clear` on type objects, which triggers `PyType_Modified`. CinderX's type watcher (`cinderx_type_watcher`) calls `jit::Context::notifyTypeModified()` at `context.cpp:395`, which iterates `type_deopt_patchers_` and dereferences `TypeDeoptPatcher` pointers. By this point in shutdown, the patchers have been freed but not removed from the map — dangling pointer dereference → SIGSEGV.

**Call chain:**
```
main → Py_RunMain → Py_FinalizeEx → PyGC_Collect → gc_collect_main
  → delete_garbage → type_clear → PyType_Modified
  → cinderx_type_watcher → jit::Context::notifyTypeModified
  → patcher->maybePatch(new_type) → SIGSEGV
```

**Fix:** Commit b59e322c — register an `atexit` callback to clear `type_deopt_patchers_` before `Py_FinalizeEx`'s final GC. Follows existing CinderX atexit pattern (`_clear_strict_modules`). `ThreadedCompileSerialize` guard prevents TOCTOU race with `notifyTypeModified`. 3 files changed, 33 insertions:
- `context.h` (+3): declare `clearTypeDeoptPatchers()`
- `context.cpp` (+5): implement with `ThreadedCompileSerialize` guard
- `_cinderx-lib.cpp` (+25): C function, method table entry, atexit registration

**Verification:** Shutdown test EXIT_CODE=0 (was -11). Benchmark worker exits cleanly with all 24 benchmarks producing valid results. Gatekeeper reviewed and approved — LIFO atexit order correct, null-check on `getContext()` safe, clearing patchers before shutdown means no deopt on late type modifications (acceptable since interpreter is shutting down).

**Key code locations:**
- `context.cpp:395` — the crash site (`patcher->maybePatch(new_type)`)
- `context.cpp:296` — new `clearTypeDeoptPatchers()` implementation
- `_cinderx-lib.cpp:1060` — new `clear_type_deopt_patchers()` C function
- `_cinderx-lib.cpp:1451` — atexit registration
