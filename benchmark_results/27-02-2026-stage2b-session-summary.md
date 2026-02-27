# CinderX JIT Stage 2b Session Summary

**Date:** 27 February 2026
**Duration:** ~17:00Z – 20:55Z (~4 hours)
**Branch:** `aarch64-jit-generators` on build-host
**Commit:** `1fa46c9b` (single commit, 3 fixes squashed)

---

## Objective

Optimise kwargs dispatch in the CinderX JIT runtime. Primary falsifier: `positional_dispatch` must improve from 0.649x (bench-5 baseline) to ≥0.85x.

## Outcome

**Performance: TARGET EXCEEDED.** `positional_dispatch` measured 0.98x at --reps=4 (16 runs, 8 per condition). Target was 0.85x — exceeded by 13pp. Total gain: +33pp from baseline.

**Correctness: SOUND.** 24/24 benchmarks pass, zero crashes, Richards gate stable (23.2ms / 180ms). Geomean unchanged (TOTAL 1.56x).

**Scope:** 4 files changed, 108 insertions, 13 deletions. All changes in `cinderx/Jit/jit_rt.cpp`, `cinderx/Jit/jit_rt.h`, `cinderx/Jit/codegen/gen_asm.cpp`, `cinderx/Jit/codegen/gen_asm.h`.

## Fix Progression

| Fix | Description | positional_dispatch | Delta | Mechanism |
|-----|-------------|-------------------|-------|-----------|
| Baseline | bench-5 (689f2f3c) | 0.649x | — | — |
| Fix 1 | kwargs stack alloc | 0.720x | +7.1pp | Replace `make_unique<PyObject*[]>` with stack buffer for ≤8 args |
| Fix 2 | getJitReentry elimination | 0.810x | +9.0pp | Pass `correct_args_entry` from JIT prologue as 5th arg, replacing hash map lookup |
| Fix 3 | Identity-mapping fast path | 0.980x | +17.0pp | Bypass `BindKeywordArgs` entirely when kwnames order matches param order |

## Technical Details

### Fix 1 — Stack allocation (jit_rt.cpp)

`JITRT_CallWithKeywordArgs` allocated a heap buffer (`std::make_unique<PyObject*[]>(total_args)`) on every call to hold reordered arguments. For the common case (≤8 parameters), this is replaced with a stack-allocated `PyObject* stack_args[8]`, falling back to heap for larger signatures.

### Fix 2 — getJitReentry elimination (gen_asm.cpp, gen_asm.h, jit_rt.cpp, jit_rt.h)

`getJitReentry(func)` performed a hash map lookup (`jit::getContext()->lookupFunc(func)`) to find the JIT re-entry point — approximately 30 instructions per call. The JIT prologue already knows the `correct_args_entry` label address at codegen time. This is now loaded into a register (x4 on aarch64/AAPCS64, r8 on x86_64/System V) and passed as a 5th parameter to `JITRT_CallWithKeywordArgs`.

`getJitReentry` is NOT deleted — 3 other call sites remain (lines 326, 376, 446 in jit_rt.cpp).

### Fix 3 — Identity-mapping fast path (jit_rt.cpp)

When a function is called with keyword arguments that are already in parameter declaration order (the common case for `f(a=x, b=y)` calling `def f(a, b)`), `BindKeywordArgs` performs an identity copy — scanning kwnames, matching each to the corresponding parameter, and copying args to the same positions. The fast path detects this case with 5 guards:

1. `kwnames != nullptr`
2. `argcount == 0` (all args passed as kwargs)
3. No `CO_VARARGS` or `CO_VARKEYWORDS`
4. `co_kwonlyargcount == 0`
5. `PyTuple_GET_SIZE(kwnames) == co_argcount`

Then verifies identity via interned string pointer comparison (`PyTuple_GET_ITEM(kwnames, i) != jit::getVarname(co, i)`). On match, re-enters JIT directly via `self_reentry`, preserving the `Ci_Py_AWAITED_CALL_MARKER` flag.

## Prediction vs Actual

Theologian predicted Fix 3 would yield positional_dispatch 0.84–0.85x. Actual was 0.98x — conservative by 13pp. The error was in estimating only the `BindKeywordArgs` inner loop cost (~2ns) rather than the total bypassed path (~16ns including C function call overhead, flag checks, branch logic, arg_space initialisation, and return through two stack frames).

## Region Table Experiment

Tested the Grace CPU Region Table hypothesis by enabling `MultipleSectionCodeAllocator` with contiguous code pools (`PYTHONJITMULTIPLECODESECTIONS=1`).

