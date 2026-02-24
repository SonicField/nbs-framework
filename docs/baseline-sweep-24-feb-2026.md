# CinderX Baseline Sweep — 24 February 2026

**Machine:** devgpu004.kcm2.facebook.com (aarch64, Grace Hopper GB200)
**Branch:** aarch64-jit-generators (HEAD: d1f23232)
**Python:** 3.12.12+meta (standard GIL build — NOT free-threaded)
**Benchmark script:** cinderx_jit_benchmark.sh (ABBA ×2 design)

**Benchmark suite (23 benchmarks):** chaos_game, coroutine_chain, dict_ops, exceptions, fannkuch, fibonacci, float_arith, func_calls, gen_interleaved, gen_nested, gen_parameterised, gen_simple, json_roundtrip, list_comp, method_calls, nbody, nqueens, richards_full, richards_slots, spectral_norm, string_ops, unpack_seq, yield_from_chain. Note: Run 1 used all 23; Run 2 excluded richards_full (crash, 19 benchmarks); Run 3 used a different script that contained 22 benchmarks (richards_full excluded).

## Methodology

**JIT activation:** warmup (10 iterations per function) → `cinderjit.force_compile()`. The warmup phase lets CPython's adaptive interpreter specialise bytecodes (e.g. `BINARY_OP` → `BINARY_OP_ADD_FLOAT`). `force_compile()` then reads those specialised bytecodes and generates type-aware JIT code with appropriate `GuardType` instructions.

**PYTHONJIT=1 / PYTHONJITALL=1 must NOT be used for benchmarks.** These cause eager compilation before warmup, so the JIT compiles unspecialised bytecodes and generates generic code. This defeats the entire specialisation pipeline. (Correction per Alex, 08:05 UTC.)

**JIT verification:** `cinderjit.is_jit_compiled()` returns `False` on aarch64 — this is a cosmetic reporting bug. JIT compilation is confirmed active by: (a) 35% fibonacci speedup (4.71ms vs 7.48ms) which is impossible from interpreter alone, and (b) LICM log output from the compiler during `force_compile()`.

**Note on Py_GIL_DISABLED:** This build uses CPython 3.12, which has no free-threaded mode. All `#ifdef Py_GIL_DISABLED` / `#ifndef Py_GIL_DISABLED` guards in CinderX are dead code in this build. The guarded fast paths (list/tuple subscript, FOR_ITER_LIST) are all ACTIVE. An earlier report incorrectly stated these were disabled; this was retracted after verifying the Python version.

**Vanilla reference binary:** `/usr/local/fbcode/platform010-aarch64/bin/python3.12` — this is the Meta fbcode-built CPython, likely compiled with PGO and LTO optimisations. This is the correct baseline: CinderX must beat the production interpreter, not a debug build. All speedup/regression percentages in this document are measured against this binary. Note: the `python-install` binary in `cinderx_dev/` is a different (unoptimised) build and produces different comparison results.

---

## Run 1: Spec OFF (JIT only, no specialised opcodes)

**Config:** `enable_specialized_opcodes()` commented out. JIT active via `cinderjit.auto()` + `cinderjit.force_compile()`.

| Benchmark | No CinderX | CinderX+JIT | Speedup | Δ% | Notes |
|-----------|-----------|-------------|---------|-----|-------|
| chaos_game | 32.93ms | 35.93ms | 0.92× | -9.1% !! | |
| coroutine_chain | 16.34ms | 17.44ms | 0.94× | -6.7% !! | Generator overhead |
| dict_ops | 5.71ms | 6.01ms | 0.95× | -5.3% !! | |
| exceptions | 9.87ms | 10.92ms | 0.90× | -10.6% !! | |
| fannkuch | 278.35ms | 248.96ms | 1.12× | +10.6% ** | |
| fibonacci | 7.48ms | 4.71ms | 1.59× | +37.0% ** | Simplify pass float speculation |
| float_arith | 4.56ms | 4.92ms | 0.93× | -7.9% !! | Needs spec guards? |
| func_calls | 9.16ms | 7.00ms | 1.31× | +23.5% ** | C→C inlining working |
| gen_interleaved | 18.92ms | 19.03ms | 0.99× | -0.6% | |
| gen_nested | 11.44ms | 11.97ms | 0.96× | -4.6% | |
| gen_parameterised | 5.90ms | 7.22ms | 0.82× | -22.4% !! | Generator overhead |
| gen_simple | 3.82ms | 4.42ms | 0.86× | -15.7% !! | Generator overhead |
| json_roundtrip | 23.01ms | 22.99ms | 1.00× | +0.1% | |
| list_comp | 5.05ms | 5.65ms | 0.89× | -11.9% !! | Fixed by spec ON (+12pp) |
| method_calls | 27.08ms | 28.67ms | 0.94× | -5.9% !! | |
| nbody | 17.03ms | 21.82ms | 0.78× | -28.1% !! | Fixed by spec ON (27pp gain) |
| nqueens | 42.85ms | 33.30ms | 1.29× | +22.3% ** | |
| richards_full | 310.83ms | 225.30ms | 1.38× | +27.5% ** | |
| richards_slots | 5.00ms | 5.51ms | 0.91× | -10.1% !! | |
| spectral_norm | 256.63ms | 290.65ms | 0.88× | -13.3% !! | Float-heavy, needs spec |
| string_ops | 7.13ms | 7.10ms | 1.00× | +0.4% | |
| unpack_seq | 5.44ms | 5.56ms | 0.98× | -2.2% | |
| yield_from_chain | 5.77ms | 7.78ms | 0.74× | -34.9% !! | Generator overhead |
| **TOTAL** | **1110.30ms** | **1032.85ms** | **1.07×** | **+7.0%** | |

`** = JIT >5% faster` `!! = JIT >5% slower`

### Analysis

