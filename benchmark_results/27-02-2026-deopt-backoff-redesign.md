# Deopt Backoff Redesign: CI_CO_SUPPRESS_JIT via Context::recordDeopt()

**Date:** 27 February 2026
**Author:** theologian
**Status:** COMMITTED (105ee2c6) — v5 deoptBackoffSuppressFunctions — 1,240,000 → 0 deopts
**Bug:** Bug 10 (deopt backoff code never executes)

---

## Problem Statement

The deopt/reopt loop produces 1.1M deopts in deep_class_super, 50K in decorator_chain, 50K in pytorch_cm, and 40K in nn_module_forward. The loop:

```
1. JIT compiles method with GuardType checks
2. New function object created → scheduleJitCompile → reoptFunc
3. reoptFunc re-attaches SAME compiled code
4. JIT code runs → guard fails → gen_asm deopt stub
5. Context::recordDeopt(CodeRuntime*, deopt_idx) → interpreter fallback
6. Next call → goto step 2
```

The previous backoff patch placed counting in `deoptFunc()`/`reoptFunc()` (pyjit.cpp). This was wrong. Runtime guard failures go through `Context::recordDeopt()` (context.cpp), not `deoptFunc()`. The patch was dead code.

---

## Architecture: The Two Deopt Paths

### Path A: Explicit Deopt (RARE — not the problem)

```
Type modification → PyType_Modified → cinderx_type_watcher
  → Context::notifyTypeModified → TypeDeoptPatcher::maybePatch
  → deoptFuncImpl (pyjit.cpp:1289) → func->vectorcall = interpreted
```

Triggered by class attribute changes, code object replacement. One-time events.

### Path B: Runtime Guard Failure (COMMON — the problem)

```
JIT code runs → GuardType check fails
  → gen_asm.cpp deopt stub (line ~230-246)
  → Context::recordDeopt(CodeRuntime* runtime, size_t deopt_idx)
  → interpreter takes over for remainder of call
```

Triggered on every call where the JIT guard doesn't match. This is the 1.1M-deopt path.

### The Connection Point: CodeRuntime → PyCodeObject

Verified on aarch64 dev server (code_runtime.h):

```
CodeRuntime
  └── frame_state_ (RuntimeFrameState)
       └── code_ (BorrowedRef<PyCodeObject>)
```

Access: `code_runtime->frameState()->code()` returns `BorrowedRef<PyCodeObject>`.

This is the key discovery: we CAN navigate from CodeRuntime to the PyCodeObject, which means we can set `CI_CO_SUPPRESS_JIT` directly.

---

## Key Discovery: CI_CO_SUPPRESS_JIT Already Integrated

Verified on aarch64 dev server (grep results):

```
py-portability.h:209:  #define CI_CO_SUPPRESS_JIT 0x40000000
pyjit.cpp:798:   if (code->co_flags & CI_CO_SUPPRESS_JIT)    // in reoptFunc()
pyjit.cpp:3892:  if (code->co_flags & CI_CO_SUPPRESS_JIT)    // in scheduleJitCompile()
pyjit.cpp:2414:  code->co_flags |= CI_CO_SUPPRESS_JIT;       // suppress API
pyjit.cpp:2429:  code->co_flags &= ~CI_CO_SUPPRESS_JIT;      // unsuppress API
```

**`reoptFunc()` already checks CI_CO_SUPPRESS_JIT (line 798).** If the flag is set on a code object, `reoptFunc` returns false — the function stays interpreted. `scheduleJitCompile()` also checks it (line 3892), preventing new compilations.

This means: **zero changes to pyjit.cpp are needed.** We just set the flag from `recordDeopt()` when the threshold is hit.

---

## Final Design: v5 Two-Step Vectorcall Reset + Lazy CI_CO_SUPPRESS_JIT (3 files, ~70 lines)

### Evolution: v1 → v3 → v4 → v5

