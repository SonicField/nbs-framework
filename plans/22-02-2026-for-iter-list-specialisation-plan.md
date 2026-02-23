# Plan: FOR_ITER_LIST JIT Specialisation

**Author:** claude (implementer)
**Date:** 22 February 2026
**Status:** IN PROGRESS
**Prerequisite:** FOR_ITER_RANGE specialisation (commit 4ff0cfe0)

---

## Goal

Add specialised HIR generation for `FOR_ITER_LIST` in the CinderX JIT, mirroring the FOR_ITER_RANGE pattern. Emit `GuardType` at GET_ITER to establish the iterator as `list_iterator`, enabling the Simplify pass to replace generic `InvokeIterNext` with `CallStatic(JITRT_InvokeIterNext)`.

## Falsifiable Success Criterion

A tight `for x in some_list` loop must show >2% speedup (ABBA methodology) with the specialisation enabled.

## Files Modified

| File | Change |
|------|--------|
| `cinderx/Jit/iterator_types.h` | Add `g_list_iterator_type` declaration |
| `cinderx/Jit/iterator_types.cpp` | Add global + init code (after range init) |
| `cinderx/Jit/bytecode.cpp` | Add `FOR_ITER_LIST` to `specializedOpcode()` |
| `cinderx/Jit/hir/builder.cpp` | Extend emitGetIter lookahead for FOR_ITER_LIST |
| `cinderx/Jit/hir/simplify.cpp` | Extend InvokeIterNext condition for list_iterator |

See approved plan file for full details: `.claude/plans/twinkling-wiggling-crown.md`