**Result: NON-FUNCTIONAL.** The allocator causes the JIT to hang during initialisation at both 64MB and 2MB hot section sizes. Root cause likely in `createSlabs()` (mmap/mprotect sequence or huge pages setup). The Grace Region Table performance impact remains theoretical — untestable without fixing the allocator.

## Other Changes

The commit includes `JITRT_RangeIterNext` — a range iterator fast path that directly accesses `_PyRangeIterObject` fields instead of going through `tp_iternext`. This was developed in a parallel work stream and bundled into the same commit. It is functionally independent of the kwargs fixes.

## Team

Multi-agent session with 6 agents: supervisor, generalist, theologian, testkeeper, gatekeeper, scribe.

- **Theologian** designed Fix 3, performed the BindKeywordArgs analysis, and self-corrected an unsafe initial proposal (direct pointer arithmetic on `func->vectorcall`).
- **Generalist** implemented all 3 fixes on build-host via remote edit.
- **Testkeeper** ran all builds, Richards gates, and benchmark suites.
- **Gatekeeper** reviewed and approved commit 1fa46c9b (5/5 checklist items).
- **Scribe** maintained the decision log throughout.

### Operational Issues

**Chat contamination:** A coordinator-impersonation template in non-English (Sanskrit, Japanese, Navajo) spread through the chat backlog. Any agent reading the contaminated history reproduced the template. Root cause: messages from a prior "sidecar" handle at 12:29Z onwards. Fixed by archiving `live.chat` and starting fresh.

**Testkeeper context exhaustion:** Testkeeper hit the autocompact floor (2–4% context) during the benchmark cycle. She was hard-restarted (L4) twice during the session. The pty-session running the benchmarks was independent of the agent process, so benchmark runs were unaffected.

## Benchmark Data

### Fix 3 — Full Results (--reps=4, 16 runs)

```
Benchmark                 Vanilla    CinderX   Speedup      Δ%
-----------------------------------------------------------------
  context_manager         16.47ms    15.93ms     1.03x    3.3%
  decorator_chain         13.81ms    15.87ms     0.87x  -14.9% !!
  deep_class_super        73.83ms    84.17ms     0.88x  -14.0% !!
  dict_ops                 5.84ms     6.42ms     0.91x   -9.9% !!
  dunder_protocol         22.72ms    21.11ms     1.08x    7.1% **
  fibonacci             6657.69ms  3287.37ms     2.03x   50.6% **
  float_arith             12.99ms    13.23ms     0.98x   -1.9%
  func_calls               9.13ms     9.76ms     0.93x   -7.0% !!
  gen_nested               7.95ms     9.03ms     0.88x  -13.6% !!
  gen_simple               2.97ms     3.73ms     0.80x  -25.2% !!
  import_callee           12.02ms    12.04ms     1.00x   -0.2%
  int_arith                8.33ms     8.87ms     0.94x   -6.6% !!
  kwargs_dispatch         14.62ms    18.73ms     0.78x  -28.1% !!
  list_comp                2.35ms     2.90ms     0.81x  -23.6% !!
  nbody                   37.39ms    37.43ms     1.00x   -0.1%
  nn_module_forward        1.56ms     2.06ms     0.76x  -31.7% !!
  nqueens                430.24ms   306.15ms     1.41x   28.8% **
  positional_dispatch      7.92ms     8.12ms     0.98x   -2.6%
  pytorch_cm              72.17ms   104.56ms     0.69x  -44.9% !!
  richards_full           39.13ms    23.33ms     1.68x   40.4% **
  richards_slots         180.68ms   182.04ms     0.99x   -0.8%
  spectral_norm         2542.94ms  2350.89ms     1.08x    7.6% **
  store_subscr             9.18ms     9.48ms     0.97x   -3.3%
  try_except_callee        4.24ms     4.70ms     0.90x  -10.8% !!
-----------------------------------------------------------------
  TOTAL                10186.17ms  6537.93ms     1.56x   35.8%
```

## Open Items

1. **kwargs_dispatch (0.78x):** Uses `CO_VARKEYWORDS` (`**kwargs`), bottleneck is dict creation + `.get()` lookups. Different architectural approach needed.
2. **MultipleSectionCodeAllocator:** Broken, needs separate engineering work to fix `createSlabs()`.
3. **d0bfbcc0 verification:** Three-tier deopt re-JIT counter — existence on build-host fork unverified.
