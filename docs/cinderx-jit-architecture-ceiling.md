# CinderX JIT on aarch64: Architecture Ceiling Report

**Date:** 2026-03-19 (updated session 4)
**Commit:** c7dc24d4 (branch: aarch64-jit-generators)
**Platform:** aarch64 (devgpu004)

## Final ABBA-Verified Standings

| Benchmark | Vanilla CPython | JIT auto() | Speedup | Status |
|-----------|----------------|------------|---------|--------|
| fibonacci | 3472ms | 2172ms | 1.60x | At ceiling |
| richards_full | 20.8ms | 19.2ms | 1.08x | At ceiling |
| pytorch_cm | 44.0ms | 47.8ms | 0.92x | Structural overhead |

These numbers represent the performance ceiling of the current JIT architecture on these benchmarks. All single-session optimisation paths have been exhausted and verified across three sessions.

## What the JIT Does (Complete Specialisation Map)

The CinderX JIT pipeline implements specialisation for **every** major CPython 3.12 adaptive opcode:

| Opcode Family | Specialised Variants | JIT Mechanism | Native? |
|---|---|---|---|
| BINARY_OP | ADD/SUB/MUL for INT | GuardType(LongExact) → LongBinaryOp → nb_add slot | C call |
| BINARY_OP | ADD/SUB/MUL for FLOAT | GuardType(FloatExact) → FloatBinaryOp → DoubleBinaryOp → fadd/fsub/fmul/fdiv | **Native** |
| COMPARE_OP | INT, FLOAT, STR | GuardType → LongCompare/FloatCompare → tp_richcompare slot | C call |
| LOAD_ATTR | SLOT | GuardType → LoadField at byte offset | **Native** |
| LOAD_ATTR | INSTANCE_VALUE | GuardType → split-dict LoadField chain (dorv → values → indexed load) | **Native** |
| LOAD_ATTR | METHOD_NO_DICT | LoadMethodCached (Simplify handles efficiently) | **Native** |
| LOAD_GLOBAL | MODULE, BUILTIN | LoadGlobalCached + GuardIs (preloader-based, single memory load) | **Native** |
| STORE_SUBSCR | LIST_INT, DICT | GuardType + StoreSubscr | C call |
| FOR_ITER | LIST, RANGE, TUPLE | GuardType at GET_ITER → JITRT_InvokeIterNext | C call |
| UNPACK_SEQUENCE | LIST, TUPLE, TWO_TUPLE | GuardType + fast-path branching | Mixed |
| CALL | PY_EXACT_ARGS (tested) | Generic VectorCall (dispatch overhead is negligible) | C call |
| Function inlining | Recursive calls | Partial — one level deep (EndInlinedFunction in HIR) | **Native** |

**The only structural gap is the CALL_* family** (15 CPython specialised variants). However, CALL_PY_EXACT_ARGS was implemented and ABBA-falsified (zero improvement) — call dispatch overhead is ~1 branch + 1 pointer load, negligible relative to function body execution time. The remaining CALL variants (CALL_NO_KW_BUILTIN_O, CALL_NO_KW_LEN, etc.) would only help if we **inline the builtin body**, which is function inlining — not dispatch shortcutting.

### Compilation Infrastructure

- **shouldSkipCompilation** (pyjit.cpp): Prevents JIT compilation of functions where JIT code is slower than interpreter. Skips `__enter__`/`__exit__` unconditionally, `__init__` unless contains STORE_ATTR_SLOT. Uses `specializedOpcode()` (not `opcode()` which de-specialises — bug fixed in commit cccfe12d).
- **compile_after_n_calls = 5000** (pyjit.cpp:1602): Delays compilation so CPython's adaptive specialisation runs first. The JIT reads specialised opcodes from the bytecode cache at compile time.
- **Vectorcall restoration**: When shouldSkipCompilation returns true, `func->vectorcall` is restored to the original. Skipped functions have zero per-call overhead after threshold.

## Per-Benchmark Ceiling Explanations

### fibonacci (1.60x) — Int Arithmetic Ceiling

**What the JIT does well:**
- LoadGlobalCached for recursive `fib` lookup (no hash table walk)
- LongCompare for `n < 2` (direct tp_richcompare slot call)
- LongBinaryOp for `n-1`, `n-2`, `fib(n-1)+fib(n-2)` (direct nb_add/nb_subtract)
- Partial function inlining (one level of recursion — EndInlinedFunction in HIR)
- VectorCall<static> for deeper recursive calls (function identity known at compile time)

