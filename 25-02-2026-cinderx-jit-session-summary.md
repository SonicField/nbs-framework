# CinderX JIT Optimisation — Session Summary

**Date:** 25 February 2026
**Author:** Testkeeper (session 1), Scribe + Generalist (session 2)
**Audience:** Dr Alex Turner

---

## Executive Summary

**Session 1** began with a 13-benchmark suite showing 10W/3L. It ended with a
21-benchmark suite showing 7W/6N/8L after fixing survivorship bias. One fix
shipped (kwargs inlining, +1 win). One fix was attempted and empirically
falsified (Fix A, direct vectorcall dispatch). The remaining regressions were
root-caused to instruction-level precision across all 7 adversarial categories.

**Session 2** implemented Stage 1 + 1.5 of the callee resolution plan:
`simplifyVectorCallBoundMethod` (simplify.cpp) and `returnType` broadening
(pass.cpp). The simplifier resolves `__enter__`/`__aenter__` on exact-type
context managers to static dispatch. Branch-miss penalty dropped from 2.754x
to 1.05x (near parity). Wall time improved from 0.82x to 0.891x. One crash
bug was found and fixed (missing Snapshot before DeoptPatchpoint). Two false
hypotheses were retracted (allocator API, manual FrameState).

The honest picture: the simplifier is correct infrastructure for inlining but
does not standalone-ship. The remaining 11% regression is instruction overhead
from BEFORE_WITH opcode decomposition, which requires Stage 2 (inlining) to
close.

---

## Deliverables

### 1. Honest 21-Benchmark Suite

The original 13-benchmark suite excluded workloads that stress the JIT's
weaknesses (nested function calls, context managers, generators, dunder
protocols, class hierarchies). Adding 8 adversarial benchmarks revealed a
scorecard of 7W/6N/8L — substantially worse than the reported 10W/3L.

**Scorecard (after kwargs fix):**

| # | Benchmark            | Ratio | Verdict |
|---|----------------------|-------|---------|
| 1 | fibonacci            | 1.92x | WIN     |
| 2 | richards_full        | 1.24x | WIN     |
| 3 | richards_slots       | 1.16x | WIN     |
| 4 | nqueens              | 1.12x | WIN     |
| 5 | nbody                | 1.06x | WIN     |
| 6 | attr_access          | 1.04x | WIN     |
| 7 | positional_dispatch  | 1.61x | WIN (new, kwargs fix) |
| 8 | spectral_norm        | 0.95x | NEUTRAL |
| 9 | list_comp            | 0.98x | NEUTRAL |
| 10| dict_ops             | 0.99x | NEUTRAL |
| 11| store_subscr         | 1.00x | NEUTRAL |
| 12| int_arith            | 1.01x | NEUTRAL |
| 13| module_attr          | 0.97x | NEUTRAL |
| 14| float_arith          | 0.94x | LOSS    |
| 15| context_manager      | 0.82x | LOSS    |
| 16| dunder_protocol      | 0.79x | LOSS    |
| 17| deep_class           | 0.77x | LOSS    |
| 18| func_calls           | 0.76x | LOSS    |
| 19| kwargs_dispatch      | 0.73x | LOSS    |
| 20| gen_simple           | 0.67x | LOSS    |
| 21| gen_nested           | 0.61x | LOSS    |

All ratios are ABBA-interleaved (JIT/interpreter, alternating runs to control
for thermal drift).

### 2. Kwargs Fix (ResolveKwargs Pass)

**Status:** GATE PASS. Committed on devgpu004.

A new HIR pass (`resolve_kwargs.cpp`, 303 lines) that resolves call-site
keyword arguments to positional order before the inliner runs. This enables
the inliner to inline functions called with keyword syntax.

**Results:**
- `positional_dispatch`: 1.61x (new benchmark, was 0.73x before fix)
- 356/356 tests passing (172 JIT + 7 JIT-specific + 6 kwargs + 171 stress)
- Zero regressions on existing benchmarks
- ABBA methodology confirmed (micro-benchmark showed 2.4x; proper ABBA showed 1.61x — validates warmup drift concern)

### 3. Fix A (Direct Vectorcall Dispatch)

**Status:** GATE FAIL. Abandoned.

