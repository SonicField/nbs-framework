# LOAD_ATTR_MODULE Inline Access — Progress Log

**Date:** 22 February 2026
**Session goal:** Improve LOAD_ATTR_MODULE specialisation in the CinderX JIT by inlining the module dict access, replacing the `LoadModuleAttrCached` runtime function call with direct field loads.
**Result:** +4.2% speedup on module attribute access. Implementation: ~40 lines across 3 files, built and verified on devgpu009 against Python 3.14.0.

---

## Discovery: Existing Specialisation Has No Impact

Before implementing the inline access, we benchmarked the existing LOAD_ATTR_MODULE specialisation (already present in the base CinderX code). The existing path:

```
Builder:  GuardType(receiver, PyModule_Type)
          LoadAttr(receiver, name_idx)
Simplify: LoadAttr → LoadModuleAttrCached
LIR:      CALL LoadModuleAttrCache::lookupHelper
```

**ABBA benchmark result: -0.9% (noise).** The existing specialisation provides no measurable benefit because:
- Both `LoadModuleAttrCached` and generic `LoadAttrCached` are runtime inline caches with function calls
- Both have similar fast-path costs (identity/type check + version check + cached value return)
- The `GuardType` adds overhead that cancels any marginal benefit

This motivated the inline access approach.

---

## Implementation: Inline Dict Access

### Architecture

The new path reads CPython's inline cache data (`dict_version`, `entry_index`) at JIT compile time and emits inline `LoadField` instructions:

```
Builder:  GuardType(receiver, PyModule_Type)
          LoadField(receiver, md_dict)               ← inline
          LoadField(dict, ma_keys)                   ← inline
          LoadField(keys, dk_version)                ← inline
          Guard(dk_version == cached_version)         ← inline, deopt on mismatch
          CallStatic(JITRT_LoadModuleDictEntry, keys, index)  ← tiny leaf function
          CheckField(value != NULL)                   ← inline, deopt if deleted
          RETURN EARLY — skip LoadAttr entirely
```

The `CallStatic` calls a 3-line helper that computes `DK_UNICODE_ENTRIES(keys)[index].me_value`. This avoids encoding the `DK_UNICODE_ENTRIES` macro's conditionals (which depend on `dk_size` and `DK_IXSIZE`) in HIR, while keeping the version guard fully inline.

### Changes

| File | Changes |
|------|---------|
| `cinderx/Jit/jit_rt.h` | +3: declare `JITRT_LoadModuleDictEntry` |
| `cinderx/Jit/jit_rt.cpp` | +7: define `JITRT_LoadModuleDictEntry` (incl. `#include` and `Py_XNewRef`) |
| `cinderx/Jit/hir/builder.cpp` | +30/-4: replace LOAD_ATTR_MODULE case with inline access + early return |

### Key Bug Fix During Development

Initial implementation returned a **borrowed reference** from `JITRT_LoadModuleDictEntry` (`return ep->me_value`). The JIT expects owned references and decrefs them — this caused a segfault after the refcount reached zero. Fixed by returning `Py_XNewRef(ep->me_value)`.

The smoke tests (100 iterations) passed with the borrowed reference because the module's dict held its own reference. The crash only manifested with larger iteration counts when the refcount underflow eventually caused a use-after-free.

### CPython IC Layout (Python 3.12 and 3.14)

The `_PyAttrCache` for LOAD_ATTR_MODULE stores:
- `version[0]|version[1]` (32-bit) = `dict->ma_keys->dk_version` (dict **keys** version, NOT a type version)
- `index` (16-bit) = entry index in `DK_UNICODE_ENTRIES(dict->ma_keys)`

This is semantically different from LOAD_ATTR_SLOT/INSTANCE_VALUE (which store a **type** version). The `findTypeByVersionTag()` approach does not work for LOAD_ATTR_MODULE — the version is for the dict keys, not the type.

---

## Benchmark Results

### ABBA: Inline access vs generic LoadAttrCached

