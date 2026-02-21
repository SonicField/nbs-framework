# Bug Report: CinderX aarch64 JIT Varargs Codegen Bug

**Date:** 21-02-2026
**Severity:** Critical (blocks 5/20 benchmarks)
**Component:** CinderX JIT, aarch64 backend, vectorcall dispatch
**Classification:** ~~Pre-existing base CinderX aarch64 JIT bug~~ **RECLASSIFIED: OUR REGRESSION** (introduced by tier1Vectorcall in commit 725004da)

## Summary

The CinderX JIT on aarch64 incorrectly handles functions with `CO_VARARGS` or `CO_VARKEYWORDS` flags (i.e., functions using `*args` or `**kwargs`). After JIT compilation, calling such a function returns the **function object itself** instead of invoking it and returning the call result.

## Minimal Reproducer

```python
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(50)

def wrapper(*args, **kwargs):
    return sum(args)

# Warmup to trigger JIT compilation
for i in range(100):
    result = wrapper(1, 2, 3)
    if not isinstance(result, int):
        print(f"BUG at iteration {i}: expected int, got {type(result).__name__}: {result}")
        break
else:
    print("PASS: all 100 iterations correct")
```

**Expected:** `PASS: all 100 iterations correct`
**Actual:** `BUG at iteration 50: expected int, got function: <function wrapper at 0x...>`

## Discriminator

The bug is triggered **exclusively** by the `CO_VARARGS`/`CO_VARKEYWORDS` code flags:

| Function signature | CO_VARARGS | CO_VARKEYWORDS | Result |
|---|---|---|---|
| `def f(x, y)` | No | No | PASS |
| `def f(*args)` | Yes | No | FAIL |
| `def f(**kwargs)` | No | Yes | FAIL |
| `def f(*args, **kwargs)` | Yes | Yes | FAIL |
| `def f(x, *args)` | Yes | No | FAIL |
| `def f(x, **kwargs)` | No | Yes | FAIL |

## Evidence of Pre-existing Classification

- Fails identically with `cinderjit.disable_hir_inliner()` (inliner OFF)
- Fails identically with `cinderjit.enable_hir_inliner()` (inliner ON)
- Passes without JIT (`-X jit-disable` or no `cinderx.init()`)
- Failure occurs at exactly `compile_after_n_calls` threshold (Tier 1 compilation)
- Our speculative inlining commit (725004da) does NOT modify the vectorcall dispatch path

## Impact

Blocks 5 of 20 benchmarks in the CinderX speculative inlining benchmark suite:

1. `context_manager` — uses `@contextlib.contextmanager` (generator with `*args`)
2. `decorator_chain` — uses `functools.wraps` wrappers (`*args, **kwargs`)
3. `deep_class` — uses `super().__init__(*args)` forwarding
4. `kwargs_dispatch` — uses `**kwargs` forwarding directly
5. `nn_module_forward` — uses `Module.__call__(*args, **kwargs)`

These are the PyTorch-relevant benchmarks most likely to benefit from speculative inlining.

## Root Cause (CONFIRMED — 17:35Z)

**tier1Vectorcall breaks the JITRT_GET_REENTRY invariant.**

The speculative inlining commit (725004da) introduces `tier1Vectorcall`, a C wrapper function set as `func->vectorcall` during `finalizeFunc` (context.cpp:437). For non-varargs functions, this works because the JIT prologue checks argcount directly without calling `JITRT_CallWithKeywordArgs`. For varargs functions (CO_VARARGS/CO_VARKEYWORDS), the JIT prologue ALWAYS calls `JITRT_CallWithKeywordArgs`, which computes re-entry via `JITRT_GET_REENTRY(func->vectorcall) = func->vectorcall - 12`. Since `func->vectorcall` points to `tier1Vectorcall` (a C function) instead of the JIT vectorcall entry label, `tier1Vectorcall - 12` points to garbage code. The re-entry call jumps to garbage, which returns X0 (the function pointer) as the result.

**Confirmed independently by claude and generalist on build-host

