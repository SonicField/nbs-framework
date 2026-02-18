# x86_64 Regression Testing Plan for CinderX Generator Changes

**Date:** 18 February 2026
**Author:** theologian (NBS agent)
**Context:** Pythia flagged that the GenDataFooter** slot-counting mismatch (computeSlots +1 vs _PyFrame_NumSlotsForCodeObject) may not be aarch64-specific. This plan covers how to verify x86_64 is not affected and establish a regression baseline.
**Status:** PLAN ONLY — not yet approved for execution.

## Terminal Goal

Verify that CinderX generator JIT on x86_64 is not affected by the same GenDataFooter** pointer corruption that crashes aarch64 generators with parameters or free variables.

## Falsifier

If x86_64 generators with parameters (co_argcount > 0) or free variables (co_nfreevars > 0) crash or produce incorrect results under JIT compilation, the hypothesis "this is an aarch64-only bug" is falsified.

## Phase 1: Source Verification (no build required)

**Goal:** Determine whether x86_64 and aarch64 use the same GenDataFooter** placement mechanism.

**Steps:**

1. Read `computeSlots()` in the CinderX source — identify where the +1 for GenDataFooter* is added
2. Read `_PyFrame_NumSlotsForCodeObject()` in CPython 3.12 — confirm it does NOT include the +1
3. Read the x86_64 generator codegen path in `gen_asm.cpp` — identify how GenDataFooter** is accessed on x86
4. Compare with the aarch64 path — are the offset calculations identical, or does x86 use a different mechanism?

**Expected outcomes:**
- If x86 uses the same `gen + tp_basicsize + NumSlotsForCodeObject * itemsize` calculation: the bug is latent on x86. Proceed to Phase 2.
- If x86 uses a different mechanism (e.g., a dedicated field, a different offset calculation): the bug is aarch64-specific. Document the difference and close this plan.

**Time estimate:** Source read only. No build, no SSH, no test execution.

### Phase 1 Results (completed by testkeeper, 09:32Z 18 Feb)

**VERDICT: The bug is structural, not aarch64-specific. Proceed to Phase 2.**

Evidence from testkeeper's source read:
1. `computeSlots()` (generators_mm.cpp:21-26) — NO architecture guard. The +1 for GenDataFooter* pointer is generic code.
2. `jitGenDataFooterPtr()` (gen_data_footer.cpp:10-24) — NO architecture guard. Same offset calculation on both architectures.
3. `jit_rt.cpp:745` — NO architecture guard. Footer pointer stored into localsplus slot on both architectures.
4. **The deopt guards in pyjit.cpp are ALL `#if defined(__aarch64__)` only.** x86_64 generators are JIT-compiled WITHOUT deopt guards.

This means x86_64 CinderX is currently JIT-compiling generators with the same slot-count mismatch. Either: (a) x86 generators crash and nobody noticed, or (b) x86's calling convention/codegen masks the bug (e.g., the footer pointer is never re-read after corruption, or the corruption doesn't occur due to different argument binding order).

Phase 2 testing is needed to distinguish (a) from (b).

## Phase 2: x86_64 Test Environment

**Goal:** Establish a test environment for CinderX on x86_64.

**Prerequisites:**
- Access to an x86_64 machine with Python 3.12 and CinderX source
- The same CinderX commit that is deployed on build-host (aarch64)

**Steps:**

1. Identify available x86_64 machine (devvm, devserver, or local)
2. Clone or sync the CinderX source at the same commit
3. Build CinderX: `./configure --with-pydebug && make -j$(nproc)`
4. Verify basic JIT works: `python -c "import cinderjit; cinderjit.force_compile(lambda: 1)"`

## Phase 3: Generator-Specific Regression Tests

**Goal:** Run targeted generator tests that exercise the GenDataFooter** pointer path.

**Test script (`test_generator_regression.py`):**