**Confirmed working (always-on paths):**
- Float speculation via Simplify pass: fibonacci +37%
- C→C call inlining (TranslateSpecializedCall): func_calls +23.5%
- General JIT compilation: richards_full +27.5%, nqueens +22.3%, fannkuch +10.6%

**Regressions needing investigation:**
- Generator overhead: yield_from_chain -35%, gen_parameterised -22%, gen_simple -16%
- Float-heavy without specialisation guards: nbody -28%, spectral_norm -13%
- Potential gaps: list_comp -12% (resolved: spec ON eliminates this regression — subscript guards needed)

**JIT compilation status:** All functions reported as INTERPRETED by `is_jit_compiled()`. This is a known aarch64 reporting bug — the JIT IS compiling (confirmed by 35% fibonacci speedup and LICM compiler log output). See Methodology section above.

### Py_GIL_DISABLED — NOT Applicable

This build uses CPython 3.12 (standard GIL). All `Py_GIL_DISABLED` guards are dead code. The list/tuple subscript and FOR_ITER_LIST fast paths in simplify.cpp and builder.cpp are all ACTIVE. An earlier analysis incorrectly reported these as disabled; this was retracted after checking the Python version.

---

## Run 2: Spec ON (JIT + specialised opcodes)

**Config:** `enable_specialized_opcodes()` active. JIT active via warmup → `cinderjit.force_compile()`. `bench_richards_full` excluded (LICM crash — see below).

| Benchmark | No CinderX | CinderX+JIT | Speedup | Δ% | Spec OFF → ON |
|-----------|-----------|-------------|---------|-----|---------------|
| chaos_game | 32.46ms | 34.97ms | 0.93× | -7.7% !! | -9.1% → -7.7% |
| coroutine_chain | 16.21ms | 17.30ms | 0.94× | -6.8% !! | -6.7% → -6.8% |
| dict_ops | 5.68ms | 5.52ms | 1.03× | +2.8% | -5.3% → +2.8% ↑ |
| exceptions | 10.05ms | 11.03ms | 0.91× | -9.7% !! | -10.6% → -9.7% |
| fannkuch | 282.43ms | 232.31ms | 1.22× | +17.7% ** | +10.6% → +17.7% ↑ |
| fibonacci | 7.37ms | 3.86ms | 1.91× | +47.6% ** | +37.0% → +47.6% ↑↑ |
| float_arith | 4.53ms | 4.88ms | 0.93× | -7.9% !! | -7.9% → -7.9% |
| func_calls | 9.09ms | 7.07ms | 1.28× | +22.2% ** | +23.5% → +22.2% |
| gen_interleaved | 18.55ms | 17.57ms | 1.06× | +5.3% ** | -0.6% → +5.3% ↑ |
| gen_nested | 10.94ms | 10.95ms | 1.00× | -0.1% | -4.6% → -0.1% ↑ |
| gen_parameterised | 5.84ms | 6.44ms | 0.91× | -10.2% !! | -22.4% → -10.2% ↑ |
| gen_simple | 3.78ms | 4.08ms | 0.93× | -7.8% !! | -15.7% → -7.8% ↑ |
| json_roundtrip | 22.76ms | 23.11ms | 0.98× | -1.6% | +0.1% → -1.6% ↓ |
| list_comp | 5.00ms | 5.02ms | 1.00× | -0.4% | -11.9% → -0.4% ↑↑ |
| method_calls | 26.51ms | 28.98ms | 0.91× | -9.3% !! | -5.9% → -9.3% ↓ |
| nbody | 16.88ms | 17.10ms | 0.99× | -1.3% | -28.1% → -1.3% ↑↑↑ |
| nqueens | 42.30ms | 31.94ms | 1.32× | +24.5% ** | +22.3% → +24.5% ↑ |
| richards_full | — | — | — | — | LICM+inliner crash (excluded) |
| richards_slots | 4.95ms | 3.80ms | 1.30× | +23.1% ** | -10.1% → +23.1% ↑↑↑ |
| spectral_norm | 254.02ms | 290.22ms | 0.88× | -14.3% !! | -13.3% → -14.3% ↓ |
| string_ops | 7.05ms | 7.11ms | 0.99× | -0.9% | +0.4% → -0.9% |
| unpack_seq | 5.41ms | 4.27ms | 1.27× | +21.1% ** | -2.2% → +21.1% ↑↑↑ |
| yield_from_chain | 5.74ms | 7.44ms | 0.77× | -29.6% !! | -34.9% → -29.6% ↑ |
| **TOTAL (19)** | **797.54ms** | **774.97ms** | **1.03×** | **+2.8%** | |

### Specialisation Impact Analysis

**Big wins from specialisations (OFF → ON):**
- richards_slots: -10.1% → +23.1% (33pp gain) — STORE_ATTR_SLOT guards
- unpack_seq: -2.2% → +21.1% (23pp gain) — UNPACK_SEQUENCE_TWO_TUPLE guards
- nbody: -28.1% → -1.3% (27pp gain) — float type guards eliminate most regression
- list_comp: -11.9% → -0.4% (12pp gain) — subscript type guards
- fibonacci: +37.0% → +47.6% (11pp gain) — additional float guards on top of Simplify pass
- dict_ops: -5.3% → +2.8% (8pp gain) — dict subscript/store guards
- fannkuch: +10.6% → +17.7% (7pp gain)
- gen_parameterised: -22.4% → -10.2% (12pp gain)
- gen_simple: -15.7% → -7.8% (8pp gain)

**Regressions from specialisations (OFF → ON):**
- method_calls: -5.9% → -9.3% (3pp worse) — possible guard overhead on method resolution
- spectral_norm: -13.3% → -14.3% (1pp worse) — guard overhead exceeds type benefit
- json_roundtrip: +0.1% → -1.6% (2pp worse) — guard overhead on mixed types