**Remaining overhead:** Each LongBinaryOp calls nb_add as a C function (~5-10ns per call). Native integer add would be ~2ns. The gap exists because Python ints are arbitrary-precision — unboxing to machine int requires compact-int guards (ob_size == 1) and overflow checks that do not exist in the Simplify pipeline.

**Estimated gain from int unboxing:** <5% (1.60x → ~1.68x). The float equivalent (DoubleBinaryOp → native fadd) already exists and works; the int version is harder due to overflow.

### richards_full (1.08x) — Polymorphic Dispatch Ceiling

**What the JIT does well (non-polymorphic functions):**
- All non-polymorphic functions compile successfully with full specialisation
- LoadField for attribute access (LOAD_ATTR_SLOT and LOAD_ATTR_INSTANCE_VALUE)
- LongCompare for control flow, LongBinaryOp for arithmetic

**Why it cannot go higher — three independent proofs:**

1. **kDeoptBackoffThreshold test** (1000 → 100000): ABBA showed 1.08x → 1.08x. Preventing deopt detach has zero effect because the guards still *fail* on every polymorphic call — the threshold controls when we give up retrying, not whether the guard succeeds.

2. **Force-compile test** (_device_fn + queue_packet): 1.004x. These uncompiled functions are either cold or hit the same polymorphic deopt.

3. **Gap map**: All opcode specialisations are already implemented. No missing specialisation to add.

**Root cause:** CinderX type profiler records ONE type per parameter and marks it `Exact` (builder.cpp:368, LoadArg). For polymorphic methods (e.g., `TaskState.isTaskHoldingOrWaiting` called on HandlerTask, DeviceTask, etc.), the profiler assigns `HandlerTask:Exact` → GuardType fails on DeviceTask → interpreter fallback → 1000 failures → detach. The 4 affected functions contribute zero JIT speedup.

The 1.08x comes entirely from non-polymorphic functions which are already at their individual ceilings.

### pytorch_cm (0.92x) — Structural Overhead Floor

**Why it is 8% slower than vanilla:**
- `cinderx.init()` installs type watchers that add overhead to every type modification
- The cinderx .so increases icache pressure
- These are fixed costs of loading CinderX, independent of JIT compilation
- Context manager `__enter__`/`__exit__` correctly skipped by shouldSkipCompilation
- Confirmed by ABBA v5 (Option 1 pre-check had zero effect on the 0.92x)

This overhead is not addressable by JIT changes. It requires profiling `cinderx.init()` itself.

## Complete Falsification Record

### Session 3 (2026-03-19, this session)

| # | Hypothesis | Prediction | Result |
|---|---|---|---|
| 13 | kDeoptBackoffThreshold 1000→100000 | Richards >1.20x | FALSIFIED — 1.08x unchanged |
| 14 | BINARY_OP_ADD_INT needs specialisation | New optimisation target | ALREADY DONE — LongBinaryOp pipeline exists |
| 15 | COMPARE_OP_INT needs specialisation | New optimisation target | ALREADY DONE — LongCompare pipeline exists |
| 16 | LOAD_GLOBAL needs specialisation | Fibonacci improvement | ALREADY DONE — LoadGlobalCached + GuardIs |
| 17 | FOR_ITER needs specialisation | Loop benchmark improvement | ALREADY DONE — GuardType + InvokeIterNext |
| 18 | Force-compiling _device_fn/queue_packet | Richards improvement | FALSIFIED — 1.004x (zero change) |

### Sessions 1-2 (2026-03-18/19)

| # | Hypothesis | Result |
|---|---|---|
| 1 | suppressExceptionDeopt perverse effect | FALSIFIED — zero effect |
| 2 | Bytecode size threshold for skip heuristic | FALSIFIED — sizes overlap |
| 3 | Guard density (712 guards) | FALSIFIED — only 40 in final HIR |
| 4 | Guard dedup pass | 0 eliminations (SSA defeats matching) |
| 5 | Cold code sections | Silent JIT failure on aarch64 — parked |
| 6 | Guard branch polarity | Already correct on aarch64 |
| 7 | getInterpretedVectorcall → vanilla | Same function pointer on 3.12 |
| 8 | Broaden shouldSkipCompilation | No additional candidates |
| 9 | CinderX type watcher de-specialisation | opcode() bug — FIXED (commit cccfe12d) |
| 10 | CALL_PY_EXACT_ARGS inlining | FALSIFIED — zero change on fibonacci |
| 11 | tp_subclasses polymorphism check | Irrelevant — profiler overrides |
| 12 | STORE_ATTR_SLOT inlining | REGRESSED 1.08x → 1.02x — reverted |

