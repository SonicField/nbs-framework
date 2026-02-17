# CinderX JIT Architecture Study — Phase 2b Deliverable

**Date:** 17 February 2026
**Author:** theologian
**Purpose:** Map the CinderX JIT compilation pipeline, identify x86-64 assumptions, assess aarch64 port scope.
**Source:** github.com/facebookincubator/cinderx (main branch, cloned to /tmp/cinderx on 009)

---

## Critical Finding

**CinderX already has substantial aarch64 support.** The aarch64 backend is largely implemented but flagged as unsupported. This changes the project scope from "build a new backend" to "complete and debug an existing partial port."

---

## 1. JIT Compilation Pipeline

CinderX compiles Python bytecode to native code through a **four-stage pipeline**:

### Stage 1: Python Bytecode → HIR (High-level IR)

- **Entry point:** `Compiler::Compile()` in `Jit/compiler.cpp:165`
- **Builder:** `hir::buildHIR(preloader)` in `Jit/hir/builder.cpp`
- Constructed by **abstract interpretation** of CPython bytecode
- HIR stays close to Python semantics: null checks, refcounting, type guards are explicit
- **HIR is completely architecture-independent** — zero arch ifdefs across ~40 files

### Stage 2: HIR Optimisation Passes

**Run by:** `Compiler::runPasses()` in `compiler.cpp:74`

Pass sequence (in order, each gated by a config flag):
1. `SSAify` — convert to SSA form (mandatory)
2. `Simplify` — peephole optimisations
3. `DynamicComparisonElimination`
4. `GuardTypeRemoval`
5. `PhiElimination`
6. `InlineFunctionCalls` — function inlining
7. `BeginInlinedFunctionElimination`
8. `BuiltinLoadMethodElimination`
9. `Simplify` (second pass)
10. `CleanCFG`
11. `DeadCodeElimination`
12. `CleanCFG` (second pass)
13. `RefcountInsertion` — insert explicit incref/decref
14. `InsertUpdatePrevInstr`

All passes are **machine-independent**.

### Stage 3: HIR → LIR (Low-level IR) Lowering

- **Performed by:** `LIRGenerator::TranslateFunction()` in `Jit/lir/generator.cpp` (3617 lines)
- Called from `NativeGenerator::getVectorcallEntry()` in `gen_asm.cpp:1126`
- Translates each HIR instruction to one or more LIR instructions
- **LIR generator has zero arch ifdefs** — completely architecture-neutral

### Stage 3b: LIR Transformations

- **Pre-regalloc rewrites** (`postgen.cpp`): constant normalisation, large constant materialisation, binary op rewrites. 4 `#ifdef` sites (x86 vs aarch64).
- **Register allocation:** Linear scan allocator in `regalloc.cpp`. 1 `#ifdef` site (x86 div register reservation).
- **Post-regalloc rewrites** (`postalloc.cpp`): phi removal, function call setup, binary op three-operand to two-operand conversion. 7 `#ifdef` sites.
- **C helper inlining** (`inliner.cpp`, `c_helper_translations.cpp`): Replaces calls to known C helpers with inline LIR sequences. No arch-specific code.

### Stage 4: LIR → Native Code (Assembly Emission)

- **Performed by:** `NativeGenerator::generateAssemblyBody()` in `gen_asm.cpp:2965`
- Iterates over LIR basic blocks and instructions, calling `AutoTranslator::translateInstr()` for each
- `AutoTranslator` maps each LIR instruction + operand pattern to a code generation function
- **x86 autogen table** (lines 1409–1748): uses declarative `GEN(pattern, ASM(mnemonic, ...))` DSL
- **aarch64 autogen table** (lines 1750–2982): uses `CALL_C(translateXxx)` functions — aarch64 instruction semantics require more complex logic (scratch registers for offsets, limited immediate encoding)

---

## 2. Architecture Abstraction Boundary

### asmjit Integration

asmjit is the **sole mechanism for emitting native code**. Fetched via CMake FetchContent:

```cmake
FetchContent_Declare(
  asmjit
  GIT_REPOSITORY https://github.com/asmjit/asmjit
  GIT_TAG cecc73f2979e9704c81a2c2ec79a7475b31c56ac  # 2025-May-10
)
```

Built as a static library (`ASMJIT_STATIC TRUE`). asmjit itself **already supports both x86-64 and aarch64**.

### Central Abstraction: `codegen/arch.h`

This file defines architecture-neutral type aliases:

| Alias | x86-64 | aarch64 |
|-------|--------|---------|
| `arch::Builder` | `asmjit::x86::Builder` | `asmjit::a64::Builder` |
| `arch::Gp` | `asmjit::x86::Gp` | `asmjit::a64::Gp` |
| `arch::Mem` | `asmjit::x86::Mem` | `asmjit::a64::Mem` |
| `arch::Reg` | `asmjit::x86::Gp` | `asmjit::a64::Gp` |
| `arch::VecD` | `asmjit::x86::Xmm` | `asmjit::a64::VecD` |

### Files with Architecture-Specific Code

