# CinderX JIT Benchmark Results — cinderx-bench-6

**Commit:** `1fa46c9b` (Optimise kwargs dispatch: 3 fixes)
**Build base:** `689f2f3c` (bench-5 baseline — TypeDeoptPatcher RAII + unwatchType)
**Platform:** aarch64 (Grace CPU)
**Mode:** `--compile=auto` (production-representative)
**Methodology:** ABBA, --reps=4 (16 runs, 8 samples per condition), subprocess-isolated
**Date:** 27 Feb 2026

**Milestone:** Stage 2b target achieved. positional_dispatch improved from 0.649x to 0.98x (+33pp), exceeding the 0.85x target by 13pp. Three targeted fixes to `JITRT_CallWithKeywordArgs` and its call path, totalling ~50 lines across 4 files.

## Fix Progression

| Fix | Description | positional_dispatch | Delta |
|-----|-------------|-------------------|-------|
| Baseline (bench-5) | `689f2f3c` | 0.649x | — |
| Fix 1 | Stack-allocate kwargs argument buffer (≤8 args) | 0.720x | +7.1pp |
| Fix 2 | Eliminate getJitReentry hash map lookup | 0.810x | +9.0pp |
| Fix 3 | Identity-mapping fast path | 0.980x | +17.0pp |
| **Total** | **3 fixes, ~50 lines** | **0.980x** | **+33.1pp** |

Each fix was independently benchmarked and confirmed before proceeding to the next. --reps=4 confirmation runs were performed for Fix 2 (0.81x) and Fix 3 (0.98x).

## Fix Details

### Fix 1 — Stack-allocate kwargs argument buffer

**Problem:** `JITRT_CallWithKeywordArgs` allocated `arg_space` via `std::make_unique<PyObject*[]>(total_args)` on every call — heap allocation + deallocation per kwargs dispatch.

**Fix:** Stack-allocate a fixed buffer (`kStackArgLimit = 8`) for the common case. Fall back to heap for functions with >8 parameters. Changed `arg_space` from `unique_ptr` to raw pointer, with lifetime managed by either stack scope or a `unique_ptr` held in the same scope.

**Effect:** positional_dispatch 0.649x → 0.720x (+7.1pp). Eliminates malloc/free overhead per call.

### Fix 2 — Eliminate getJitReentry hash map lookup

**Problem:** After binding keyword arguments, `JITRT_CallWithKeywordArgs` called `getJitReentry(func)` to find the JIT entry point. This performed a hash map lookup via `jit::getContext()->lookupFunc()` — approximately 30 instructions per call.

**Fix:** Pass the `correct_args_entry` address from the JIT prologue as a 5th argument (`x4` on aarch64, `r8` on x86_64) to `JITRT_CallWithKeywordArgs`. The caller already knows the entry point; the callee no longer needs to look it up.

**Files changed:** `gen_asm.cpp` (pass address via `adr`/`lea`), `gen_asm.h` (new label parameter), `jit_rt.cpp` + `jit_rt.h` (new `vectorcallfunc self_reentry` parameter).

**Effect:** positional_dispatch 0.720x → 0.810x (+9.0pp). Confirmed at --reps=4 (16 runs).

### Fix 3 — Identity-mapping fast path

**Problem:** For calls like `f(a=0, b=0)` where kwargs are in parameter declaration order, `BindKeywordArgs` performs a linear scan, null-initialisation, flag checks, and an identity copy — producing `arg_space[i] = args[i]` for all `i`. The entire function (~15–16ns per call) is wasted work.

**Fix:** Before calling `BindKeywordArgs`, check whether all arguments are keyword-only, in parameter order, with no varargs/varkeywords/kwonly. If so, the args array is already correctly ordered — re-enter the JIT directly, bypassing `BindKeywordArgs` entirely.

**Guards (all must hold for fast path):**
1. `kwnames != nullptr`
2. `PyVectorcall_NARGS(nargsf) == 0` (all args are keyword)
3. No `CO_VARARGS` or `CO_VARKEYWORDS` flags
4. `co_kwonlyargcount == 0`
5. `PyTuple_GET_SIZE(kwnames) == co_argcount` (exact param count match)
6. Identity check: `kwnames[i] == getVarname(co, i)` for all `i` (pointer comparison, valid for interned strings)

Falls through to existing `BindKeywordArgs` path on any mismatch.

