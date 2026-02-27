# Adaptive Interpreter Specialisation Investigation

**Date:** 27 February 2026
**Commit:** 1fa46c9b (origin/aarch64-jit-generators)
**Platform:** aarch64 Grace CPU (build-host
**Context:** CinderX JIT geomean 0.984x across 24 benchmarks — JIT is a net loss

## Direction

Alex's question (21:35:17Z): *"Why are we looking at deopts when the goal was about taking advantage of the adaptive interpreter's adaptation bytecodes?"*

Clarification (21:40:31Z): *"You can use the adaptive specialisation to guide optimisation. Because the adaptive interpreter has really good type information."*

The investigation shifted from deopt-triggered recompilation (Option B, code-complete but shelved) to understanding how effectively the JIT uses the adaptive interpreter's specialised bytecodes as compilation guidance.

## Methodology

Read the source code of builder.cpp, simplify.cpp, and codegen/autogen.cpp on build-host Traced the flow from specialised bytecode → GuardType emission → Simplify pass → LIR → machine code.

## Findings

### 1. What the JIT DOES with adaptation data (working)

| Specialised Bytecode | Builder (builder.cpp) | Simplify (simplify.cpp) | Deep Optimisation |
|---|---|---|---|
| BINARY_OP_ADD_INT | GuardType(left, TLongExact) + GuardType(right, TLongExact) | BinaryOp → LongBinaryOp | Direct `_PyLong_Add` call (skip `PyNumber_Add`) |
| BINARY_OP_ADD_FLOAT | GuardType(left, TFloatExact) + GuardType(right, TFloatExact) | BinaryOp → FloatBinaryOp | **Unboxed C double** (`PrimitiveUnbox` + `DoubleBinaryOp`) |
| BINARY_SUBSCR_LIST_INT | GuardType(left, TListExact) + GuardType(right, TLongExact) | BinaryOp → IndexUnbox + CheckSequenceBounds + LoadArrayItem | **Direct array access** (skip `PyObject_GetItem`) |
| LOAD_ATTR_INSTANCE_VALUE | GuardType(receiver, ExactType) | LoadAttr → simplifyLoadAttrSplitDict | **Direct field load** via split dict values array + SplitDictDeoptPatcher |
| LOAD_ATTR_MODULE | GuardType(receiver, PyModule_Type) + dict version check | Inline dict access | Direct entry load via JITRT_LoadModuleDictEntry |
| FOR_ITER_RANGE | GuardType(iterator, range_iterator_type) at GET_ITER | InvokeIterNext → CallStatic | Direct iterator advancement |
| COMPARE_OP_INT | GuardType(left, TLongExact) + GuardType(right, TLongExact) | Compare with known types | Direct comparison |

The JIT IS performing deep optimisations with adaptation data. Float operations are unboxed to C doubles. List subscripts become direct array loads. Instance attribute loads become direct field loads with patcher-based invalidation.

### 2. Type propagation through operations (working)

- LongBinaryOp outputs TLongExact (hir.h:1853)
- FloatBinaryOp outputs TFloatExact (hir.h:1971)
- Within a basic block, guards on operation results ARE eliminated by `simplifyGuardType` (simplify.cpp:299)
- Example: `GuardType(x, TLongExact)` → `LongBinaryOp(x, y)` → result is TLongExact → subsequent `GuardType(result, TLongExact)` eliminated

### 3. CONFIRMED GAP: CALL specialisation not used

**Location:** builder.cpp:2241-2274

The builder handles `case CALL:` with a generic `CallMethod` emission. It does **not** check `bc_instr.specializedOpcode()` for CALL instructions.

**What is ignored:**
- CALL_PY_EXACT_ARGS: callee is a Python function, args match exactly
- CALL_BOUND_METHOD_EXACT_ARGS: callee is a bound method, args match
- CALL_BUILTIN_CLASS: callee is a builtin type constructor

**What IS optimised for calls (Simplify pass):**
1. Global functions: `simplifyVectorCallGlobal` (line 2028) — LoadGlobalCached → GuardIs → GlobalDeoptPatcher → LoadConst + Static VectorCall → inliner can inline
2. Context manager __enter__/__exit__: `simplifyVectorCallBoundMethod` (line 1902) — resolves method via MRO at compile time (limited to 4 specific attributes)
3. C method descriptors: `trySpecializeCCall` (line 1753) — direct CallStatic for METH_NOARGS / METH_O

**What is NOT optimised:** Regular method calls (`obj.method()`). These go through full vectorcall dispatch: function object lookup → vectorcall slot check → arg validation → entry point resolution.

**Cost:**
- Adaptive interpreter CALL_PY_EXACT_ARGS: ~15 instructions
- JIT VectorCall: ~40+ instructions
- Delta: ~25 instructions per method call

### 4. Guard overhead on aarch64

**From codegen/autogen.cpp:346-354 (aarch64 path), a GuardType emits:**
1. `ldr scratch, [obj + ob_type]` — load type pointer (memory access)
2. `emit_cmp(scratch, expected)` — compare with expected type
3. `b_ne deopt_label` — conditional branch to deopt

Cost: ~4-5 cycles per guard (L1 hit assumed for ob_type).

**Fundamental asymmetry:** The adaptive interpreter pays O(0) per iteration for type checks (specialised opcodes trust the type, rely on invalidation for changes). The JIT pays O(n_guards) per iteration (re-verifies every type every time). For stable types, the interpreter's invalidation-based approach is strictly better.

### 5. Two populations of structural losers

**Call-heavy (CALL gap is primary lever):**
- func_calls (0.91x) — 1 CALL_PY_EXACT_ARGS per iteration
- nn_module_forward (0.76x) — CALL_BOUND_METHOD_EXACT_ARGS per method dispatch
- pytorch_cm (0.69x) — 6+ __enter__/__exit__ calls per iteration
- context_manager (0.90x) — __enter__/__exit__ dispatch
- decorator_chain (0.87x) — closure calls through stacked decorators

**Arithmetic (different overhead dominates):**
- int_arith (0.91x) — zero function calls in hot path
- float_arith (0.97x) — zero function calls in hot path

For the arithmetic benchmarks, the overhead is from per-iteration guards on loop variables and the fact that `_PyLong_Add` is not meaningfully faster than the adaptive interpreter's `BINARY_OP_ADD_INT` C path — both call the same underlying C function.

## Proposed fix directions

### For CALL-heavy benchmarks: Method call specialisation

**Approach A (GuardIs-based, reuses existing infrastructure):**
1. In builder.cpp case CALL: check `bc_instr.specializedOpcode()`
2. For CALL_PY_EXACT_ARGS: read cached function from bytecode inline cache
3. Emit GuardIs(func, cached_function)
4. Existing `simplifyVectorCallGlobal` path then optimises → LoadConst + inlining

**Approach B (patcher-based, aligned with Alex's direction):**
1. Emit TypeDeoptPatcher on receiver's type
2. If type's method table changes, invalidate JIT code
3. Cost: O(type_changes) not O(iterations)
4. Matches the adaptive interpreter's invalidation model

Approach B is architecturally superior but more complex.

### For arithmetic benchmarks: Guard elimination

The per-iteration guard overhead on loop variables is the dominant cost. TypeDeoptPatcher could replace per-iteration GuardType with O(type_changes) invalidation — the same insight Alex raised. The JIT should *trust* the adaptation more.

## Shelved work

Adaptive recompilation (Option B from the previous session) is code-complete across 4 files (context.h, context.cpp, pyjit.cpp, simplify.cpp) but not pushed to build-host The direction pivot means this work is deprioritised. It addresses only 4 deopt-caused benchmarks and cannot move the geomean above 1.0x alone.

## Next steps

1. Implement CALL specialisation (Approach A first — lower risk, reuses infrastructure)
2. Measure impact on call-heavy structural losers
3. Investigate TypeDeoptPatcher approach for guard elimination (Approach B)
4. Get HIR dumps for structural losers to verify guard counts and identify specific overhead sources
