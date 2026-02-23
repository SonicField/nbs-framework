# FOR_ITER_LIST Specialisation — Progress Log

**Date:** 22 February 2026
**Session goal:** Apply FOR_ITER_LIST specialisation to CinderX JIT, proving >2% speedup with ABBA.
**Result:** 23.4% speedup (1.23x) with GuardType at GET_ITER. Terminal goal achieved.

## Implementation Progress

### Step 1: iterator_types.h/.cpp — DONE
- Added `extern PyTypeObject* g_list_iterator_type;` to header
- Added `g_list_iterator_type = nullptr;` global
- Added init code after range init: creates empty list, gets iterator, captures type
- Restructured range init to use nested-if instead of early-return, so list init always runs

### Step 2: bytecode.cpp — DONE
- Added `case FOR_ITER_LIST:` after `case FOR_ITER_RANGE:` in `specializedOpcode()` switch

### Step 3: builder.cpp — DONE
- Restructured emitGetIter lookahead block: shared `specialized_opcodes` check, separate cases for FOR_ITER_RANGE and FOR_ITER_LIST
- Each case emits GuardType with the appropriate iterator type

### Step 4: simplify.cpp — DONE
- Extended `kInvokeIterNext` condition to check for both `g_range_iterator_type` and `g_list_iterator_type`
- `iterator_types.h` include already present from FOR_ITER_RANGE work

### Build — DONE
- 100% built, no errors (pre-existing warnings only)
- `_cinderx.so` at `cinderx/PythonLib/_cinderx.so` (51MB)

### Verification — DONE

#### Smoke tests: 5/5 PASS
1. basic_sum (sum of list(range(100)) = 4950): PASS
2. empty_list (0 iterations): PASS
3. large_list (100k elements): PASS
4. nested_list_range (list + range nesting): PASS
5. list_of_strings (string iteration): PASS

#### Range regression: 4/4 PASS
- range_sum, range_neg_step, range_empty, range_large: all PASS
- No regression from simplify.cpp condition extension

#### ABBA benchmark: +23.4% PASS

| Sample | Time (ms) | Condition |
|--------|-----------|-----------|
| A1 | 2.837 | specialised |
| B1 | 3.453 | generic |
| B2 | 3.498 | generic |
| A2 | 2.878 | specialised |
| A3 | 2.863 | specialised |
| B3 | 3.433 | generic |
| B4 | 3.744 | generic |
| A4 | 2.875 | specialised |

- A avg: 2.863ms, B avg: 3.532ms
- Ratio B/A: 1.234x
- Speedup: +23.4%
- All samples JIT-compiled (jit=True)
- Checksums match (901683)

## Benchmark Methodology Notes

Five benchmark iterations were needed to get a valid ABBA measurement:

1. **v1**: Functions not JIT-compiled (200 warmup insufficient). Result: noise.
2. **v2**: Fresh closures via make_bench, still not compiled. Result: noise.
3. **v3**: exec() for fresh code objects, still not compiled. Result: noise.
4. **v4**: force_compile() — returns True but is_jit_compiled returns False. Result: noise.
5. **v5**: 15000 warmup calls but with full 200-iteration benchmark per call — timed out (3B iterations).
6. **v6 (final)**: `cinderjit.auto()` + 15000 light warmup calls (1 iteration each) + exec() for fresh code objects. Functions compiled (`jit=True`). Result: +23.4%.

Key lesson: CinderX JIT requires `cinderjit.auto()` mode and ~10000+ function calls to trigger compilation. `force_compile()` returns True but doesn't patch the function's vectorcall. Warmup must be cheap (small iteration count) but frequent (many calls).

## Files Modified (5 files, ~21 insertions, ~7 deletions)

1. `cinderx/Jit/iterator_types.h` (+1 line)
2. `cinderx/Jit/iterator_types.cpp` (+12/-8 lines)
3. `cinderx/Jit/bytecode.cpp` (+1 line)
4. `cinderx/Jit/hir/builder.cpp` (+8/-5 lines)
5. `cinderx/Jit/hir/simplify.cpp` (+3/-2 lines)