**Effect:** positional_dispatch 0.810x → 0.980x (+17.0pp). Confirmed at --reps=4 (16 runs).

**Theologian prediction error:** Predicted 0.84–0.85x based on ~2ns inner-loop estimate. Actual gain was +17pp because the fast path bypasses the *entire* `BindKeywordArgs` call path (~15–16ns: two C function calls, stack frames, null-init, flag checks, linear scan, copy, returns), not just the inner loop. Lesson: when estimating function overhead, count the total call path, not just the hot loop.

## Key Results (--reps=4)

| Benchmark | Bench-5 (0.649x baseline) | Bench-6 | Δ |
|-----------|--------------------------|---------|---|
| positional_dispatch | 0.649x | 0.980x | +0.331x |
| kwargs_dispatch | 0.650x | 0.780x | +0.130x |
| context_manager | 0.900x | 1.040x | +0.140x |
| decorator_chain | 0.770x | 0.880x | +0.110x |
| fibonacci | 2.020x | 2.030x | +0.010x |
| nqueens | 1.430x | 1.450x | +0.020x |
| richards_full | 1.690x | 1.690x | +0.000x |

### Totals

| Metric | Bench-5 | Bench-6 | Δ |
|--------|---------|---------|---|
| Total Speedup | 1.54x | 1.56x | +0.02x |
| Geomean Speedup | 0.943x | 0.983x | +0.040x |
| Benchmarks passing | 24/24 | 24/24 | — |
| Crashes | 0 | 0 | — |

**Note:** kwargs_dispatch improved from 0.65x to 0.78x (+13pp) despite the identity-mapping fast path not firing for it (kwargs_dispatch uses `**kwargs` dict, different code path). The improvement comes from Fix 1 (stack alloc) and Fix 2 (getJitReentry elimination), which benefit all kwargs calls.

## Stage 2b Falsification

| Claim | Falsifier | Target | Actual | Result |
|-------|-----------|--------|--------|--------|
| kwargs dispatch overhead is addressable | positional_dispatch speedup | ≥0.85x | 0.98x | **PASSED** |
| Fixes cause no regressions | 24-benchmark suite | All pass | All pass | **PASSED** |
| Identity-mapping fast path is safe | Fallthrough to BindKeywordArgs on mismatch | No crash | No crash | **PASSED** |
| Theologian prediction (0.84–0.85x for Fix 3) | --reps=4 measurement | 0.84–0.85x | 0.98x | **FALSIFIED** (conservative by 13pp) |
| BindKeywordArgs inner loop is ~2ns bottleneck | Full call path measurement | ~2ns | ~15–16ns | **FALSIFIED** (total path dominates) |

## Commit Details

| File | Insertions | Deletions | Description |
|------|-----------|-----------|-------------|
| `cinderx/Jit/jit_rt.cpp` | 84 | 5 | Identity-mapping fast path, stack alloc, self_reentry, RangeIterNext |
| `cinderx/Jit/jit_rt.h` | 11 | 2 | self_reentry parameter, RangeIterNext declaration |
| `cinderx/Jit/codegen/gen_asm.cpp` | 19 | 6 | Pass correct_args_entry via r8/x4 |
| `cinderx/Jit/codegen/gen_asm.h` | 7 | 3 | correct_args_entry label parameter |
| **Total** | **108** | **13** | |

**Also included:** `JITRT_RangeIterNext` — range iterator fast path that directly accesses `_PyRangeIterObject` fields instead of going through `tp_iternext`. Unrelated to kwargs dispatch; bundled in same commit.

## Verification

| Check | Agent | Result |
|-------|-------|--------|
| --reps=2 benchmark (Fix 1+2) | testkeeper | 0.81x confirmed |
| --reps=4 benchmark (Fix 1+2) | supervisor (direct pty read) | 0.81x confirmed |
| --reps=2 benchmark (Fix 1+2+3) | testkeeper | 0.97x confirmed |
| --reps=4 benchmark (Fix 1+2+3) | supervisor (direct pty read) | 0.98x confirmed |
| Richards gate (each fix) | testkeeper | 23.2ms / 180ms (pass) |
| Gatekeeper review (5/5 criteria) | gatekeeper | APPROVE |
| Code review (Fix 3 design) | testkeeper, supervisor | Edge cases addressed (argcount extraction, Ci_Py_AWAITED_CALL_MARKER, kwnames guard) |
| Commit pushed | supervisor | origin/aarch64-jit-generators |