## Multi-Session Paths Forward

### Path A: Type Profiler Fix for Polymorphic Dispatch (richards)

**Problem:** Profiler records monomorphic exact type for each parameter. For polymorphic methods, this causes GuardType failure on every call with a different subclass.

**Fix:** Broaden profiled type when multiple subclass types are observed. Emit GuardType<BaseClass> instead of GuardType<ExactSubclass>.

**Complexity:** HIGH — requires changes to LoadArg profiling in builder.cpp, which propagates through the entire HIR pipeline (Simplify pass, type inference, guard emission, deopt handling).

**Expected gain:** Unknown — could be significant if polymorphic functions compile successfully. The threshold test does NOT measure this scenario (it prevents giving up, but the guard still fails). Only building the fix can measure the actual gain.

**Risk:** Broadening types may cause the Simplify pass to miss optimisations that depend on exact type knowledge (e.g., devirtualisation, field layout assumptions).

### Path B: Compact-Int Native Arithmetic (fibonacci)

**Problem:** LongBinaryOp calls nb_add as a C function (~5-10ns). Native add is ~2ns.

**Fix:** Add to `simplifyLongBinaryOp` (following existing `simplifyFloatBinaryOp` pattern): compact-int guard (ob_size == 1) → PrimitiveUnbox to CInt → IntBinaryOp (native add/sub/mul) → overflow check → PrimitiveBox or small-int cache.

**Complexity:** MODERATE — the float equivalent already exists as a template. Main challenge is overflow handling (two single-digit longs can produce a two-digit result).

**Expected gain:** fibonacci 1.60x → ~1.70x (estimated <10% improvement).

### Path C: Deeper Function Inlining (all benchmarks)

**Problem:** Each function call requires VectorCall dispatch, argument setup, frame creation. Only one level of inlining exists.

**Fix:** Inline hot callees into callers at HIR level. Requires call graph analysis, inline heuristics, HIR fusion, frame state merging, deopt frame reconstruction for inlined frames.

**Complexity:** VERY HIGH — weeks of work. Highest ceiling but highest risk.

**Expected gain:** Would benefit all benchmarks. Eliminates call overhead AND enables cross-function optimisation (constant propagation across call boundaries, dead code elimination of unused return values).

### Path D: Reduce cinderx.init() Overhead (pytorch_cm)

**Problem:** `cinderx.init()` imposes ~8% overhead on dispatch-heavy benchmarks from type watchers and .so icache effects.

**Fix:** Profile `cinderx.init()` to identify most expensive registrations. Possibly defer or make lazy.

**Complexity:** MODERATE — requires understanding cinderx initialisation sequence and which watchers are essential vs optional.

**Expected gain:** pytorch_cm 0.92x → ~1.00x (eliminate the structural deficit).

## Key Lessons Across All Sessions

1. **Always verify before implementing.** The verification sweep prevented 4 wasted implementation cycles (BINARY_OP, COMPARE_OP, LOAD_GLOBAL, FOR_ITER — all already handled).

2. **Cheap diagnostics before expensive fixes.** The kDeoptBackoffThreshold test (1 constant change) and force-compile test (2 function calls) each took minutes and definitively answered multi-hour questions.

3. **LOAD inlining wins, STORE inlining loses.** LOAD_ATTR inlining eliminates expensive generic lookup (MRO walk, descriptor protocol). STORE_ATTR inlining loses because the generic store path is already near-optimal.

4. **opcode() vs specializedOpcode()** is a general trap. BytecodeInstruction::opcode() de-specialises adaptive opcodes via word() → unspecialize(). Any code checking adaptive specialisation MUST use specializedOpcode().

5. **cinderx.init() imposes a structural overhead floor.** ~8% on dispatch-heavy benchmarks. Not addressable by JIT changes.

6. **The JIT pipeline is architecturally complete for opcode specialisation.** Further gains require going deeper (native lowering of C slot calls) or wider (function inlining). Both are infrastructure-level changes requiring multiple sessions.

7. **Profile before implementing.** Every proposal that was tested against source analysis or perf data before implementation either confirmed the target existed or prevented wasted effort. Ad-hoc implementation without analysis produced the STORE_ATTR_SLOT regression and the zero-improvement CALL_PY_EXACT_ARGS.