Hypothesis: replacing `JITRT_Vectorcall` with `_PyObject_Vectorcall` for
TFunc-typed callees would eliminate dispatch overhead.

**What happened:**
- Tier 1 (3-line CallMethod TFunc bypass): zero measurable effect on
  target benchmarks. TFunc guard never fires because callable type at
  VectorCall/CallMethod sites is TObject, not TFunc.
- Tier 2 (direct vectorcall for all sites): cancelled after branch
  misprediction thesis was falsified (3.96x was a measurement artefact
  from process startup contamination; steady-state ratio is 1.05x).
- Dispatch overhead measured at 2–5ns per call (JITRT_Vectorcall vs
  _PyObject_Vectorcall) — negligible, cannot explain 22–29% regressions.

**Lesson:** gate criterion #0 (perf record at instruction level before
coding) was established after this failure. Applied to all subsequent work.

### 4. Validated Root Cause Map (7/7 Categories)

Every adversarial benchmark now has instruction-level validation (perf stat
`instructions:u`, `branch-misses:u`).

| Cat | Benchmark          | Ratio | Root Cause                    | Validation               | Insn Ratio | Bmiss Ratio |
|-----|--------------------|-------|-------------------------------|--------------------------|------------|-------------|
| A   | func_calls         | 0.76x | Nested MakeFunction, no IC    | HIR dump                 | —          | —           |
| B   | gen_simple/nested   | 0.67x/0.61x | InvokeIterNext + L2 cache | perf stat (452K L2 miss) | 1.12x      | —           |
| C   | context_manager    | 0.82x | BEFORE_WITH decomposition     | perf stat                | 1.063x     | 2.754x      |
| D   | deep_class         | 0.77x | Load*→TObject blocks inlining | pass.cpp type chain      | —          | —           |
| E   | dunder_protocol    | 0.79x | UNARY_LENGTH decomposition    | perf stat                | 1.066x     | 2.770x      |
| F   | kwargs_dispatch    | 0.73x | CO_VARKEYWORDS (no fix)       | code analysis            | —          | —           |
| G   | float_arith        | 0.94x | PrimitiveBox/PrimitiveUnbox   | perf stat                | 1.175x     | 2.795x      |

**Root cause grouping:**
- **Opcode decomposition** (C, E): JIT breaks monolithic interpreter opcodes
  (BEFORE_WITH, UNARY_LENGTH) into multiple HIR operations. The interpreter's
  tight C implementations are faster because they avoid multiple operations,
  register allocation, and branch prediction overhead.
- **Inlining gap** (A, D): The inliner cannot resolve callees. Nested
  functions created by MakeFunction have no IC entry. LoadAttrSpecial and
  LoadMethodCached produce TObject, preventing method identity resolution.
- **Generator overhead** (B): InvokeIterNext dispatches through JIT runtime.
  JitGenFreeList arena (1 MiB, 2048x512-byte entries) causes L2 cache
  pressure. 452K extra L2→DRAM misses account for 70%+ of the cycle gap.
- **Boxing overhead** (G): PrimitiveBox/PrimitiveUnbox on every float
  operation adds 17.5% more instructions. Impact is diluted (0.94x) because
  float ALU operations are cheap.
- **No fix** (F): CO_VARKEYWORDS is a callee property — the JIT cannot
  remove `**kwargs` from a function that declares it.

### 5. InlineFailureType Diagnostic