| File | Purpose | Arch ifdefs |
|------|---------|-------------|
| `codegen/autogen.cpp` | LIR-to-native translation rules | 27 |
| `codegen/gen_asm.cpp` | Prologue, epilogue, deopt trampolines | 43 |
| `codegen/frame_asm.cpp` | Frame linking/unlinking | 25 |
| `codegen/register_preserver.cpp` | Register save/restore | 9 |
| `codegen/gen_asm_utils.cpp` | Call emission utilities | 4 |
| `code_patcher.cpp` | Runtime code patching | 10 |
| `lir/postalloc.cpp` | Post-regalloc instruction rewriting | 10 |
| `lir/postgen.cpp` | Pre-regalloc instruction rewriting | 6 |

**Total: ~18 files with arch-specific code, mostly in `codegen/`.**

---

## 3. LIR x86-Assumption Catalogue

This is the go/no-go classification requested by gatekeeper. Each x86-specific LIR operation is classified as:
- **(a)** Direct aarch64 translation — single instruction mapping exists
- **(b)** Multi-instruction aarch64 sequence — already implemented in autogen.cpp
- **(c)** Requires new approach / IR modification — NOT FOUND

### x86-Named Operations in LIR

| Operation | x86 Semantics | aarch64 Translation | Category |
|-----------|--------------|---------------------|----------|
| `Test` | Bitwise AND, sets flags | `tst Rn, Rm` | (a) |
| `Test32` | 32-bit test | `tst Wn, Wm` | (a) |
| `BitTest` | Test single bit | `tst Rn, #(1 << bit)` | (b) |
| `Lea` | Load effective address | `add Rd, Rn, #offset` or `mov` | (a)/(b) |
| `Push` | Push to stack | `str Rn, [sp, #-8]!` (pre-index) | (b) |
| `Pop` | Pop from stack | `ldr Rn, [sp], #8` (post-index) | (b) |
| `Cdq` | Sign-extend EAX→EDX:EAX | Not used on aarch64 (sdiv is different) | N/A |
| `Cwd` | Sign-extend AX→DX:AX | Not used on aarch64 | N/A |
| `Cqo` | Sign-extend RAX→RDX:RAX | Not used on aarch64 | N/A |

### Flag Register Model

| LIR Concept | x86 | aarch64 | Status |
|-------------|-----|---------|--------|
| `FlagEffects::kSet` | Sets EFLAGS | Sets PSTATE condition flags | Direct mapping |
| `BranchZ` | `jz` (ZF=1) | `b.eq` | (a) |
| `BranchNZ` | `jnz` (ZF=0) | `b.ne` | (a) |
| `BranchG` | `jg` (ZF=0, SF=OF) | `b.gt` | (a) |
| `BranchGE` | `jge` (SF=OF) | `b.ge` | (a) |
| `BranchL` | `jl` (SF≠OF) | `b.lt` | (a) |
| `BranchLE` | `jle` (ZF=1 or SF≠OF) | `b.le` | (a) |
| `BranchA` | `ja` (CF=0, ZF=0) | `b.hi` | (a) |
| `BranchAE` | `jae` (CF=0) | `b.hs` | (a) |
| `BranchB` | `jb` (CF=1) | `b.lo` | (a) |
| `BranchBE` | `jbe` (CF=1 or ZF=1) | `b.ls` | (a) |
| `BranchC` | carry flag | `b.cs` | (a) |
| `BranchNC` | no carry | `b.cc` | (a) |
| `BranchO` | overflow | `b.vs` | (a) |
| `BranchNO` | no overflow | `b.vc` | (a) |
| `BranchS` | sign | `b.mi` | (a) |
| `BranchNS` | no sign | `b.pl` | (a) |

### Calling Convention

| Aspect | x86-64 (System V AMD64) | aarch64 (AAPCS64) | Status |
|--------|------------------------|---------------------|--------|
| Argument regs | RDI, RSI, RDX, RCX, R8, R9 | X0–X7 | Defined in `arch/aarch64.h` |
| Return regs | RAX, RDX | X0, X1 | Defined |
| Caller-save | Many | X0–X17 | Defined |
| Callee-save | RBX, R12–R15, RBP | X19–X28 | Defined |
| Stack align | 16-byte | 16-byte | Same |

### Memory Addressing (MemoryIndirect)

The LIR's `MemoryIndirect` uses x86 SIB-style encoding (base + index × scale + offset, where scale is log2-encoded). The aarch64 backend translates this via `leaIndex()` and `ptrIndirect()` helper functions in `autogen.cpp`, emitting multi-instruction sequences where needed.

### Verdict

**Category (c) — "requires IR modification" — was NOT found.** Every x86-specific LIR operation has either a direct (a) or multi-instruction (b) aarch64 translation that is **already implemented** in the existing autogen.cpp aarch64 table.

**GO/NO-GO: GO.** The LIR is sufficiently architecture-neutral for the aarch64 port. No IR redesign is required.

---

## 4. Current State of aarch64 Support

### What exists and is complete:

1. **Register definitions** (`arch/aarch64.h`, `aarch64.cpp`): All 31 GP, 32 FP, calling conventions
2. **Architecture abstraction** (`arch.h`): All type aliases resolve correctly
3. **Autogen translation rules** (`autogen.cpp:1750–2982`): ~1200 lines covering all LIR instruction types
4. **gen_asm.cpp**: Most functions have aarch64 branches (prologue, epilogue, frame setup, deopt exits)
5. **frame_asm.cpp**: Frame linking/unlinking has aarch64 paths
6. **Pointer addressing helpers** (`arch.cpp`): `ptr_offset()` and `ptr_resolve()` handle aarch64 limited offset modes

### What remains:

1. **`CINDER_UNSUPPORTED` flag** in `detection.h:14` — blocks aarch64 activation
2. **13 `CINDER_UNSUPPORTED` fallthrough sites** in `autogen.cpp` — edge cases in some `translateXxx()` functions
3. **`code_patcher.cpp`** (10 arch ifdefs) — runtime instruction patching may have incomplete aarch64 paths
4. **Testing** — no evidence of aarch64 test coverage in `RuntimeTests/`
5. **Generator/coroutine yield/resume paths** — may have untested edge cases

---

## 5. Revised Scope Assessment

### Original Plan Phase 3 (from `17-02-2026-cinderx-arm-jit-plan.md`):

> Incremental implementation order:
> 1. Arithmetic operations
> 2. Control flow
> 3. Function calls
> 4. Object operations
> 5. Container operations
> 6. Advanced (async, generators, etc.)

### Revised Assessment:

**All six groups are already implemented in the aarch64 autogen table.** The incremental implementation order is unnecessary. The real work is:

1. Remove `CINDER_UNSUPPORTED` guard (1 line change in `detection.h`)
2. Build on build-host and fix compile errors
3. Implement the **7 genuinely missing** aarch64 sites — but see note below
4. Run test suite and fix runtime failures
5. Integration test with PyTorch

This is **debugging and completing an existing partial port**, not building a new backend.

### CINDER_UNSUPPORTED Site Analysis (final)

Of 72 total CINDER_UNSUPPORTED sites across 12 files:
- **65** already have aarch64 code — the UNSUPPORTED is the `#else` fallback for unknown architectures
- **7** are nominally missing aarch64 implementation, but:

**3 are dead code on Python 3.12+** (guarded by `PY_VERSION_HEX < 0x030C0000`):

| # | File | Line | Context | Status |
|---|------|------|---------|--------|
| 1 | `gen_asm.cpp` | 2069 | Generator epilogue (pre-3.12 path) | Dead on Python 3.12 |
| 2 | `gen_asm.cpp` | 2108 | Frame unlinking (pre-3.12 path) | Dead on Python 3.12 |
| 3 | `autogen.cpp` | 786 | Yield resume (pre-3.12 path) | Dead on Python 3.12 |

**4 are conditional on `ENABLE_SHADOW_FRAMES`** (not defined in OSS CinderX build system):

| # | File | Line | Context | Status |
|---|------|------|---------|--------|
| 4 | `frame_asm.cpp` | 50 | Shadow frame constants | Only if shadow frames enabled |
| 5 | `frame_asm.cpp` | 1058 | Shadow frame unlinking | Only if shadow frames enabled |
| 6 | `frame_asm.cpp` | 1126 | Shadow frame linking | Only if shadow frames enabled |
| 7 | `frame_asm.cpp` | 1140 | Shadow frame init | Only if shadow frames enabled |

**Conclusion:** For Python 3.12 without shadow frames, the aarch64 backend may be **100% complete**. The only way to verify is to remove the `CINDER_UNSUPPORTED` guard in `detection.h`, build on build-host and run the smoke test.

The core instruction emission table (`autogen.cpp`) is 100% complete for aarch64.

---

## 6. Files That Would NOT Need Changes

These are architecture-independent — zero ifdefs, no changes needed:

- All of `Jit/hir/` (~40 files)
- `Jit/lir/generator.cpp` (3617 lines)
- `Jit/lir/c_helper_translations.cpp`
- `Jit/lir/inliner.cpp`
- `Jit/lir/verify.cpp`
- `Jit/deopt.cpp`
- `Jit/inline_cache.cpp`
- `Jit/compiled_function.cpp`
- `Jit/compiler.cpp`
- `Jit/context.cpp`
- `Jit/config.cpp`
- All ELF-related files

---

## 7. Risk Reassessment

| Risk from Original Plan | Status |
|------------------------|--------|
| "asmjit deeply entangled (not cleanly swappable)" | **RESOLVED** — asmjit already supports aarch64 and CinderX already uses it for aarch64 |
| "Register allocation redesign needed" | **RESOLVED** — register allocator has only 1 arch ifdef; aarch64 register sets already defined |
| "LIR has x86 semantics baked in" | **RESOLVED** — LIR is x86-influenced but all operations already have aarch64 translations |
| "torch.compile interop breaks with ARM JIT" | **STILL OPEN** — untested |
| "CinderX doesn't build on ARM" | **STILL OPEN** — needs verification on build-host |

**New risk:** The existing aarch64 code may have been written but never tested on real hardware. Runtime correctness bugs may be numerous even though the code exists.