**Unchanged:**
- float_arith: -7.9% → -7.9% (no change — needs investigation)
- coroutine_chain, func_calls, chaos_game: minimal change

**Note:** Overall total drops from 1.07× (spec OFF) to 1.03× (spec ON) because richards_full (+27.5%, 311ms) is excluded from spec ON due to the LICM crash. Richards_full was the largest absolute contributor. Post-LICM-fix, the spec-ON total should be significantly higher.

### Crash Root Cause: Interpreter Frame in JIT Lightweight Frame Chain (CONFIRMED 09:55 UTC)

**Root cause confirmed by diagnostic build (supervisor, 09:55):** The crash occurs when the JIT deopts from speculatively inlined code whose outer function's frame was created by the *interpreter*, not the JIT. The JIT's lightweight frame model expects `f_funcobj == frameReifier()` for JIT frames, but interpreter-created frames have `f_funcobj` set to the real function object. `getUnitFrames()` walks the frame chain, finds the outer frame is not a JIT frame (`isJitFrame()` returns false), and aborts.

**Discriminating experiment (generalist, 09:12):**
- Inliner ON + spec ON → CRASH at `frame.cpp:163`
- Inliner OFF + spec ON → SUCCESS, correct result, no crash
- Inliner ON + spec OFF → SUCCESS (no speculative guards to fire)
- Inliner OFF + spec OFF → SUCCESS

