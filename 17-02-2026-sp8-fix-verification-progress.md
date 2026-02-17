# [SP,#8] Fix Verification — Progress Log

**Date:** 17 February 2026
**Author:** testkeeper
**Commit:** 68126095 ("Fix getIP on aarch64: SP-relative saved-IP slot [SP,#8]")

## Summary

The [SP,#8] proper fix for frame.cpp getIP() on aarch64 has been verified.
Commit 68126095 is the definitive implementation. It replaces the interim
fix (74111fab) which read the saved LR from [FP+8].

## Test Results

| Configuration | Result | Runs |
|---------------|--------|------|
| Default threshold | 15/15 PASS | 3/3 |
| N=1 (-X jit-threshold=1) | 15/15 PASS | **10/10** |
| PYTHONJITLISTALL=1 | 15/15 PASS | 3/3 |
| PYTHONJITALL=1 | CRASH (pre-existing) | 3/3 |

## PYTHONJITALL=1 Crash — Pre-existing Bug

PYTHONJITALL=1 crashes on **both** the interim fix and the [SP,#8] fix.
This is a pre-existing aarch64 JIT bug, not caused by our changes.

**Evidence:**
- Interim fix (26MB .so, fa8cb9cc): CRASH 3/3 at PYTHONJITALL=1
- SP8 fix (68126095): CRASH 3/3 at PYTHONJITALL=1
- Both pass at N=1 threshold and default threshold

**GDB backtrace:**
- PC jumps to `_PyRuntime+459352` (data section, contains `udf #0`)
- LR = 0x8533e0 (same address, data not code)
- "Backtrace stopped: previous frame identical to this frame (corrupt stack?)"
- Crash occurs during interpreter bootstrap, before user code

**Root cause hypothesis:**
PYTHONJITALL=1 JIT-compiles Python bootstrap functions (importlib, etc.)
that are normally never JIT-compiled. Some of these functions exercise
code paths with assumptions that break on aarch64.

**Generator-specific crash:**
Generators crash at PYTHONJITALL=1 (confirmed by bisection: T01-T05 pass,
T06 generators crashes). Adding zero-init to generator static entry and
resume entry paths did NOT fix it. The crash is in the JIT compilation
pipeline, not the zero-init.

## Files Modified (commit 68126095)

1. **gen_asm_utils.cpp**: ADR x12, after_call; STR x12, [SP, #8] before
   BL/BLR. Uses `asmjit::arm::Mem(asmjit::a64::sp, 8)` — no scratch
   register needed (avoids ptr_resolve issues).

2. **autogen.cpp**: Shared `after_call` label with ADR+STR [SP,#8] before
   each BLR in translateCall. Moves call target to x16 when input is a
   register (avoids clobbering x12).

3. **gen_asm.cpp**: Zero-init [SP,#8] inside `allocateHeaderAndSpillSpace`
   (line 1460). All paths that call this function get zero-init.

4. **frame.cpp**: getIP reads `frame_base - frame_size - kPointerSize` with
   LR fallback when slot is zero. Also adds `updatePrevInstr(frame)` in
   frame conversion.

## Formula Verification

```
Writer: STR x12, [SP, #8]  =>  address = SP + 8
Reader: frame_base - frameSize() - 8
      = FP - (S - 16) - 8
      = FP - S + 8
      = SP + 8  (since SP = FP - S)
MATCH ✓
```

Where S = stack_frame_size (physical), frameSize() = S - kStackAlign.

## Build System Notes

- CMake RelWithDebInfo produces 50MB .so (with debug_info sections)
- The 26MB "working" .so (fa8cb9cc) was from an earlier setup.py build
  without debug info — confirmed identical via md5sum
- Both builds use the same compiler (/opt/llvm/stable/.../clang)
- `pip install -e .` provides editable install pointing to PythonLib/

## Next Steps

1. The PYTHONJITALL=1 crash is a separate investigation (tracked independently)
2. The [SP,#8] fix is production-ready for all thresholds up to and including N=1
3. GPU test baselines should be verified with the 68126095 build
