# Bug Report: CinderX aarch64 JIT Varargs Codegen Bug

**Date:** 21-02-2026
**Severity:** Critical (blocks 5/20 benchmarks)
**Component:** CinderX JIT, aarch64 backend, vectorcall dispatch
**Classification:** Pre-existing base CinderX aarch64 JIT bug (NOT introduced by speculative inlining)

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

## Probable Root Cause

The JIT vectorcall dispatch for varargs functions (in `postalloc.cpp rewriteVectorCallFunctions` or the aarch64 `translateCall` path) returns the callable object instead of invoking it. The x86 path works correctly for the same code patterns.

Likely locations:
- `postalloc.cpp:rewriteVectorCallFunctions` — kVectorCall argument layout for CO_VARARGS
- `autogen.cpp:translateCall` — aarch64 BLR emission for varargs vectorcall
- `gen_asm.cpp` — aarch64 calling convention for packed args tuple

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