- **v1** set CI_CO_SUPPRESS_JIT in `recordDeopt()` (mid-deopt). Caused decorator_chain segfault (H1: mid-execution co_flags mutation).
- **v3** placed lazy check in `lookupFunc` branch of `reoptFunc()`. Wrong branch — `didCompile` branch returns `true` first. Unchanged deopts.
- **v4** added backoff check to `didCompile` branch AND `lookupFunc` branch. Fixed nn_module_forward (40K→0) but not module-level functions (reoptFunc never re-entered — Path C).
- **v5** adds `deoptBackoffSuppressFunctions()` in `recordDeopt()`: when threshold hit, iterates `compiled_funcs_`, resets vectorcall on matching functions. Combined with v4's lazy CI_CO_SUPPRESS_JIT in reoptFunc, all three deopt paths are covered.

### context.h Changes

```cpp
// In Context class, near deopt_stats_ declaration:

static constexpr uint32_t kDeoptBackoffThreshold = 100;
UnorderedMap<const CodeRuntime*, uint32_t> deopt_backoff_counts_;

// Check if a CodeRuntime has exceeded the deopt backoff threshold.
// Called from reoptFunc() to decide whether to set CI_CO_SUPPRESS_JIT
// and refuse re-attachment.
bool isDeoptBackoffTriggered(const CodeRuntime* runtime) const {
    auto it = deopt_backoff_counts_.find(runtime);
    return it != deopt_backoff_counts_.end() &&
           it->second >= kDeoptBackoffThreshold;
}
```

### context.cpp Changes

```cpp
void Context::recordDeopt(
    CodeRuntime* code_runtime,
    std::size_t idx,
    BorrowedRef<> guilty_value) {
  DeoptStat& stat = deopt_stats_[code_runtime][idx];
  stat.count++;
  if (guilty_value != nullptr) {
    stat.types.recordType(Py_TYPE(guilty_value));
  }

  // Deopt backoff: count guard failures. When threshold is reached,
  // reset vectorcall on all function objects using this CodeRuntime.
  auto& count = deopt_backoff_counts_[code_runtime];
  count++;
  if (count == kDeoptBackoffThreshold) {
    BorrowedRef<PyCodeObject> code = code_runtime->frameState()->code();
    JIT_LOG(
        "Deopt backoff: {} reached {} guard failures, suppressing",
        PyUnicode_AsUTF8(code->co_qualname),
        count);
    deoptBackoffSuppressFunctions(code_runtime);
  }
}

void Context::deoptBackoffSuppressFunctions(CodeRuntime* code_runtime) {
  // Collect functions using this CodeRuntime. Must collect first because
  // removeCompiledFunc modifies compiled_funcs_ (iterator invalidation).
  std::vector<BorrowedRef<PyFunctionObject>> to_deopt;
  for (auto func : compiled_funcs_) {
    if (CompiledFunction* compiled = lookupFunc(func)) {
      if (compiled->runtime() == code_runtime) {
        to_deopt.push_back(func);
      }
    }
  }

  // Deopt each function: remove from compiled set, reset vectorcall to
  // interpreter entry, mark as deopted. Safe mid-deopt because vectorcall
  // is a pointer on PyFunctionObject (not co_flags on PyCodeObject).
  for (auto func : to_deopt) {
    JIT_LOG(
        "Deopt backoff: detaching JIT from {}",
        PyUnicode_AsUTF8(func->func_qualname));
    removeCompiledFunc(func);
    func->vectorcall = getInterpretedVectorcall(func.get());
    addDeoptedFunc(func);
  }
}
```

### pyjit.cpp Changes (v4 — didCompile branch)

reoptFunc() has TWO reopt paths:
- **didCompile branch** (line 789): handles functions that were compiled and then deopted. This is the deopt/reopt loop path — the one that fires 1.1M times.
- **lookupFunc branch** (line 819): handles nested functions sharing a code object. Not the deopt loop path.

v3 placed the check in the lookupFunc branch only. v4 adds it to the didCompile branch (the actual deopt loop path):