```
[0] A=186.2ms B=190.4ms B=196.1ms A=183.5ms
[1] A=178.9ms B=185.6ms B=192.8ms A=190.2ms
[2] A=186.2ms B=193.9ms B=216.0ms A=190.9ms
[3] A=190.3ms B=199.5ms B=198.7ms A=194.8ms
[4] A=181.0ms B=188.2ms B=201.1ms A=184.6ms
[5] A=199.2ms B=184.8ms B=199.0ms A=181.8ms

A (inline) avg: 187.3ms
B (generic) avg: 195.5ms
Speedup: +4.2%
Ratio: 1.044x
```

Workload: `m.pi` access in a tight loop (5M iterations, `cinderjit.auto()` + 15000 warmup + `exec()` for fresh code objects per ABBA sample).

---

## Build Environment

Built CinderX from PyPI source tarball (cinderx-2026.2.2.0) on devgpu009.ncg6 (x86_64):

- **Python:** 3.14.0 (GIL-enabled, non-debug, built from `/data/users/alexturner/cpython-314-gil/`)
- **Compiler:** Clang 21.1.7
- **Dependencies:** asmjit, fmt, parallel-hashmap from fbsource third-party; usdt header from fbsource xplat
- **GitHub blocked:** Proxy returns 403 for github.com. Solved by pre-populating cmake FetchContent deps from fbsource and using `FETCHCONTENT_SOURCE_DIR_*` cmake variables.

The devgpu004.prn3 environment (where our aarch64 changes live) was unreachable from devgpu009.ncg6 — DNS resolution fails across regions.

---

## Architectural Insights

### Module dicts are fundamentally different from instance dicts

For LOAD_ATTR_INSTANCE_VALUE (+57.3%), the Simplify pass's `simplifyLoadAttrSplitDict` replaces `LoadAttr` with direct `LoadField` instructions — zero function calls, fully inlined. This works because the **type** determines the split dict layout, and `GuardType` establishes the exact type.

For LOAD_ATTR_MODULE, this approach doesn't work:
1. All modules are `PyModule_Type` — the type doesn't distinguish between modules
2. Module dicts are combined (not split) dicts — `simplifyLoadAttrSplitDict` doesn't apply
3. The entry index comes from the CPython IC, not from the type's dict layout

The inline access approach is the correct solution for modules: use the IC's `dict_version` for a version guard, and the IC's `index` for direct entry access via `DK_UNICODE_ENTRIES`.

### The +4.2% is modest but real

The existing `LoadModuleAttrCached` runtime IC was already efficient (identity check + version check + cached value return). Our inline access eliminates the function call overhead but still calls a helper for `DK_UNICODE_ENTRIES` computation. A fully inline path (computing `DK_UNICODE_ENTRIES` in HIR) would require emitting conditionals for `DK_IXSIZE(dk_size)` — diminishing returns for additional complexity.

### Refcount discipline in JIT helpers

JIT runtime helpers must return **owned references** (via `Py_NewRef`/`Py_XNewRef`). The JIT's refcount insertion pass assumes outputs from `CallStatic` are owned and inserts decrefs accordingly. Returning borrowed references causes refcount underflow — a subtle bug that may not crash immediately.

---

## Summary Table

| Specialisation | ABBA Speedup | Mechanism |
|---------------|-------------|-----------|
| FOR_ITER_RANGE | +25% | GuardType at GET_ITER → CallStatic(JITRT_InvokeIterNext) |
| FOR_ITER_LIST | +23.4% | Same as range |
| FOR_ITER_TUPLE | +25.7% | Same as range |
| LOAD_ATTR_INSTANCE_VALUE | +57.3% | GuardType on receiver → simplifyLoadAttrSplitDict |
| LOAD_ATTR_MODULE | +4.2% | Inline dk_version guard → CallStatic(JITRT_LoadModuleDictEntry) |
| STORE_ATTR_INSTANCE_VALUE | (type propagation) | GuardType on receiver → downstream type narrowing |
| STORE_ATTR_SLOT | (type propagation) | Same as STORE_ATTR_INSTANCE_VALUE |