## Fix

**Status: FIXED (Option D implemented and verified)**

**Option C (immediate fix, applied first):** In `finalizeFunc` (context.cpp:437), skip `tier1Vectorcall` for `CO_VARARGS`/`CO_VARKEYWORDS` functions and use `compiled.vectorcallEntry()` directly. This restores the `JITRT_GET_REENTRY` invariant but disables tiering for varargs functions.

**Option D (long-term fix, IMPLEMENTED):** In `JITRT_CallWithKeywordArgs` (jit_rt.cpp:240), look up the JIT vectorcall entry via `jit::getContext()->lookupFunc(func)->vectorcallEntry()` instead of using `func->vectorcall`. This makes re-entry correct regardless of whether `func->vectorcall` points to `tier1Vectorcall` or JIT code. `tier1Vectorcall` remains installed for ALL tier 1 functions (including varargs), preserving the tiering mechanism.

### Option D implementation (2 files, +14 lines)

**jit_rt.cpp** (line 240):
```cpp
// Option D: Look up the JIT vectorcall entry from CompiledFunction
// instead of using func->vectorcall, which may point to tier1Vectorcall
vectorcallfunc jit_entry = func->vectorcall;
jit::CompiledFunction* compiled = jit::getContext()->lookupFunc(func);
if (compiled != nullptr) {
  jit_entry = compiled->vectorcallEntry();
}
return JITRT_GET_REENTRY(jit_entry)(
    (PyObject*)func, arg_space.get(), new_nargsf, nullptr);
```

**context.cpp** (line 436): No code change — `tier1Vectorcall` is always used for tier 1. Added explanatory comment.

### Verification

All 6 varargs tests PASS, nqueens PASS, all 5 previously-failing module benchmarks PASS.

## Probable Root Cause (SUPERSEDED)

~~The JIT vectorcall dispatch for varargs functions (in `postalloc.cpp rewriteVectorCallFunctions` or the aarch64 `translateCall` path) returns the callable object instead of invoking it.~~

**Actual root cause:** tier1Vectorcall wrapper violates the JITRT_GET_REENTRY invariant. See above.

## Workaround

Add a guard in `canJitCompile()` (or `tryCompile()`) to refuse JIT compilation of functions with `CO_VARARGS` or `CO_VARKEYWORDS`. This lets the interpreter handle these functions correctly at the cost of no JIT speedup for varargs code.

```cpp
// In pyjit.cpp, before compilation:
PyCodeObject* code = (PyCodeObject*)PyFunction_GET_CODE(func);
if (code->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) {
    return PYJIT_RESULT_CANNOT_SPECIALIZE;
}
```

Note: this workaround prevents JIT compilation of exactly the functions the benchmarks need to speed up. It gives correctness but not performance.

## Related Bugs

1. **super().__init__() at depth 4+** — same symptom class, resolved by clean rebuild (likely stale build artefact)
2. **nqueens LICM GuardType hoisting** — separate bug, different mechanism
3. **Inlining functions with try/except** — FIXED by co_exceptiontable guard (commit 23c868ac)

## Falsification History

14 hypotheses were falsified during investigation:
1. Vectorcall transition
2. Oparg/specialisation mismatch
3. Stale build (×2)
4. Deopt corruption
5. Registration bug
6. Outdated wiki (no aarch64 backend)
7. translateLea bug
8. DCE elimination
9. Register clobber
10. co_exceptiontable guard as mechanism
11. Generator-specific
12. Closure-specific
13. Decorator-specific
14. Stale build artefact for module benchmarks

The final discriminator (CO_VARARGS/CO_VARKEYWORDS) was identified by claude through systematic testing of function signatures with and without varargs.

## Test Script

See `test_super_fix.py` in the repo root — includes varargs-specific test cases.

## Environment

- Machine: build-host (aarch64)
- Python: 3.12.12+meta
- CinderX: commit 23c868ac (aarch64-jit-generators branch)
- Repository: https://github.com/SonicField/cinderx