```cpp
// In reoptFunc(), in the didCompile branch (PRIMARY deopt loop path):
} else if (jitCtx()->didCompile(func)) {
    // Deopt backoff: before re-attaching JIT code, check if this function's
    // CodeRuntime has exceeded the guard failure threshold.
    if (CompiledFunction* compiled = jitCtx()->lookupFunc(func)) {
      CodeRuntime* rt = compiled->runtime();
      if (jitCtx()->isDeoptBackoffTriggered(rt)) {
        BorrowedRef<PyCodeObject> code{func->func_code};
        code->co_flags |= CI_CO_SUPPRESS_JIT;
        return false;
      }
    }
    jitCtx()->removeDeoptedFunc(func);
    return true;
}

// In reoptFunc(), in the lookupFunc branch (fallback for nested functions):
if (CompiledFunction* compiled = jitCtx()->lookupFunc(func)) {
    CodeRuntime* rt = compiled->runtime();
    if (jitCtx()->isDeoptBackoffTriggered(rt)) {
      code->co_flags |= CI_CO_SUPPRESS_JIT;
      return false;
    }
    jitCtx()->finalizeFunc(func, *compiled);
    return true;
}
```

---

## Design Decisions and Rationale

### Why CI_CO_SUPPRESS_JIT (not a custom map + custom checks)

The original design proposed a custom `deopt_backoff_` map with a `suppressed` flag, plus a custom `isDeoptSuppressed()` query in `reoptFunc()`. After reading the source code on the aarch64 dev server, I found that CI_CO_SUPPRESS_JIT already does exactly this:

| Feature | Custom map approach | CI_CO_SUPPRESS_JIT approach |
|---------|--------------------|-----------------------------|
| Files modified | 3 (context.h, context.cpp, pyjit.cpp) | 2 (context.h, context.cpp) |
| New API surface | `isDeoptSuppressed()` method | None (uses existing flag) |
| reoptFunc changes | Add lookup + check | None (check already exists) |
| scheduleJitCompile changes | None (gap) | None (check already exists) |
| Cleanup on decompile | Must erase map entry | Not needed (flag is on code object) |
| Use-after-free risk | Map keyed by pointer | Counter map only (flag persists on code object) |

The CI_CO_SUPPRESS_JIT approach is strictly better.

### Why the Counter Map is Still Needed

`deopt_stats_` tracks per-deopt-site statistics and is cleared by `cinderjit.get_and_clear_runtime_stats()` (used by the benchmark harness between measurements). The backoff counter must survive stat clearing, so it lives in a separate map.

### Memory Management

`CodeRuntime` objects live in `Context::code_runtimes_` (a `SlabArena<CodeRuntime>`). SlabArena does not free individual elements — it allocates from slabs and frees everything when the arena is destroyed (i.e., when Context is destroyed). Therefore:

- CodeRuntime pointers are stable for the lifetime of the Context
- No use-after-free risk for the counter map
- Stale entries are impossible (pointers never become invalid individually)
- The counter map grows monotonically but is bounded by total compiled functions

No explicit cleanup is needed. Testkeeper's concern about use-after-free (00:04:33Z) is resolved by SlabArena semantics.

### Thread Safety

`recordDeopt()` is called from JIT-generated deopt stubs under the GIL. `deopt_stats_` already uses the same access pattern (unsynchronised map operations under GIL). `deopt_backoff_counts_` follows the same pattern. Comment in context.h notes the GIL dependency.

### Threshold Selection

- **kDeoptBackoffThreshold = 100**: Tolerates warmup deopts (importlib functions deopt ~50 times during module loading with compile_after_n_calls=0). Catches deopt loops (deep_class_super: 220K deopts per call → suppressed after 100, 99.95% reduction).
- No exponential backoff needed: once CI_CO_SUPPRESS_JIT is set, reoptFunc and scheduleJitCompile both refuse to re-JIT. The suppression is permanent unless explicitly cleared (pyjit.cpp:2429).

### Why Permanent Suppression is Correct

1. Functions that deopt repeatedly are FASTER interpreted (deep_class_super: 0.52x with JIT → ~1.0x interpreted)
2. The inner-class pattern creates NEW function objects each call — there is no cooldown period where guards might start passing
3. CI_CO_SUPPRESS_JIT can be cleared via `cinderjit.unsuppress_jit(code)` (pyjit.cpp:2429) if needed for testing
4. If we later need adaptive un-suppression (e.g., after type stabilisation), we can add a call-count mechanism to clear the flag — but this is not needed for the current use case

