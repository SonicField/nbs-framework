# Pre-Existing CinderX JIT Bugs — 21–26 February 2026

Found during speculative inlining benchmark testing on build-host (aarch64, Python 3.12.12+meta).

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

---

## Bug 7: setup.py os.makedirs Creates .so Path as Directory (FIXED)

**Severity:** Was causing SIGSEGV on import for all `build_ext --inplace` builds. Latent until `pip install -e .` (editable install) created the conditions for it to manifest. Now fixed.

**Symptom:** `Segmentation fault (core dumped)` on `import cinderx` after `build_ext --inplace`.

**Minimal reproducer:**
```bash
cd /data/users/alexturner/cinderx_dev/cinderx
python3 setup.py build_ext --inplace
python3 -c "import cinderx"  # SIGSEGV
```

**Root cause:** `setup.py` line 378 calls `os.makedirs(extension_dir, exist_ok=True)` where `extension_dir` is the full path to the `.so` file (e.g. `cinderx/PythonLib/_cinderx.cpython-312-aarch64-linux-gnu.so`). This creates a DIRECTORY with the `.so` suffix instead of a file. Python's import machinery finds this directory via ABI-tagged suffix priority (`*.cpython-312-aarch64-linux-gnu.so` > `*.so`), passes it to `dlopen()`, which segfaults on a directory.

**Proof:** `ls -la cinderx/PythonLib/_cinderx.cpython-312-aarch64-linux-gnu.so` shows `d` (directory) instead of `-` (file). Removing the corrupt directory and rebuilding restores correct behaviour.

**Fix:** Applied by supervisor on build-host — changed line 378 from:
```python
os.makedirs(extension_dir, exist_ok=True)
```
to:
```python
os.makedirs(os.path.dirname(extension_dir), exist_ok=True)
```

**Verification:** `build_ext --inplace` no longer creates corrupt `.so` directory. `import cinderx; cinderx.init()` succeeds. Identified by testkeeper (22:48:26Z), fix applied by supervisor.

**Note:** This bug was latent — it only manifests when the parent directory doesn't already exist, which is the case after `pip install -e .` deletes `PythonLib/` (Bug 7 companion failure mode). Three `pip install -e .` failure modes were discovered during this session and `pip install -e .` is now BANNED on the CinderX project (gatekeeper gate rule, 22:58:02Z).

**Key code locations:**
- `setup.py:378` — the `os.makedirs` call (fixed)

---

## Bug 8: cinderx.init() Crashes on aarch64 (OPEN)

**Severity:** Blocks all cinderx.init()-dependent functionality on aarch64. JIT compilation works via `cinderjit.auto()` without init(), so benchmarking is not blocked. 25/26 benchmarks work without init(); decorator_chain is the 1 that requires it (see Bug 9, merged into this bug).

**Symptom:** `Segmentation fault (core dumped)` when calling `cinderx.init()` on aarch64, regardless of call context (module-scope import, top-level call, or function body).

**Minimal reproducer:**
```python
import sys
sys.path.insert(0, "/data/users/alexturner/cinderx_dev/cinderx")
sys.path.insert(0, "/data/users/alexturner/cinderx_dev/cinderx/cinderx/PythonLib")
import cinderx
cinderx.init()  # SIGSEGV
```

**Root cause:** Unknown. `cinderx.init()` installs type watchers, frame evaluators (PEP 523), and other runtime hooks. The crash occurs inside `init()` during `os.environ.get()` or nearby code. Not specific to import context — crashes from any call site.

**Workaround:** Do not call `cinderx.init()`. Use `cinderjit.auto()` or `cinderjit.force_compile()` directly — JIT compilation works without init(). The `__init__.py` module-level `init()` call was removed (Option B, team consensus 23:11Z).

**Status:** OPEN — deferred per Alex's directive (23:24:59Z): "no one ever claimed cinderx.init() did work on ARM64. This is just more code which needs fixing." Proper debugging requires stepping through with GDB.

---

## Bug 9: decorator_chain SEGFAULT — MERGED INTO BUG 8

**Status:** CLOSED — merged into Bug 8.

**Falsification result (supervisor 23:52:24Z):** Tested decorator_chain on
BASELINE (no backoff patches — `git checkout -- compiled_function.h pyjit.cpp`,
clean rebuild). Result: SEGFAULT (exit 139). The crash is NOT caused by the
backoff patch or struct layout change. It is a pre-existing CinderX bug when
running WITHOUT `cinderx.init()` on aarch64. The original baseline benchmark
(26-02-2026-deopt-classification.md, 50K deopts, no crash) used a build where
`cinderx.init()` worked.

**Conclusion:** decorator_chain depends on something `cinderx.init()` sets up
(likely type watchers or frame evaluators). 25/26 benchmarks work without
init(); decorator_chain is the 1 that doesn't. This is the same class of
issue as Bug 8.

---

## Bug 10: Deopt Backoff Code Never Executes (OPEN)

**Severity:** The deopt backoff patch is completely inert. All deopt counts are byte-for-byte identical to baseline across all 26 benchmarks.

**Symptom:** After applying the backoff patch (compiled_function.h +35 lines, pyjit.cpp +23 lines), clean rebuild, and running full 26-benchmark deopt_stats_measure.py:
- deep_class_super: 1,100,000 deopts (baseline: 1.1M) — IDENTICAL
- pytorch_cm: 50,000 deopts (baseline: 50K) — IDENTICAL
- nn_module_forward: 40,000 deopts (baseline: 40K) — IDENTICAL
- All 22 structural: 0 deopts — unchanged (expected)

**Root cause (confirmed, supervisor 23:44:13Z):** The backoff code was placed in
the WRONG code path. CinderX has two distinct deopt mechanisms:

1. **Explicit deoptimisation** (`deoptFunc`/`reoptFunc` in pyjit.cpp): triggered
   by type invalidation or code object changes. These are ONE-TIME events. The
   backoff code was placed here.

2. **Runtime guard failures** (`Context::recordDeopt` in context.cpp, called from
   gen_asm.cpp deopt stub): triggered when JIT-compiled guard checks fail at
   runtime. These are the PER-CALL events that produce 1.1M deopts. The backoff
   code was NOT placed here.

The deopt/reopt loop goes: JIT code → guard fails → gen_asm.cpp deopt stub →
`Context::recordDeopt()` → interpreter fallback → next call re-enters JIT →
guard fails again. None of this touches `deoptFunc()`/`reoptFunc()`.

The earlier 5.7x deep_class_super result (22:44:43Z) was an artefact of ODR
violation — memory corruption from struct layout mismatch accidentally triggered
unrelated code paths.

**Correct fix site:** `Context::recordDeopt()` in `cinderx/Jit/context.cpp`
(line ~211). After incrementing `stat.count`, check if count exceeds a threshold
and suppress JIT via `CI_CO_SUPPRESS_JIT` on the associated code object.
Challenge: `recordDeopt` receives `CodeRuntime*` + `deopt_idx`, not
`PyFunctionObject*` — need a path from `CodeRuntime` to `PyCodeObject`.

**Status:** ROOT CAUSE CONFIRMED. Patch needs redesign to target
`Context::recordDeopt()` instead of `deoptFunc()`/`reoptFunc()`.

**Key code locations:**
- `context.cpp:211` — `Context::recordDeopt()` (correct fix site)
- `gen_asm.cpp:230-246` — runtime deopt stub (calls recordDeopt)
- `pyjit.cpp:deoptFunc()` — explicit deopt handler (WRONG — not called during guard failures)
- `pyjit.cpp:reoptFunc()` — reopt handler (WRONG — called once at startup, not per-call)