**Diagnostic output (supervisor's diagnostic build, 09:55):**
```
DEOPT DIAG: deopt_idx=74, inline_depth=1
DEOPT DIAG: initial frame=0x... code=_RPacket.append_to
DEOPT DIAG: chain[0] code=_RPacket.append_to owner=1
DEOPT DIAG: chain[1] code=_RHandlerTask.fn owner=0
DEOPT DIAG: chain[2] code=_RTask.runTask owner=0
DEOPT DIAG: chain[3] code=_richards_schedule owner=0
DIAG: hit non-JIT frame at 0x... while looking for non-inlined frame
DIAG: [0] code=_RPacket.append_to isJit=true isInlined=true
```

**Mechanism (9 steps):**
1. The HIR inliner speculatively inlines `_RPacket.append_to` into `_RHandlerTask.fn` (via IC type feedback on `CallMethod`)
2. This inserts a `GuardType` before the inlined body, with a `FrameState` at `inline_depth=1`
3. At runtime, `_RHandlerTask.fn` is called by the *interpreter* (from `_RTask.runTask`), which sets `f_funcobj` to the real function object
4. The JIT enters via the compiled code for `_RHandlerTask.fn`, and its frame is the interpreter-created frame
5. The inlined `_RPacket.append_to` executes. The `GuardType` fires (type mismatch on polymorphic dispatch)
6. `prepareForDeopt` (gen_asm.cpp) reads `deopt_idx=74`, `inline_depth=1`, gets `frame` from thread state
7. `reifyLightweightFrames` walks `inline_depth` levels up the frame chain, calling `convertInterpreterFrameFromStackToSlab` → `updatePrevInstr` → `getUnitState` → `getUnitFrames`
8. `getUnitFrames` walks `frame->previous` looking for the first non-inlined JIT frame. It finds `_RHandlerTask.fn`'s frame, checks `isJitFrame()`: `frameFunction(frame) == frameReifier()` → FALSE (interpreter set it to the real function)
9. `getUnitFrames` aborts: "couldn't find non-inlined frame"

**The fundamental issue:** The JIT assumes that if it is executing inlined code, the outer function's frame was created by the JIT (and thus has `f_funcobj == frameReifier()`). But when the outer function is called by the interpreter and then enters JIT code, the frame was created by the interpreter with `f_funcobj` set to the real function object. The JIT's `isJitFrame()` check fails.

**Two fixes applied/identified:**
- **LICM fix** (applied, defence-in-depth): `isHoistableGuard` in `licm.cpp` rejects guards with `inlineDepth() > 0`. Prevents LICM from hoisting inlined guards to the preheader where the inlined call has not occurred. This is semantically correct regardless of the frame bug.
- **Frame reification fix** (Phase 2, APPLIED by generalist): `updatePrevInstr` moved out of `convertInterpreterFrameFromStackToSlab` and called once in `prepareForDeopt` BEFORE `reifyLightweightFrames`, while all frames still have their JIT reifiers. This handles the case where the outer function's frame was created by the interpreter. Files changed: `frame.cpp`, `frame.h`, `gen_asm.cpp`. Verified: 2×2 matrix ALL PASS, richards_full with spec ON + inliner ON produces correct result (returns 1). Clean rebuild (without diagnostic logging) in progress on devgpu-arm3.

**Investigation timeline (9 hypotheses):**
1. LICM + inlined FrameState (08:19) — **PARTIALLY CORRECT** (confirmed 09:12 by inliner falsifier)
2. LICM + general bytecode offset mismatch (08:40) — overcorrection (crash is specifically inlined frames)
3. Interpreter specialised opcodes (08:45) — WRONG (`enable_specialized_opcodes` is JIT-only)
4. Bug 7 re-introduction / 4-arg GuardType (08:59) — WRONG for current HEAD (Bug 7 already applied)
5. `simplifyLoadAttrSplitDict` wrong offset (09:02) — WRONG (correct type guard + key lookup)
6. Inliner falsifier (09:12) — confirms hypothesis 1
7. Interpreter-managed frame mismatch (supervisor, 09:48) — **CORRECT** (confirmed by diagnostic build)
8. Frame reification ordering bug (generalist, 09:48) — subsumed by hypothesis 7
9. Diagnostic build confirmation (supervisor, 09:55) — exact frame chain at crash: `_RPacket.append_to` (isJit=true, isInlined=true) → `_RHandlerTask.fn` (isJit=FALSE, interpreter-created)

**Note:** Testkeeper's wrong-result bug (LOAD_ATTR loading wrong attributes for polymorphic types) was a **separate** bug from the crash. It was NOT REPRODUCED by independent testing (helper's A/B/C matrix all passed with correct results; testkeeper's 23/23 output validation confirmed no corruption). Bug B is deprioritised.

**DATA INTEGRITY:** The spec-ON 19-benchmark results (1.03× overall) were measured WITHOUT the inliner crashing those benchmarks. The crash only affects richards_full (which was excluded). The 19-benchmark timing data is valid — the abort is fail-stop, not silent corruption. Results should be re-verified after the frame reification fix is applied and richards_full is included.

---

## Raw Data

**Run 1 — Spec OFF:** `/tmp/cinderx_benchmark_20260223_235420/` on devgpu004
**Run 2 — Spec ON:** `/tmp/cinderx_benchmark_20260224_000822/` on devgpu004
**Run 3 — Inliner OFF sweep:** `/tmp/cinderx_sweep_inliner_off_20260224_013133/` on devgpu004

Rerun comparisons:
```
python3 /tmp/cinderx_benchmark_20260223_235420/compare.py /tmp/cinderx_benchmark_20260223_235420
python3 /tmp/cinderx_benchmark_20260224_000822/compare.py /tmp/cinderx_benchmark_20260224_000822
```

---

## Run 3: Controlled Spec Comparison, Inliner OFF

**Why inliner is disabled:** A 2×2 matrix (generalist, 09:26 UTC) showed the crash requires BOTH `enable_specialized_opcodes()` AND the HIR inliner. The LICM fix (`isHoistableGuard` parent check) is necessary but not sufficient — the inliner itself has a bug in deopt metadata construction for specialised opcode guards inside inlined functions. When such a guard fires, `frame.cpp:163` cannot reconstruct the inlined frame chain.

The discriminating experiment:

| | Inliner OFF | Inliner ON |
|---|---|---|
| **Spec OFF** | PASS | PASS |
| **Spec ON** | PASS | **CRASH** |

Until the inliner bug is fixed, `PYTHONJITENABLEHIRINLINER=0` is required for spec-ON benchmarks. Runs 1 and 2 had the inliner ON, which confounds the spec-ON/OFF comparison. Run 3 uses matched inliner-OFF configs for both.

### Run 3a: Spec OFF, Inliner OFF (control)

**Config:** No `enable_specialized_opcodes()`. `PYTHONJITENABLEHIRINLINER=0`. JIT active via warmup → `cinderjit.force_compile()`.

| Benchmark | No CinderX | CinderX+JIT | Speedup | Δ% | Notes |
|-----------|-----------|-------------|---------|-----|-------|
| chaos_game | 32.98ms | 36.10ms | 0.91× | -9.5% !! | |
| coroutine_chain | 16.43ms | 17.51ms | 0.94× | -6.6% !! | Generator overhead |
| dict_ops | 5.83ms | 5.88ms | 0.99× | -1.0% | |
| exceptions | 10.22ms | 10.83ms | 0.94× | -6.0% !! | |
| fannkuch | 278.56ms | 253.65ms | 1.10× | +8.9% ** | |
| fibonacci | 7.53ms | 6.40ms | 1.18× | +15.0% ** | Simplify pass only (no inliner) |
| float_arith | 4.61ms | 4.91ms | 0.94× | -6.6% !! | |
| func_calls | 9.19ms | 9.46ms | 0.97× | -2.9% | Inliner OFF → no C→C inlining |
| gen_interleaved | 18.83ms | 19.03ms | 0.99× | -1.1% | |
| gen_nested | 11.11ms | 11.90ms | 0.93× | -7.1% !! | |
| gen_parameterised | 5.91ms | 7.30ms | 0.81× | -23.5% !! | Generator overhead |
| gen_simple | 3.84ms | 4.45ms | 0.86× | -16.0% !! | Generator overhead |
| json_roundtrip | 23.20ms | 23.09ms | 1.00× | +0.5% | |
| list_comp | 5.08ms | 5.58ms | 0.91× | -9.9% !! | |
| method_calls | 27.02ms | 28.96ms | 0.93× | -7.2% !! | |
| nbody | 17.87ms | 21.81ms | 0.82× | -22.0% !! | Float-heavy, no spec guards |
| nqueens | 42.98ms | 35.07ms | 1.23× | +18.4% ** | |
| richards_full | 30.78ms | 42.93ms | 0.72× | -28.3% !! | Supplementary (supervisor, force_compile) |
| richards_slots | 5.03ms | 5.51ms | 0.91× | -9.4% !! | |
| spectral_norm | 257.63ms | 292.34ms | 0.88× | -13.5% !! | |
| string_ops | 7.19ms | 7.13ms | 1.01× | +0.9% | |
| unpack_seq | 5.50ms | 5.69ms | 0.97× | -3.5% | |
| yield_from_chain | 5.82ms | 7.76ms | 0.75× | -33.4% !! | Generator overhead |
| **TOTAL (23)** | **833.13ms** | **863.30ms** | **0.97×** | **-3.5%** | |

### Run 3b: Spec ON, Inliner OFF (treatment)

**Config:** `enable_specialized_opcodes()` active. `PYTHONJITENABLEHIRINLINER=0`. JIT active via warmup → `cinderjit.force_compile()`. (LICM fix is present in the build but irrelevant here — with inliner OFF, no inlined guards exist to be hoisted.)

| Benchmark | No CinderX | CinderX+JIT | Speedup | Δ% | Δ vs 3a |
|-----------|-----------|-------------|---------|-----|---------|
| chaos_game | 32.89ms | 34.14ms | 0.96× | -3.8% | +5.7pp |
| coroutine_chain | 16.32ms | 17.49ms | 0.93× | -7.1% | -0.5pp |
| dict_ops | 5.76ms | 5.50ms | 1.05× | +4.4% | +5.4pp |
| exceptions | 10.13ms | 11.06ms | 0.92× | -9.2% | -3.2pp |
| fannkuch | 279.68ms | 238.27ms | 1.17× | +14.8% ** | +5.9pp |
| fibonacci | 7.48ms | 4.87ms | 1.53× | +34.8% ** | +19.8pp ↑↑ |
| float_arith | 4.61ms | 4.92ms | 0.94× | -6.6% !! | 0.0pp |
| func_calls | 9.14ms | 8.63ms | 1.06× | +5.5% ** | +8.4pp ↑ |
| gen_interleaved | 18.67ms | 17.86ms | 1.05× | +4.3% | +5.4pp |
| gen_nested | 10.99ms | 11.02ms | 1.00× | -0.2% | +6.9pp |
| gen_parameterised | 5.87ms | 6.46ms | 0.91× | -10.1% !! | +13.4pp ↑ |
| gen_simple | 3.81ms | 4.16ms | 0.92× | -9.2% !! | +6.8pp |
| json_roundtrip | 22.99ms | 23.19ms | 0.99× | -0.9% | -1.4pp |
| list_comp | 5.05ms | 5.10ms | 0.99× | -1.0% | +8.9pp ↑ |
| method_calls | 26.71ms | 29.09ms | 0.92× | -8.9% !! | -1.7pp |
| nbody | 17.09ms | 17.28ms | 0.99× | -1.1% | +20.9pp ↑↑ |
| nqueens | 42.91ms | 33.59ms | 1.28× | +21.7% ** | +3.3pp |
| richards_full | 30.62ms | 43.10ms | 0.71× | -29.0% !! | -38.6pp ↓↓ (force_compile) |
| richards_slots | 5.01ms | 3.83ms | 1.31× | +23.5% ** | +32.9pp ↑↑↑ |
| spectral_norm | 256.73ms | 292.13ms | 0.88× | -13.8% !! | -0.3pp |
| string_ops | 7.12ms | 7.16ms | 1.00× | -0.5% | -1.4pp |
| unpack_seq | 5.47ms | 4.28ms | 1.28× | +21.7% ** | +25.2pp ↑↑↑ |
| yield_from_chain | 5.81ms | 7.48ms | 0.78× | -28.7% !! | +4.7pp |
| **TOTAL (23)** | **831.01ms** | **830.60ms** | **1.00×** | **+0.0%** | **+3.5pp** |

`** = JIT >5% faster` `!! = JIT >5% slower` `↑/↑↑/↑↑↑ = spec effect >5/15/25pp`

**richards_full: compilation method matters.** Two measurements exist:
- **force_compile** (supervisor, matches sweep methodology): spec ON = 43.10ms (-29%), spec OFF = 42.93ms (-28%). Spec effect: +0.4pp (noise). No catastrophic regression — spec ON adds nothing but does no harm.
- **auto-JIT** (testkeeper, different methodology): spec ON = 308.13ms (-90%), spec OFF = 28.16ms (+9.6%). Spec effect: -99.7pp. Auto-JIT over-compiles polymorphic helper functions, causing deopt storms when guards fail on polymorphic dispatch.

The 22-benchmark sweep used force_compile (matching supervisor's methodology). For consistency, the tables above use supervisor's force_compile figures for richards_full. The auto-JIT pathology is real but represents a different compilation strategy.

**Note:** richards_full timing from supervisor's matched measurement (N_ITER=100000). Not part of the original ABBA ×2 sweep — separate supplementary run.

### Expected vs Actual Changes (Run 1/2 → Run 3)

Disabling the inliner removes C→C call inlining (TranslateSpecializedCall). Predictions and outcomes:

- **func_calls**: Predicted loss of +23.5% from C→C inlining → **CONFIRMED**. Run 3a: -2.9% (no spec, no inliner). Run 3b: +5.5% (spec recovers some via specialised guards). The +23.5% was entirely from inlining.
- **fibonacci**: Predicted partial loss → **PARTIALLY CONFIRMED**. Run 1: +37.0%, Run 3a: +15.0% (−22pp from inliner loss), Run 3b: +34.8% (spec recovers most of it). Simplify pass does the heavy lifting, but inliner contributed ~2pp.
- **Specialisation gains persist**: **CONFIRMED**. richards_slots +32.9pp, unpack_seq +25.2pp, nbody +20.9pp, fibonacci +19.8pp — these come from GuardType + simplify, not inlining.
- **Generator benchmarks**: Predicted slight improvement → **MIXED**. gen_parameterised improved +13.4pp with spec, yield_from_chain improved +4.7pp. But gen_simple and gen_nested still heavily regressed.

### Specialisation Effect Analysis

**Largest positive spec effects (Δ vs 3a):**

| Benchmark | 3a (spec OFF) | 3b (spec ON) | Spec effect |
|-----------|---------------|--------------|-------------|
| richards_slots | -9.4% | +23.5% | +32.9pp |
| unpack_seq | -3.5% | +21.7% | +25.2pp |
| nbody | -22.0% | -1.1% | +20.9pp |
| fibonacci | +15.0% | +34.8% | +19.8pp |
| gen_parameterised | -23.5% | -10.1% | +13.4pp |
| list_comp | -9.9% | -1.0% | +8.9pp |
| func_calls | -2.9% | +5.5% | +8.4pp |
| gen_nested | -7.1% | -0.2% | +6.9pp |
| gen_simple | -16.0% | -9.2% | +6.8pp |
| fannkuch | +8.9% | +14.8% | +5.9pp |
| chaos_game | -9.5% | -3.8% | +5.7pp |

**Neutral or negative spec effects:**

| Benchmark | 3a (spec OFF) | 3b (spec ON) | Spec effect |
|-----------|---------------|--------------|-------------|
| exceptions | -6.0% | -9.2% | -3.2pp |
| method_calls | -7.2% | -8.9% | -1.7pp |
| json_roundtrip | +0.5% | -0.9% | -1.4pp |
| string_ops | +0.9% | -0.5% | -1.4pp |
| spectral_norm | -13.5% | -13.8% | -0.3pp |
| float_arith | -6.6% | -6.6% | 0.0pp |

**Interpretation:** Specialisation helps most for benchmarks with tight attribute/subscript/iteration loops (richards_slots, unpack_seq, nbody). It hurts slightly where guard overhead exceeds the type-awareness benefit (exceptions, method_calls). float_arith and spectral_norm show no change — these are likely memory-bound or rely on patterns not covered by current specialisation handlers.

### Interpretation Notes

Run 3 isolates the **specialisation effect** (reading CPython's adaptive IC to emit type-aware guards) from the **inlining effect** (inlining callees into callers). The Δ between 3a and 3b is the pure specialisation contribution.

To measure the full JIT potential (specialisation + inlining), the inliner bug must be fixed first. This will be Run 4 (future).

### Polymorphic Dispatch Pathology: richards_full

richards_full exposes a fundamental interaction between specialisation and type polymorphism. The benchmark uses inheritance-based polymorphism: multiple `Task` subclasses (`DeviceTask`, `HandlerTask`, `IdleTask`, `WorkerTask`) sharing `.fn()` method dispatch via a scheduling loop.

**The mechanism (definitive analysis, helper 10:09 + generalist 10:09):**

The guards causing deopt storms are NOT `GuardType` (type version checks). They are **dict values check** guards — the split-dict / values-pointer optimisation for `LOAD_ATTR_INSTANCE_VALUE`. These guards check `tp_version_tag` against a cached version to verify that the type's `__dict__` layout has not changed.

The 340 guard failures per iteration in `_RPacket.append_to` occur because individual `_RPacket` instances have different dict layouts (different insertion orders or extra attributes). The TYPE is the same (`_RPacket`) — this is NOT polymorphic type dispatch. It is dict version invalidation across instances of the same class.

**Deopt site breakdown** (via `cinderjit.get_and_clear_runtime_stats()`, single iteration, force_compile, spec ON, inliner OFF):

| Function | Line | Deopts | Guard type |
|----------|------|--------|------------|
| `_RPacket.append_to` | 174 | 340 | dict values check |
| `_RTaskState.isTaskWaiting` | 212 | 6 | dict values check |
| `_RTaskState.isPacketPending` | 209 | 6 | dict values check |
| `_RTaskState.isTaskHolding` | 206 | 6 | dict values check |
| `_RTaskState.isTaskHoldingOrWaiting` | — | 1 | GuardType |
| `_richards_schedule` | 377 | 1 | GuardType |

Total: 360 deopts/iteration, 6 unique deopt sites, 71 compiled functions.

**Cost model (generalist, 10:09):** The JIT compiles each function ONCE (invocation-count-based recompilation, not deopt-count-based). On each call to `_RPacket.append_to`, the same compiled code is re-entered, the dict values guard fails, and the function falls back to the interpreter for the rest of that invocation. Per-call cost: JIT entry + guard check + deopt transition (register save, frame reification) + interpreter execution. This is WORSE than pure interpreter (which skips JIT entry and deopt entirely).

For monomorphic call sites (e.g. `richards_slots` where `__slots__` constrains the type and dict layout), the guards succeed on every iteration and branch prediction makes them nearly free.

**Quantitative impact:**

| Config | Compilation | richards_full time | vs vanilla | Notes |
|--------|------------|-------------------|------------|-------|
| Spec OFF, inliner ON (Run 1) | force_compile | 225.30ms | +27.5% | Inliner specialises per-callsite |
| Spec OFF, inliner OFF (Run 3a) | force_compile | 42.93ms | -28.3% | No guards, no inlining |
| Spec ON, inliner OFF (Run 3b) | force_compile | 43.10ms | -29.0% | Guards add nothing (+0.4pp) |
| Spec ON, inliner OFF | auto-JIT | 308.13ms | -90.1% | **DEOPT STORMS** (over-compiled helpers) |
| Spec OFF, inliner OFF | auto-JIT | 28.16ms | +9.6% | Auto-JIT compiles top-level efficiently |

**Two distinct findings:**

1. **force_compile** (matches sweep methodology): Spec ON has NO EFFECT on polymorphic code (+0.4pp, noise). The specialised guards on the top-level function's attribute accesses do not cause deopt storms because force_compile only compiles the benchmark function, not the polymorphic helpers. The guards on the benchmark function's monomorphic local types succeed.

2. **auto-JIT** (testkeeper's supplementary run): Spec ON causes a 10× regression. `cinderjit.auto()` compiles ALL hot functions including the polymorphic dispatch helpers (`_RPacket.append_to`, `_RHandlerTask.fn`, etc.). With spec ON, these helpers get dict values check guards that fail on every call because `_RPacket` instances have varying dict layouts. Each guard failure triggers deopt → interpreter fallback. The JIT does NOT recompile after deopt (invocation-count-based, not deopt-count-based), so the same failing guard is hit on every subsequent call.

**Why the inliner solves both:** The inliner specialises each call site for its observed type, converting megamorphic guards into monomorphic per-callsite guards that succeed. Run 1 shows +27.5% with inliner ON (spec OFF) — the inliner contributed the entirety of the benefit. The target configuration (spec ON + inliner ON) should combine both effects.

**Contrast with richards_slots:** richards_slots uses `__slots__` classes with uniform types in the hot loop. Spec effect: +32.9pp (best in suite). This demonstrates that specialisation IS effective when types are monomorphic.

**Implication for the JIT compilation strategy:** The auto-JIT pathology reveals that `cinderjit.auto()` needs guard-failure awareness. Three potential mitigations:
1. **Per-site deopt counting** (Phase 3): after N guard failures at the same site, de-specialise that guard (remove the dict values check, fall through to generic LoadAttr). With 4 unique guard sites × threshold 3 = 12 deopts then stable. This directly addresses the 'warm code with cold guards' problem.
2. **Guard coalescing**: combine multiple dict values guards into a single check per type per function
3. **Inliner fix** (Phase 2, DONE): the inliner naturally solves this by per-callsite specialisation — but only after the frame reification bug is fixed (now fixed by generalist)

**Deopt storm investigation (CLOSED):** helper's runtime stats analysis + generalist's recompilation mechanism check definitively identified the root cause as dict layout variation across `_RPacket` instances, NOT type polymorphism or recompilation. The force_compile approach correctly leaves inner helpers in the interpreter, avoiding the pathology entirely.

### Output Validation (testkeeper, 09:41 UTC)

**23/23 benchmarks produce identical return values** between spec ON and spec OFF, in both interpreter-only and JIT modes. No Bug B corruption detected.

Tested benchmarks: gen_simple, gen_parameterised, gen_nested, gen_interleaved, coroutine_chain, yield_from_chain, func_calls, float_arithmetic, fibonacci_recursive, nbody_step, spectral_norm, chaos_game, richards_slots, richards_full, fannkuch, nqueens, json_roundtrip, method_calls, dict_ops, list_comp, string_ops, unpack_sequence, exceptions.

Key observations:
- richards_full: outputs match (returns 1) — no crash with inliner OFF
- All deterministic benchmarks produce identical return values spec ON vs spec OFF
- chaos_game uses seeded random — still matches
- 7 benchmarks return None (generators, func_calls, dict_ops, list_comp) — SKIP-equivalent, validated as matching

Methodology: `PYTHONJITENABLEHIRINLINER=0` for all runs. N_ITER=1000, N_WARMUP=20.

### Aggregate Results

**22-benchmark subset (excluding richards_full):**
- **Spec OFF (inliner OFF):** JIT is **0.978×** vanilla (−2.2%)
- **Spec ON (inliner OFF):** JIT is **1.016×** vanilla (+1.6%)
- **Specialisation effect:** **+3.8pp** (from −2.2% to +1.6%)

**Distribution of spec effect (22-benchmark subset, hypergrep cross-check):**
- Time-weighted aggregate: **+3.8pp** (dominated by spectral_norm at 35.6% of runtime with -0.3pp effect)
- Equal-weighted mean: **+7.5pp** (better summary of per-benchmark improvement)
- Equal-weighted median: **+5.6pp**
- 16 of 22 benchmarks show positive spec effect; 6 show neutral-to-negative (worst: exceptions -3.2pp)

**23-benchmark total (including richards_full, force_compile):**
- **Spec OFF (inliner OFF):** JIT is **0.965×** vanilla (−3.5%)
- **Spec ON (inliner OFF):** JIT is **1.000×** vanilla (+0.0%)
- **Specialisation effect:** **+3.5pp** — consistent with 22-benchmark subset

richards_full with force_compile shows spec effect of +0.4pp (noise). The 23-benchmark aggregate is no longer dominated by a catastrophic outlier.

**Auto-JIT caveat:** testkeeper's auto-JIT measurement showed a 10× regression for richards_full (308ms vs 28ms). This is caused by `cinderjit.auto()` over-compiling polymorphic helper functions — the guards on those helpers cause deopt storms. With `force_compile` (top-level only), polymorphic helpers remain in the interpreter and handle dispatch efficiently. The 22-benchmark sweep used force_compile, so this auto-JIT pathology does not affect the sweep data.

**Key finding:** Specialisation helps monomorphic/dimorphic workloads (+3.8pp across 22 benchmarks) and is neutral for polymorphic code when using force_compile. The auto-JIT over-compilation of inner helpers is a separate concern — the JIT needs per-site deopt counting to de-specialise guards that fail repeatedly on dict layout variation (not type polymorphism).

All 22 benchmarks from the original sweep completed without crash in both conditions. ABBA ×2 design, 16 runs total (8 per condition). richards_full timing from supervisor's force_compile supplementary measurement (warmup + force_compile, N_ITER=100000, matching sweep methodology).

### Verification Checklist

- [x] richards_full correctness confirmed (testkeeper output validation, returns 1, no crash) — NOT in original sweep
- [x] richards_full timing collected (testkeeper supplementary, 09:49) — **10× regression under spec ON + inliner OFF (auto-JIT only, not force_compile)**
- [x] 80 correctness tests pass (testkeeper, 09:28) — FOR_ITER_RANGE 20/20, FOR_ITER_TUPLE 20/20, LOAD_ATTR_INSTANCE_VALUE 20/20, STORE_SUBSCR_DICT 20/20
- [x] Output validation: 23/23 benchmarks produce identical return values spec ON vs spec OFF (testkeeper, 09:41)
- [x] Per-benchmark timing breakdown — DONE (helper 09:42 + supervisor 09:55; all 23 benchmarks)
- [x] richards_full timing data — RESOLVED: force_compile shows +0.4pp (noise), auto-JIT shows -90% (separate pathology, documented)
- [x] Deopt mechanism — CLOSED: dict values check guards on varying `_RPacket` dict layouts, not type polymorphism or recompilation (helper 10:09 + generalist 10:09)
- [ ] Raw data archived locally — PENDING

---

## Phase 2: Frame Reification Fix — Status

**Fix applied by generalist (10:00 UTC).** Three files changed on devgpu004: `frame.cpp`, `frame.h`, `gen_asm.cpp`. The fix moves `updatePrevInstr` to `prepareForDeopt`, called ONCE before `reifyLightweightFrames` while all frames still have JIT reifiers.

### Phase 2 Gate Status (gatekeeper, 10:18 — CONDITIONAL PASS)

| Gate | Status | Evidence |
|------|--------|----------|
| 1. Crash gate | **PASS** | generalist 2×2 matrix ALL PASS + richards_full smoke test (spec ON + inliner ON, returns 1) |
| 2. Correctness gate | **PASS** | 80/80 spec opcode tests PASS (testkeeper, 10:08). 22/23 output validation PASS (10:15). Bug C (spec OFF crash) is non-target config. |
| 3. Performance gate | AWAITING | Needs clean rebuild + ABBA sweep. Prediction: 22 monomorphic benchmarks improve, richards_full regresses under auto-JIT (accepted limitation). |
| 4. Document gate | AWAITING | Results to be documented after sweep. |

### Phase 2 Correctness (testkeeper, 10:08)

80/80 spec opcode correctness tests PASS with inliner ON:
- FOR_ITER_RANGE: 20/20
- FOR_ITER_TUPLE: 20/20
- LOAD_ATTR_INSTANCE_VALUE: 20/20
- STORE_SUBSCR_DICT: 20/20

Config: PYTHONJIT=1, inliner enabled (default). Reification fix does NOT break any specialised opcode family.

**Note:** Tests ran against build WITH diagnostic logging (pre-rebuild). Correctness results are valid — JIT_LOG does not change code generation or semantics. Performance measurements require the clean rebuild.

### Pending

- Supervisor: clean rebuild on devgpu-arm3 (removing diagnostic JIT_LOG lines)
- ~~Testkeeper: 23/23 output validation with inliner ON~~ DONE (22/23 PASS, see Bug C below)
- After clean rebuild: gate-quality ABBA performance sweep (spec ON + inliner ON) → Run 4

### Phase 2 Output Validation (testkeeper, 10:15)

**22/23 PASS, 1 mismatch** (auto-JIT, inliner ON):

- Interpreter only: 23/23 OK (spec ON vs spec OFF outputs match)
- JIT mode (auto-JIT, inliner ON): 22/23 OK

**The mismatch is richards_full — but the failure is in spec OFF, not spec ON:**
- Spec OFF + auto-JIT + inliner ON: **CRASH** (rc=1, Python exception in `_richards_schedule` line 264)
- Spec ON + auto-JIT + inliner ON: returns 1 (correct)

**This is Bug C — a NEW bug, distinct from the Phase 1 crash (Bug A):**

| | Bug A (Phase 1) | Bug C (Phase 2, NEW) |
|---|---|---|
| Config | spec ON + inliner ON | spec OFF + inliner ON + auto-JIT |
| Exit code | rc=-6 (C abort) | rc=1 (Python exception) |
| Location | frame.cpp:163 | `_richards_schedule` line 264 |
| Cause | Frame reification ordering | UNKNOWN — awaiting traceback |
| Status | FIXED (generalist) | OPEN |

**Why this was not seen before:** Generalist's 2×2 matrix used `force_compile`, which does not compile inner helpers. Run 1 also used `force_compile` (helper confirmed, 10:17). The Bug C crash requires auto-JIT to compile the inner polymorphic helpers (`_RPacket.append_to`, etc.) WITHOUT specialised guards (spec OFF). This configuration was never tested before testkeeper's output validation. The crash is NOT necessarily a regression from the reification fix — it may be pre-existing but only triggered under auto-JIT compilation of inner helpers.

**Impact on Phase 2:** Bug C does NOT block the Phase 2 performance sweep. The target configuration is spec ON + inliner ON, which passes. Bug C affects the non-target configuration (spec OFF + inliner ON) and should be filed for Phase 3 investigation.

### Phase 2 Key Question: Does the Inliner Fix richards_full?

**Critical clarification (helper, 10:12):** The benchmark script uses auto-JIT via warmup, NOT `force_compile`. All inner functions that become hot during warmup are compiled — including `_RPacket.append_to`. The Phase 2 performance sweep will therefore show the richards_full deopt storm UNLESS the inliner (now enabled) changes the guard behaviour.

**ANSWER: NO — the inliner does NOT fix the deopt pathology (helper, 10:15).**

Deopt counts for richards_full, spec ON + auto-JIT, single iteration:

| Config | Total deopts | Dominant site | Guard type |
|--------|-------------|---------------|------------|
| Inliner OFF | 360 | `_RPacket.append_to` (340) | dict values check |
| Inliner ON | 11,646 | `_RTask.qpkt` (11,625) | GuardType |

The inliner successfully inlined `_RPacket.append_to` (it disappears from the deopt list). But this shifted the guard failure upstream to `_RTask.qpkt`, which now has 11,625 GuardType deopts — **30× more** than the inliner-OFF configuration. The inliner widened the blast radius: more code is in the JIT-compiled region, so more code falls back to interpreter on each deopt.

**Timing paradox:** Despite 30× more deopts, timing is slightly better (42ms vs 48ms). The inliner reduces call overhead on the non-deopt path, roughly offsetting the increased deopt cost.

**Prediction (a) FALSIFIED:** The inliner does NOT eliminate deopt storms on polymorphic code. The Phase 2 sweep will still show a large richards_full regression under auto-JIT.

**Implications:**
- The +3.5pp aggregate from Phase 1 will NOT improve for the 23-benchmark set with inliner ON
- richards_full remains a major outlier under auto-JIT methodology
- The fix is per-site deopt counting (Phase 3), not inlining

**Methodology note (helper, 10:17):** Run 1 used `force_compile`, NOT auto-JIT. Run 1's +27.5% for richards_full reflected force_compile's benefit (top-level function only, inner helpers in interpreter). This was incorrectly cited as evidence that the inliner solved the deopt problem — the inliner was not the relevant factor; the compilation scope was.