---

## Falsification Criteria

### The design is CORRECT if:

1. After applying the patch, deep_class_super produces ≤400 deopts (4 code objects × threshold 100) instead of 1,100,000
2. The 22 structural benchmarks (0 deopts) are unaffected
3. No crashes, including decorator_chain (no struct changes = no heap layout change)
4. JIT/Vanilla ratio for deep_class_super improves toward 1.0x (interpreted speed) from 0.52x
5. nn_module_forward, pytorch_cm, decorator_chain also show reduced deopts

### The design is WRONG if:

1. Deopts are unchanged → recordDeopt is not in the guard failure path (our path analysis is wrong)
2. Threshold too low → suppresses functions that would have stabilised after warmup (JIT wins become losses)
3. Counter map introduces measurable overhead on the hot path (each deopt does one map insert/lookup)
4. CI_CO_SUPPRESS_JIT causes cascading effects — other code paths check this flag and do unexpected things

### Test Protocol:

1. Clean build on aarch64 dev server: `rm -rf build/ && python3 setup.py build_ext --inplace`
2. Run `deopt_stats_measure.py` with all 26 benchmarks
3. Compare deopt counts: must be ≤ threshold × number_of_code_objects for each deopt-caused benchmark
4. Verify 22 structural benchmarks have 0 deopts (unchanged)
5. Run ABBA benchmark for timing comparison (deep_class_super should improve)
6. Run decorator_chain explicitly to verify no crash

---

## Files Modified

| File | Change | Lines |
|------|--------|-------|
| `cinderx/Jit/context.h` | Add `kDeoptBackoffThreshold`, `deopt_backoff_counts_` map, `isDeoptBackoffTriggered()` method, `deoptBackoffSuppressFunctions()` declaration | +28 |
| `cinderx/Jit/context.cpp` | Counter logic in `recordDeopt()` + `deoptBackoffSuppressFunctions()` vectorcall reset | +40 |
| `cinderx/Jit/pyjit.cpp` | Lazy CI_CO_SUPPRESS_JIT in `reoptFunc()` didCompile branch + lookupFunc branch | +18 |

**Total: 3 files, ~86 lines of new code. Two-step suppression: vectorcall reset in recordDeopt (Path C) + lazy CI_CO_SUPPRESS_JIT in reoptFunc (Paths A+B).**

---

## Comparison: All Versions

| Aspect | Previous patch | v1 (recordDeopt) | v3 (lazy lookupFunc) | v4 (lazy didCompile) | v5 (vectorcall reset) |
|--------|---------------|------------------|---------------------|---------------------|----------------------|
| Fix site | deoptFunc/reoptFunc | recordDeopt | recordDeopt + reoptFunc | recordDeopt + reoptFunc | recordDeopt + reoptFunc |
| Code path | Explicit deopt (rare) | Guard failure (correct) | Guard failure (correct) | Guard failure (correct) | Guard failure (correct) |
| Path C coverage | No | No (co_flags only) | No | No (reoptFunc never called) | **Yes (vectorcall reset)** |
| Struct changes | +8 bytes | None | None | None | None |
| co_flags mutation | N/A | Mid-deopt (CRASH) | Between calls (safe) | Between calls (safe) | None in recordDeopt |
| decorator_chain | Crash | Crash | No crash (no effect) | No crash (no effect) | **No crash, 0 deopts** |
| Deopt reduction | None (dead code) | 40K→0 (1 bench) | None (wrong branch) | 40K→0 (1 bench) | **1,240,000→0 (all)** |
| Uses existing infra | No | Yes | Yes | Yes | Yes |

---

## v4 Benchmark Results and the Third Deopt Path

**Date:** 27 February 2026, 01:14Z
**Build:** v4 on aarch64 dev server, threshold=100, cinderjit.auto()

### Results

