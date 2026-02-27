# Open Items — 27 February 2026

**Last updated:** 27 Feb 2026, 04:15 UTC
**Maintained by:** scribe

---

## Active Bugs

### Bug 8: cinderx.init() Crashes on aarch64 (DEFERRED)

**Severity:** Blocks all cinderx.init()-dependent functionality on aarch64.
**Platform:** aarch64 dev server (Grace CPU), internal Python 3.12 build.
**Status:** DEFERRED — per Alex's directive (26 Feb 23:24:59Z): "no one ever claimed cinderx.init() did work on ARM64. This is just more code which needs fixing."

**Symptom:** SIGSEGV when calling `cinderx.init()` on aarch64, regardless of call context (module-scope import, top-level, function body). Crash occurs during `os.environ.get()` or nearby code inside `init()`.

**Investigation history (26 Feb archive):**
- Team (theologian, generalist, testkeeper, supervisor) traced the cinderx JIT deopt pipeline.
- Generalist narrowed: bare `import _cinderx` works; `os.environ.get()` after bare import works; `cinderx.init()` (which calls `maybe_enable_parallel_gc()` → `os.environ.get()`) crashes.
- f_globals=0xa corruption found in `resumeInInterpreter` (gen_asm.cpp:397). Frame conversion suspected but not confirmed.
- `CINDERX_DISABLE=1` (skips init()) prevents crash.

**Workaround:** Do not call `cinderx.init()`. Use `cinderjit.auto()` or `cinderjit.force_compile()` directly — JIT compilation works without `init()`.

**Next step:** GDB stepping through `init()` to identify exact crash point.

**References:**
- `benchmark_results/21-02-2026-cinderx-bugs-found.md` (Bug 8 section)
- 26 Feb archive: `live-20260226-095058-archive.chat`

---

### Pre-existing spec_from_loader JIT Bug (UNASSIGNED)

**Severity:** Blocks any use of `compile_after_n_calls=0` — importlib functions get JIT-compiled at startup and fail.
**Platform:** aarch64 dev server, internal Python 3.12 build (may also affect x86_64).
**Status:** UNASSIGNED — confirmed pre-existing, not caused by any team patches.

**Symptom:** `TypeError` from `spec_from_loader()` when using `cinderjit.compile_after_n_calls(0)`.

**Root cause (confirmed, supervisor 27 Feb 00:53:03Z):** With `compile_after_n_calls=0`, importlib functions (`_find_and_load`, `_ModuleLockManager.__enter__`, etc.) get JIT-compiled at startup. They deopt during normal module loading, then subsequent imports fail. Confirmed on baseline (git stash + clean rebuild) — not caused by backoff patches.

**Additional finding (supervisor 27 Feb 01:06:22Z):** `PYTHONPATH` containing PythonLib causes `_cinderx.so` to load during Python init, which also triggers the bug via a different path.

**Workaround:** Use `cinderjit.auto()` (sets threshold=1000 before imports) or `compile_after_n_calls` with threshold ≥ 100.

**Next step:** Investigate why JIT-compiled importlib functions fail after deopt. Likely a frame state or code object corruption issue in the import machinery path.

**References:**
- `benchmark_results/21-02-2026-cinderx-bugs-found.md` (Bug 10 discussion)
- live.chat messages: supervisor 00:39:30Z, 00:53:03Z, 01:06:22Z (27 Feb)

---

## Completed Work (This Session, 26–27 Feb)

| Item | Status | Commit | Details |
|------|--------|--------|---------|
| Bug 10: deopt backoff dead code | COMMITTED | 105ee2c6 | 1,240,000 → 0 deopts. v5 CI_CO_SUPPRESS_JIT via recordDeopt(). |
| Bug 9: decorator_chain struct crash | RESOLVED | — | Avoided by Bug 10 fix (no struct changes). Latent memory bug remains. |
| Sidecar MAX_LINE buffer overflow | FIXED | c49cdfb | Replaced fgets with getline() in chat_client.c. 27/27 tests pass. |
| Benchmark documentation | UPDATED | 016cd42 | Bugs 9-10 writeup, decorator_chain root cause, lazy-flag history. |

---

## Completed Work (Earlier Sessions)

| Item | Status | Commit | Session |
|------|--------|--------|---------|
| Bug 1: super().__init__() dispatch corruption | FIXED | d23c1e53 | 21 Feb |
| Bug 2: LICM GuardType hoisting SEGFAULT | FIXED | f44f531d | 21 Feb |
| Bug 3: inliner exception frame crash | FIXED | 23c868ac | 21 Feb |
| Bug 4: tight-loop type mutation | FIXED | d23c1e53 | 21 Feb |
| Bug 5: varint endianness mismatch | FIXED | 0974344a | 21 Feb |
| Bug 6: shutdown SIGSEGV in notifyTypeModified | FIXED | b59e322c | 21 Feb |
| Bug 7: setup.py os.makedirs creates .so as directory | FIXED | — | 22 Feb |
| Callee resolution Stage 1 + 1.5 | COMPLETE | — | 25 Feb |
| Phase 2 benchmark sweep (ABBA methodology) | COMPLETE | — | 24 Feb |

---

## Stalled/Deferred Investigations

### Callee Resolution Stage 2 (NOT STARTED)

**Context:** Stage 1.5 closed 39% of the context_manager performance gap (0.82x → 0.891x) by eliminating branch-miss penalty. The remaining 11% regression is structural — instruction overhead from BEFORE_WITH opcode decomposition.

**Possible approaches:**
1. `__enter__` inlining for trivial context managers
2. BEFORE_WITH fusion into a single HIR op
3. Dead code elimination of LoadAttrSpecial when result is statically known

**References:**
- `25-02-2026-callee-resolution-progress.md` (Stage 1.5 Final Status)
- `25-02-2026-cinderx-jit-diagnostic-report.md` (Strategic Recommendations)

### Generator Optimisation (NOT STARTED)

**Context:** gen_simple (0.67x) and gen_nested (0.61x) are the worst JIT regressions. Root cause: `InvokeIterNext` + L2 cache misses from JitGenFreeList arena (1 MiB). No existing step in next-jit-steps.md covers this.

**References:**
- `25-02-2026-cinderx-jit-diagnostic-report.md` (Root Cause Map, Category B)

### Bug 9 Latent Memory Bug (NOT INVESTIGATED)

**Context:** Adding 8 bytes to `CompiledFunctionData` triggers a crash in decorator_chain. Likely a buffer overrun or hardcoded sizeof somewhere in CinderX. Avoided by Bug 10 fix (no struct changes), but would be triggered by ANY future struct size increase.

**References:**
- `benchmark_results/21-02-2026-cinderx-bugs-found.md` (Bug 9 section)

---

## Infrastructure Notes

- **Sidecar:** Running. Language drift in check-in prompts continues (English → Sanskrit → French → Navajo). Known prompt issue, not a system bug.
- **Fixup:** Hourly checkpoints running. Occasional agent restarts from permission modal stalls.
- **Team:** Dismissed since 27 Feb 01:37:28Z. No active supervisor.