```python
"""
Regression tests for GenDataFooter** pointer corruption.
Tests generators with parameters, free variables, and combinations.
Each test exercises the GenDataFooter** pointer by:
1. Creating a generator with the specified characteristics
2. JIT-compiling it (force_compile or threshold=1)
3. Exercising next/send/close/throw
4. Verifying correct results
"""

import cinderjit

# Category 1: Zero-arg generators (baseline — expected to work)
def test_zero_arg_generator():
    def gen():
        yield 1
        yield 2
        yield 3
    g = gen()
    assert list(g) == [1, 2, 3]

# Category 2: Parameterised generators (crashed on aarch64)
def test_single_arg_generator():
    def gen(x):
        yield x
        yield x + 1
    g = gen(10)
    assert list(g) == [10, 11]

def test_multi_arg_generator():
    def gen(a, b, c):
        yield a + b
        yield b + c
        yield a + c
    g = gen(1, 2, 3)
    assert list(g) == [3, 5, 4]

def test_kwarg_generator():
    def gen(x, y=10):
        yield x + y
    assert list(gen(5)) == [15]
    assert list(gen(5, y=20)) == [25]

def test_varargs_generator():
    def gen(*args):
        for a in args:
            yield a * 2
    assert list(gen(1, 2, 3)) == [2, 4, 6]

# Category 3: Closure generators (crashed on aarch64 — COPY_FREE_VARS)
def test_closure_generator():
    x = 42
    def gen():
        yield x
    assert list(gen()) == [42]

def test_closure_with_args_generator():
    multiplier = 3
    def gen(base):
        yield base * multiplier
    assert list(gen(10)) == [30]

# Category 4: send/close/throw (exercise resume path)
def test_send():
    def gen(initial):
        val = yield initial
        yield val * 2
    g = gen(1)
    assert next(g) == 1
    assert g.send(5) == 10

def test_close():
    closed = []
    def gen(x):
        try:
            yield x
        finally:
            closed.append(True)
    g = gen(1)
    next(g)
    g.close()
    assert closed == [True]

def test_throw():
    def gen(x):
        try:
            yield x
        except ValueError:
            yield -1
    g = gen(1)
    assert next(g) == 1
    assert g.throw(ValueError) == -1

# Category 5: Nested/complex generators
def test_yield_from():
    def inner(x):
        yield x
        yield x + 1
    def outer(x):
        yield from inner(x)
        yield from inner(x + 10)
    assert list(outer(1)) == [1, 2, 11, 12]

def test_generator_of_generators():
    def make_gen(n):
        def gen():
            yield n
        return gen
    results = [list(make_gen(i)()) for i in range(5)]
    assert results == [[0], [1], [2], [3], [4]]

if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        try:
            t()
            print(f"  PASS: {t.__name__}")
        except Exception as e:
            print(f"  FAIL: {t.__name__}: {e}")
```

**Execution modes:**

1. **Threshold mode (production-like):**
   ```bash
   PYTHONJITCOMPILATIONTHRESHOLD=1 python test_generator_regression.py
   ```

2. **Force-compile mode (stress test):**
   ```python
   # Wrap each generator with cinderjit.force_compile before use
   ```

3. **Existing test suite:**
   ```bash
   PYTHONJITCOMPILATIONTHRESHOLD=1 python -m pytest cinderx/test_cinderx/ -k generator
   ```

## Phase 4: Comparison

**Goal:** Compare x86_64 results against aarch64 results.

| Test | aarch64 (expected) | x86_64 (to verify) |
|------|-------------------|---------------------|
| Zero-arg generators | PASS | ? |
| Parameterised generators | CRASH (nil footer) | ? |
| Closure generators | CRASH (COPY_FREE_VARS corruption) | ? |
| send/close/throw | Depends on above | ? |
| yield from | Depends on above | ? |

**If x86_64 passes where aarch64 crashes:** The bug is aarch64-specific (different codegen path) or masked (x86 uses the slot but reads it back before it's overwritten). Need to verify with GDB whether the GenDataFooter** pointer is actually correct or just happens to not be dereferenced at the crash point.

**If x86_64 also crashes:** The bug is structural (computeSlots vs _PyFrame_NumSlotsForCodeObject mismatch). Fix must be applied to both architectures.

## Dependencies

- Phase 1 can be done immediately (source read only, no machine access needed)
- Phases 2-4 require an x86_64 machine with CinderX build capability
- This plan does not block Phase 5e/5f work on aarch64

## Risks

- x86_64 may pass all tests even with the latent bug if the corrupted slot is never dereferenced on x86 (different codegen path or calling convention)
- Testing with threshold=1 and force_compile may not exercise the same paths as production workloads
- The test script above is not exhaustive — real-world generators may have additional characteristics (decorators, class methods, nested scopes) that trigger the bug