| Benchmark | Before | After v4 | Status |
|-----------|--------|----------|--------|
| nn_module_forward | 40,000 | 0 | FIXED |
| deep_class_super | 1,100,000 | 1,100,000 | UNCHANGED |
| decorator_chain | 50,000 | 50,000 | UNCHANGED |
| pytorch_cm | 50,000 | 50,000 | UNCHANGED |
| 22 structural | 0 | 0 | UNAFFECTED |
| decorator_chain crash | — | NO CRASH | SAFE |

v4 fixes nn_module_forward (inner-class methods → new function objects → reoptFunc called → backoff check fires). The remaining 3 benchmarks are unaffected.

### Root Cause: The Persistent Vectorcall Path (Path C)

v4 covers two paths through reoptFunc():
- **Path A (didCompile):** Same function object re-called after deopt → didCompile=true → v4 backoff check
- **Path B (lookupFunc):** New function object sharing code object → didCompile=false → lookupFunc → v3 backoff check

But there is a **third path that bypasses reoptFunc entirely:**

```
Path C: Persistent vectorcall (module-level functions)

1. Function object created at module import → scheduleJitCompile → compiles
2. vectorcall = JIT entry point (set during compilation)
3. Call → JIT entry → guard fails → deopt stub → recordDeopt → interpreter (this call)
4. Next call → vectorcall STILL = JIT entry → goto step 3
```

reoptFunc is only called via scheduleJitCompile, which fires on function creation (a one-time event for module-level functions). After initial compilation, nothing re-enters reoptFunc for the same function object. The vectorcall pointer remains as the JIT entry forever. Deopt only affects the current call (interpreter fallback), not future calls.

This is why:
- **nn_module_forward works:** Inner-class methods create NEW function objects each call → scheduleJitCompile → reoptFunc → backoff check
- **deep_class_super, decorator_chain, pytorch_cm unchanged:** Module-level functions with persistent function objects → vectorcall stays as JIT entry → reoptFunc never re-entered

### Verification

19 JIT_LOG messages confirm the counter reaches threshold 100 and backoff triggers. The counter mechanism works. The flag mechanism works (for Path B). Path C is simply not intercepted by any check.

### v5 Fix: deoptBackoffSuppressFunctions (IMPLEMENTED — VERIFIED)

**Option A selected:** In recordDeopt, when threshold is hit, iterate `compiled_funcs_` to find PyFunctionObjects matching the CodeRuntime's PyCodeObject. For each match: `removeCompiledFunc` + reset vectorcall to `_PyFunction_Vectorcall` (interpreted entry) + `addDeoptedFunc`.

Key properties:
- `removeCompiledFunc` only erases from `compiled_funcs_` (per-func-object set), NOT `compiled_codes_` (per-code-object map). Verified by generalist (context.cpp:659).
- vectorcall reset is a pointer swap on PyFunctionObject (safe mid-deopt, unlike co_flags on PyCodeObject which crashed in v1)
- O(n) iteration over compiled_funcs_, but only runs ONCE per CodeRuntime at threshold
- v4 checks in reoptFunc (didCompile + lookupFunc branches) remain as defence-in-depth

### v5 Benchmark Results (PASS — ALL CRITERIA MET)

**Date:** 27 February 2026, 01:25Z
**Build:** v5 on aarch64 dev server, threshold=100, cinderjit.auto()

| Benchmark | Before | After v5 | Change |
|-----------|--------|----------|--------|
| deep_class_super | 1,100,000 deopts, 140ms | 0 deopts, 85ms | -100% deopts, +40% speed |
| decorator_chain | 50,000 deopts, 20.5ms | 0 deopts, 17.5ms | -100% deopts, +15% speed |
| pytorch_cm | 50,000 deopts | 0 deopts | -100% deopts |
| nn_module_forward | 40,000 deopts | 0 deopts | -100% deopts |
| 22 structural | 0 deopts | 0 deopts | Unchanged |

**Total deopt reduction: 1,240,000 → 0 (100% elimination).**

Gate criteria:
1. decorator_chain NO CRASH: PASS
2. deep_class_super ≤400 deopts: PASS (0 — exceeds by 400)
3. 22 structural 0 deopts: PASS

**GATE APPROVED for commit.**