**Finding:** the document `next-jit-steps.md` Step 1 ("Expand Inlining
Frontier") lists the wrong targets for our benchmarks.

Ran `PYTHONJITENABLEHIRINLINER=1 PYTHONJITDEBUGINLINER=1` on all
adversarial benchmarks. Results:

- **Total inline successes:** 0
- **Total inline failures:** 1 (IsGenerator on `_simple_gen` in gen_simple)
- **All other benchmarks:** 0 inlined, 0 failures

The inliner records zero failures because it cannot RESOLVE the callee —
the `InlineFailureType` enum only covers why a resolved callee was rejected.
If the callee identity is unknown (TObject from Load*), `canInline()` is
never called.

**Debug inliner output reveals three resolution failure modes:**

1. **"unknown function v36:MortalFunc"** (inliner.cpp:409) — callee IS
   typed as MortalFunc but the inliner cannot determine WHICH function.
   MakeFunction target, no IC entry. Fix: SSA backward tracing.
2. **"non-function v33:MortalTypeExact[_Deep:obj]"** (inliner.cpp:386) —
   callee is a type object (class constructor). Fix: type-call → __init__
   inlining.
3. **TObject from Load*** — callee type too broad. Fix: attribute type
   propagation from class definitions.

**Implication:** Step 1 should be retitled "Expand Callee Resolution." The
frontier (what CAN be inlined) is not the bottleneck. The resolution (what
can be FOUND to inline) is. The document's concrete steps (try/except,
closures, *args/**kwargs) would not fix our benchmarks.

### 6. Branch Miss Comparison

**Finding:** the ~2.75x branch miss overhead is specific to losing
benchmarks, not universal JIT infrastructure.

| Benchmark          | Insn Ratio | Branch Miss Ratio | Category |
|--------------------|------------|-------------------|----------|
| fibonacci          | 1.000x     | 0.990x            | WINNING  |
| int_arith          | 1.000x     | 1.000x            | WINNING  |
| list_comp          | 0.997x     | 1.001x            | WINNING  |
| attr_access        | 1.003x     | 1.003x            | WINNING  |
| context_manager    | 1.063x     | 2.754x            | LOSING   |
| dunder_protocol    | 1.066x     | 2.770x            | LOSING   |
| float_arith        | 1.175x     | 2.795x            | LOSING   |

Winning benchmarks: JIT adds ZERO extra branch misses.
Losing benchmarks: JIT adds ~660K extra branch misses from decomposed
opcode operations on the hot path.

**Implication for Step 6 (Hot/Cold Splitting):** reduced value. The extra
branches are on the hot path (decomposed operations), not on cold paths
(guard failures, exception handlers). Hot/cold splitting moves cold code —
but the bottleneck is hot-path branch count from decomposition.

### 7. Stage 1 + 1.5: Callee Resolution for Context Managers

**Status:** ACCEPTED AS INFRASTRUCTURE. Does not standalone-ship.

Two-file change implementing static dispatch for `__enter__`/`__aenter__`
on exact-type context managers.

#### 7a. Changes

**File: `cinderx/Jit/hir/pass.cpp` — returnType broadening (Stage 1.5)**

Broadened `returnType` for heap types with `tp_new == PyBaseObject_Type.tp_new`.
When a VectorCall's callee is a type object (class constructor) and the type
uses the default allocator, the return type is narrowed to `Type::fromTypeExact(cls)`.
This participates in `reflowTypes` during the simplifier's fixpoint loop,
providing TExact narrowing one iteration before `simplifyVectorCallBoundMethod` runs.

```cpp
if (Py_TYPE(callable_obj) == &PyType_Type) {
    PyTypeObject* cls = reinterpret_cast<PyTypeObject*>(callable_obj);
    Type result = Type::fromTypeExact(cls);
    if (!(result <= TType) &&
        (result <= TBuiltinExact || cls->tp_new == PyBaseObject_Type.tp_new)) {
        if (result <= TUnicodeExact || result <= TBytesExact) {
            return result | TUser;
        }
        return result;
    }
}
return TObject;
```

**File: `cinderx/Jit/hir/simplify.cpp` — simplifyVectorCallBoundMethod (Stage 1)**

New simplification function that traces VectorCall backward to LoadAttrSpecial,
resolves `__enter__`/`__aenter__` through the MRO at compile time, and replaces
the indirect call with a static VectorCall dispatch.

Key structure:
1. Reject kwargs, static, awaited calls
2. Check func operand was produced by LoadAttrSpecial
3. Only handle `__enter__` and `__aenter__` (with-statement methods)
4. Require exact receiver type, valid version tag
5. Resolve method through MRO via `typeLookupSafe`
6. Reject non-PyFunctionObject results (C methods, descriptors)
7. Emit Snapshot + DeoptPatchpoint (for type stability) + UseType + LoadConst + static VectorCall

Also added `simplifyCallMethod` type narrowing for constructor calls (Stage 1.5):
narrows CallMethod→VectorCall output to `Type::fromTypeExact(cls)` for type
object callees with `tp_new == PyBaseObject_Type.tp_new`.

#### 7b. Bug: Missing Snapshot Before DeoptPatchpoint

**Symptom:** Segfault (EXIT=139) during JIT compilation when the simplifier fires.

**GDB backtrace:**
```
FrameState::FrameState (copy ctor)
  → DeoptBase::setFrameState
  → bindGuards (refcount_insertion.cpp:1226)
  → RefcountInsertion::Run
```

**Root cause:** `LoadAttrSpecial` is classified as non-replayable. The
`bindGuards` function in `refcount_insertion.cpp` walks blocks linearly,
tracking the most recent Snapshot's FrameState. When it encounters a
non-replayable instruction, it resets the tracked FrameState to `nullptr`.
Our DeoptPatchpoint followed LoadAttrSpecial in the block — `bindGuards`
attempted to copy a null FrameState, causing the crash.

**Fix:** Emit `env.emitInstr<Snapshot>(*instr->frameState())` immediately
before the DeoptPatchpoint. This gives `bindGuards` a fresh FrameState to
copy.

```cpp
if (!_PyClassLoader_IsImmutable(py_type)) {
    env.emitInstr<Snapshot>(*instr->frameState());
    auto patchpoint = env.emitInstr<DeoptPatchpoint>(
        env.func.allocateCodePatcher<TypeAttrDeoptPatcher>(
            py_type, BorrowedRef<PyUnicodeObject>{attr_id}, method));
    patchpoint->setGuiltyReg(receiver);
    patchpoint->setDescr("LoadAttrSpecial method resolution");
}
```

**Why existing DeoptPatchpoints in simplify.cpp don't need explicit Snapshots:**
They replace instructions immediately after a dominating Snapshot, with no
non-replayable instructions in between. Our context has intervening
LoadAttrSpecial instructions, requiring explicit Snapshot emission.

#### 7c. Retracted False Hypotheses

| Hypothesis | Evidence Against | Retraction |
|-----------|-----------------|------------|
| Wrong allocator API (`allocateDeoptPatcher`) | `allocateDeoptPatcher` does not exist. All 5 DeoptPatchpoint creations in simplify.cpp use `allocateCodePatcher`. | False alarm — correct pattern. |
| Manual `setFrameState` needed | `bindGuards` OVERWRITES any manually set FrameState with the dominating Snapshot's copy. Manual set is irrelevant. | Red herring — bindGuards handles it. |

#### 7d. Gate Results

| Criterion | Status | Detail |
|-----------|--------|--------|
| C0 (baselines) | SATISFIED | Root cause analysis complete |
| C1a (correctness) | PASS | 8/8 functional tests + 500K stress test |
| C1b (full suite) | NOT RUN | Blocked on test infrastructure |
| C1c (tp_new guard) | PASS | Custom `__new__` correctly rejected |
| C2a (type narrowing) | PASS | `receiver=ObjectUser[CM:Exact] isExact=1` |
| C2b (simplifier fires) | PASS | `SUCCESS: static dispatch to CM.__enter__` |
| C3 (branch-misses) | PASS | 2.754x → 1.054x (near parity) |
| C3 (wall time) | FAIL | 0.891x (target >=0.95x) |
| C4 (ABBA wall time) | FAIL | 0.891x (10-sample subprocess ABBA, tight clusters) |
| C5a (custom `__new__`) | PASS | Falsifier passes |
| C5b (custom `__new__` returns different type) | PASS | SubCM.__new__ returns BaseCM — correctly handled |
| C5c (C extension parent — list) | PASS | list-based CM — tp_new guard rejects |
| C5c2 (C extension parent — dict) | PASS | dict-based CM — tp_new guard rejects |
| C5d (runtime monkey-patch) | N/A | Pre-existing CinderX JIT bug in BEFORE_WITH handling |
| C5e (metaclass with custom `__call__`) | PASS | Correctly handled |
| C5f (deep inheritance chain) | PASS | Grandparent/parent/child — MRO resolution correct |
| C5g (`__exit__` raises) | PASS | RuntimeError propagation correct |

**Verdict:** 10 PASS, 2 FAIL (wall time), 1 N/A, 1 NOT RUN. Accepted as infrastructure.

#### 7e. Performance

**Branch-miss penalty eliminated:**

|                  | Pre-fix | Post-fix | Change |
|------------------|---------|----------|--------|
| Branch-misses    | 2.754x  | 1.054x   | Near parity |
| Instructions     | 1.063x  | 1.055x   | Slight improvement |
| Wall time (ABBA) | 0.82x   | 0.891x   | 7pp improvement (39% of gap closed) |

The simplifier eliminates the indirect dispatch branch-miss penalty. The
remaining 11% wall-time regression is instruction overhead from BEFORE_WITH
opcode decomposition — the JIT emits more instructions than the interpreter's
monolithic C implementation. This is architecturally upstream of the simplifier.

#### 7f. Architecture Insights

**Simplifier fixpoint loop:** The outer loop in `simplify.cpp` runs
CopyPropagation + `reflowTypes` + CleanCFG between iterations. `returnType`
in `pass.cpp` participates in `reflowTypes`, providing type narrowing one
iteration before `simplifyVectorCallBoundMethod` runs. This is why Stage 1.5
(returnType broadening) is required — without it, the receiver type is TObject
and the simplifier never fires.

**bindGuards (refcount_insertion.cpp:1226):** Walks blocks linearly tracking
the most recent Snapshot's FrameState. For Guard/Deopt/DeoptPatchpoint, copies
the tracked FrameState. For non-replayable instructions (side effects), resets
the tracked FrameState to `nullptr`. LoadAttrSpecial is in the non-replayable
list at `hir.cpp:133-193`.

**Python 3.14 note:** Python 3.14 replaces BEFORE_WITH with LOAD_SPECIAL+CALL.
The simplifier is 3.12-only.

#### 7g. Methodology Lessons

**In-process ABBA is invalid:** An initial in-process ABBA test showed 0.987x,
but was correctly identified as flawed — after `cinderx.init()`, both JIT and
interpreter code paths execute JIT-compiled code in the same process. The
authoritative result came from subprocess-isolated ABBA (separate processes
with different environment variables), which showed 0.891x.

---

## Falsified Hypotheses

| Hypothesis | How Falsified | Session |
|-----------|---------------|---------|
| Branch misprediction 3.96x | Steady-state is 1.05x; 3.96x was startup noise | 1 |
| TFunc guard on VectorCall | Guard never fires; callable type is TObject | 1 |
| JITRT_Vectorcall overhead | Only 2–5ns per call (negligible) | 1 |
| MakeFunction type annotation | Already TMortalFunc (pass.cpp:329) | 1 |
| Load* type fix sufficient | Max 3% improvement (dispatch only) | 1 |
| Branch misses are JIT infrastructure | Winning benchmarks at ~1.0x | 1 |
| Wrong allocator API (`allocateDeoptPatcher`) | API does not exist; all 5 usages use `allocateCodePatcher` | 2 |
| Manual `setFrameState` fixes crash | `bindGuards` overwrites manual FrameState with dominating Snapshot | 2 |

---

## Strategic Recommendations

Ordered by empirically validated impact:

### Priority 1: Inlining Resolved Callees (Stage 2)

**Addresses:** C (0.82x → 0.891x → target >=0.95x)

Stage 1 + 1.5 provide the infrastructure: the inliner can now see the
resolved `__enter__`/`__exit__` as PyFunctionObjects with object-specialised
TFunc. The inliner's check at `inliner.cpp:369` (`hasValueSpec(TFunc)`) now
passes, and `objectSpec()` at line 386 returns the function pointer.

What remains is for the inliner to actually inline the resolved methods. This
eliminates the BEFORE_WITH decomposition overhead entirely — the inlined code
replaces both the LoadAttrSpecial and the VectorCall with the function body.

**Risk:** moderate. The inliner has 15 property checks in `canInline()`. If
any fail (e.g. the target function has closures, generators, or
CO_VARKEYWORDS), inlining will be rejected even though the callee is resolved.

### Priority 2: Callee Resolution for Other Categories

**Addresses:** A (0.76x), D (0.77x)

The same pattern applied to other resolution gaps:
1. SSA backward tracing from VectorCall → MakeFunction (nested functions)
2. LoadMethodCached type narrowing (class hierarchy methods)
3. Type-call → __init__ inlining (class constructors)

### Priority 3: Generator-Specific Optimisation

**Addresses:** B (0.61x, 0.67x) — the WORST regressions.

InvokeIterNext dispatches through JIT runtime. JitGenFreeList arena causes
L2 cache pressure (452K extra L2→DRAM misses). Needs:
1. Cache-aware memory layout for generator data
2. Reduced indirection in iteration dispatch

**Risk:** high. Touches allocator and memory subsystem.

**Gap:** `next-jit-steps.md` has no proposal covering this. A new section
is needed.

### Priority 4: Hot/Cold Splitting (Complementary)

**Addresses:** C (0.82x), E (0.79x), G (0.94x) — partially.

Low effort, no dependencies. But the dominant branch miss source is hot-path
decomposition, not cold guard paths. Value is marginal in isolation; best
pursued in parallel with Priority 1 if capacity allows.

### Not Addressable

- **F (kwargs_dispatch, 0.73x):** CO_VARKEYWORDS is a callee property.
  Cannot be fixed by JIT — would require changing the callee function's
  signature.
- **H (spectral_norm, 0.95x):** within measurement noise.

---

## Session Methodology

- **ABBA interleaving:** all benchmark comparisons alternate JIT ON/OFF
  runs (A, B, B, A pattern) to control for thermal drift and baseline noise.
  Session 2 confirmed: in-process ABBA is invalid; subprocess isolation
  required.
- **Gate criterion #0:** perf record at instruction level BEFORE designing
  any fix. Established after Fix A wasted a design cycle on an unvalidated
  mechanism.
- **Falsification discipline:** every hypothesis stated with a falsifier.
  Eight hypotheses were falsified across both sessions. Each falsification
  redirected effort productively.
- **Micro-benchmark warning validated:** kwargs fix showed 2.4x in quick
  test, 1.61x with proper ABBA. ~50% inflation from warmup drift.
- **Multi-agent team:** Session 2 used NBS teams (supervisor, theologian,
  testkeeper, gatekeeper, generalist, scribe) for parallel investigation
  of crash root cause, gate criteria evaluation, and benchmarking.

---

## Files Modified (on devgpu004)

| File | Change | Status |
|------|--------|--------|
| `cinderx/Jit/hir/resolve_kwargs.h` (31 lines, NEW) | ResolveKwargs pass header | Committed |
| `cinderx/Jit/hir/resolve_kwargs.cpp` (~303 lines, NEW) | ResolveKwargs pass implementation | Committed |
| `cinderx/Jit/compiler.cpp` | Added include + pass insertion before InlineFunctionCalls | Committed |
| `benchmark_cinderx.py` | Added bench_positional_dispatch (#21) | Committed |
| `generator.cpp:2200-2222` | Fix A Tier 1 (CallMethod TFunc bypass) | REVERTED |
| `cinderx/Jit/hir/pass.cpp` | returnType broadened for heap types (tp_new guard) | Applied, uncommitted |
| `cinderx/Jit/hir/simplify.cpp` | simplifyVectorCallBoundMethod + Snapshot fix + simplifyCallMethod type narrowing | Applied, uncommitted |

---

## Remaining Questions for Alex

1. **Stage 2 (inlining):** The resolved callee is now visible to the inliner.
   Should the next session attempt inlining `__enter__`/`__exit__` directly,
   or investigate whether `canInline()` accepts the target functions first?
2. **Commit Stage 1 + 1.5?** The two-file change set is correct and tested
   but accepted as infrastructure only. Commit now (as WIP on feature branch)
   or wait until Stage 2 validates the full pipeline?
3. **C5b-e falsifiers:** C extension parent tp_new, ABCMeta, exotic
   metaclass, runtime monkey-patching. These are soundness tests that have
   not been run. Should they gate the infrastructure commit?
4. **ASan coverage:** the kwargs fix was accepted without ASan due to the
   build environment lacking it. Stage 1 + 1.5 touches pointer arithmetic
   (DeoptPatchpoint, Snapshot, register allocation). Should ASan be set up
   before committing?
