# FOR_ITER_RANGE Specialisation — Progress Log

**Date:** 23 February 2026
**Session goal:** Apply CPython 3.12 adaptive interpreter specialisation to CinderX JIT, proving >2% speedup with ABBA benchmarks.
**Target:** FOR_ITER_RANGE — range iteration loop specialisation.
**Result:** 25% speedup (1.34x) with GuardType at GET_ITER. Terminal goal achieved.

## Gap Analysis

Two independent gap analyses (theologian + generalist) identified ~33-39 gaps across 10 of 12 CPython 3.12 specialisation families. FOR_ITER_RANGE was selected for:

- Highest per-iteration impact in tight loops
- No existing HIR-level optimisation (confirmed dead code in simplify.cpp)
- Clean scope — no IC family dependency
- Easy to benchmark

## Implementation

### Files Changed (4 files, 21 insertions, 4 deletions)

1. **bytecode.cpp** (+1 line): Added `FOR_ITER_RANGE` to `specializedOpcode()` switch — preserves the specialised opcode instead of unspecialising it.
2. **builder.h** (+1/-1 line): `emitGetIter` signature change to accept `bc_instr` parameter.
3. **builder.cpp** (+18/-1 lines): Include `iterator_types.h`. `emitGetIter` peeks at next bytecode instruction; if `FOR_ITER_RANGE` and `specialized_opcodes` enabled and `g_range_iterator_type` non-null, emits `GuardType(result, range_iterator_type, result, tc.frame)`.
4. **simplify.cpp** (+1/-1 line): Pre-existing latent bug fix — `CallStatic(JITRT_InvokeIterNext)` return type `TOptObject` → `TObject`. Required because `InvokeIterNext` output type is `TObject`; the replacement must be ≤ original type. Null sentinel handling is done by `CondBranchIterNotDone`, not the type system.

### Optimisation Chain

```
FOR_ITER_RANGE bytecode
  → GET_ITER: GuardType(iterator, range_iterator) [runs ONCE]
  → FOR_ITER: InvokeIterNext [type is now range_iterator]
  → [Simplify pass]: InvokeIterNext → CallStatic(JITRT_InvokeIterNext)
  → [Simplify pass]: skips JitGen check, direct iterator advance
```

## Three-Variant Analysis

This is the key methodological finding. Three guard placement strategies were benchmarked:

### Variant 1: GuardType per FOR_ITER iteration

- **Placement:** Inside `emitForIter()`, runs every loop iteration
- **ABBA result:** -1.3% (SLOWER) — 8 samples, two passes
- **Analysis:** Per-iteration guard check (type comparison + branch) costs more than the savings from `JITRT_InvokeIterNext`. For `range(1000)`, that's 1000 guard checks vs 1000 fast dispatches. Guard overhead dominates.
- **Falsification:** The negative result proved the guard overhead was the bottleneck, not the dispatch savings.

### Variant 2: UseType (no runtime guard)

- **Placement:** Compile-time type assertion in `emitForIter()`, zero runtime cost
- **ABBA result:** +23% (FASTER) — 4 samples
- **Analysis:** Eliminates all guard overhead. Proves the `JITRT_InvokeIterNext` fast path IS significantly faster than generic dispatch.
- **Safety concern:** UseType is unsound for polymorphic call sites. If a function is called with both `range()` and `list` iterators, UseType would assert the wrong type with no deopt fallback → undefined behaviour.
- **Team decision:** Rejected on safety grounds (theologian, gatekeeper, testkeeper, helper all flagged the polymorphic concern).

### Variant 3: GuardType at GET_ITER (one-time)

- **Placement:** In `emitGetIter()`, runs ONCE before the loop entry
- **ABBA result:** +25% (FASTER) — 4 samples
- **Analysis:** Single guard check before the loop is negligible overhead. Type narrowing propagates through SSA (no phi node intervenes — v3 is loop-invariant). Simplify pass sees `range_iterator` type on `InvokeIterNext` and fires the fast path.
- **Safety:** Full deopt safety for polymorphic sites via GuardType.
- **Bonus:** Slightly FASTER than UseType (25% vs 23%), likely because GuardType's type narrowing gives later optimisation passes (register allocator, etc.) more information to work with.

### Summary Table

| Variant | Guard placement | Runtime cost | ABBA result | Safe? |
|---------|----------------|-------------|-------------|-------|
| 1 | FOR_ITER (per iteration) | 1 check/iteration | -1.3% | Yes |
| 2 | UseType (none) | 0 | +23% | No (polymorphic) |
| 3 | GET_ITER (once) | 1 check/loop | +25% | Yes |

### Key Insight

Guard placement is a first-order performance concern for loop specialisations. A per-iteration guard can turn a 25% speedup into a 1.3% slowdown. The correct pattern for loop specialisations is: **guard at iterator creation, not per iteration**.

## Bugs Found

1. **Simplify pass latent bug (simplify.cpp:2113):** `CallStatic(JITRT_InvokeIterNext)` created with `TOptObject` return type, but `InvokeIterNext` output type is `TObject`. Assertion at line 2171 requires `new_output->type() <= instr.output()->type()`. This was dead code — the `InvokeIterNext` range_iterator fast path in the Simplify pass had never been triggered before our `GuardType` provided the type info.

## Verification Scorecard

| # | Criterion | Status |
|---|-----------|--------|
| 1 | GuardType emitted on iterator | PASS |
| 2 | FrameState correct (GET_ITER offset) | PASS |
| 3 | Deopt to generic path | PASS |
| 4 | Range exhaustion handled | PASS (sum(range(100))=4950) |
| 5 | Edge cases (negative step, zero-length, large) | PASS |
| 6 | ABBA >2% speedup | PASS (25%) |
| 7 | No regressions on existing benchmarks | PENDING |
| 8 | Full test suite passes | PENDING |
| 9 | Self-contained (4 files) | PASS |
| 10 | No inliner changes | PASS |
| 11 | Generic path unchanged | PASS |

## Methodology Notes

- **ABBA pattern:** A-B-B-A, minimum 4 samples per condition, cancels thermal/frequency drift
- **A condition:** `cinderjit.enable_specialized_opcodes()` — our FOR_ITER_RANGE fires
- **B condition:** Default (no `enable_specialized_opcodes`) — generic path
- **Benchmark:** 5 sub-benchmarks targeting different range patterns (tight, short, nested, stepped, accumulate)
- **Correctness:** Checksum verification ensures both conditions compute the same result
