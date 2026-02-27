# CinderX JIT Benchmark Runbook

**Date:** 27 February 2026
**Author:** theologian
**Purpose:** Single-source operational guide for running CinderX JIT benchmarks on any machine. Consolidates knowledge from decision logs D-1772143688 through D-1772193715, benchmark reports, code comments, and debugging sessions.
**Falsifier:** If an agent follows this runbook on a fresh machine and produces valid benchmark results without debugging infrastructure issues, the runbook is sufficient. If she hits an undocumented failure, the runbook has a gap.

---

## 1. Prerequisites

### 1.1 Machine Requirements

- **Architecture:** aarch64 (tested on Grace CPU). x86_64 works but results are NOT directly comparable to aarch64 results.
- **Build:** Optimised build required for benchmarks (debug builds have different performance characteristics).

### 1.2 Source Code

The fork is at the CinderX development directory on the aarch64 dev server.

**CRITICAL — Fork Divergence:** The fork's `compile_after_n_calls` is `optional<uint32_t>`, NOT `compile_all` (a `bool` in upstream). Three agents made incorrect assessments by reading upstream code instead of fork code (decision D-1772191500 series). Always read the fork source, not upstream.

### 1.3 Branch

Branch: `aarch64-jit-generators`

Key commits (chronological):
- `41c82288` — JIT default-on (baseline, no deopt backoff)
- `ee8e4b9c` — Step 6 cold block marking
- `105ee2c6` — Deopt backoff v5 (CI_CO_SUPPRESS_JIT mechanism)
- `0730c07e` — kDeoptBackoffThreshold 100→1000 + documentation

---

## 2. Mandatory Flags and Their Rationale

### 2.1 `-S` Flag (REQUIRED)

**What:** Python's `-S` flag skips `site.py` loading.

**Why:** The fork sets `compile_after_n_calls=0` during `_cinderx.so` loading (before `cinderjit.auto()` is called). This causes JIT compilation of every function encountered during import, including `spec_from_loader` in `importlib`. The JIT-compiled `spec_from_loader` triggers a SIGSEGV. The `-S` flag prevents `site.py` from loading `_cinderx.so` during the import phase, deferring JIT activation until the benchmark script controls it.

**What happens without it:** SIGSEGV in `spec_from_loader` during import. This crash occurs regardless of the deopt backoff threshold value — it is about JIT activation timing, not the threshold.

**Decision log:** D-1772191842, D-00:53Z (26-02), D-00:39Z (26-02).

### 2.2 `cinderjit.auto()` Before Imports (REQUIRED)

**What:** Call `cinderjit.auto()` at the top of the benchmark script, before importing benchmark functions.

**Why:** `cinderjit.auto()` sets `compile_after_n_calls=1000` and walks all existing function objects to register them for JIT compilation. If called AFTER importing benchmark functions, those functions miss the registration walk and may not be JIT-compiled. The `auto()` call ensures the adaptive interpreter specialises bytecodes for 1000 calls before JIT compilation, providing type feedback for better JIT code.

**What happens without it:** Benchmark functions may run interpreted-only or compile without type feedback, producing unrepresentative results.

### 2.3 Skip `cinderx.init()` (REQUIRED on aarch64)

**What:** Do NOT call `cinderx.init()`.

**Why:** Bug 8 — `cinderx.init()` causes SIGSEGV on aarch64 due to `f_globals` corruption in `resumeInInterpreter`. The JIT works without `init()` because `cinderjit.auto()` is sufficient to activate JIT compilation.

**What happens with it:** SIGSEGV. The crash is in `resumeInInterpreter` when accessing `f_globals` of a resumed frame.

---

## 3. Benchmark Command

```bash
python3 -S benchmark_cinderx.py all --compile=auto --reps=2
```

**Subcommands:**
- `all` — Full suite: JIT vs Vanilla, ABBA comparison, specialisation ON/OFF, G1 generator fast path
- `jit` — JIT vs Vanilla only
- `abba` — ABBA interleaved comparison only

**Do NOT use** `--compile=force` for production-representative results. `force` compiles immediately without adaptive interpreter warmup, which is not how production code behaves. Use `auto` (threshold=1000).

**Decision log:** D-19:04 (26-02) — Alex corrected command from `jit` to `all --compile=auto --reps=2`.

---

## 4. Known Bugs

| Bug | Symptom | Workaround | Status |
|-----|---------|------------|--------|
| Bug 8 | SIGSEGV in `resumeInInterpreter` (`f_globals` corruption) | Skip `cinderx.init()`, use `cinderjit.auto()` only | Open — aarch64 only |
| compile_after_n_calls=0 | SIGSEGV in `spec_from_loader` during import | Use `-S` flag | Open — architectural (JIT activation timing) |
| _Environ.__iter__ deopts | JIT debug noise from `os.environ` iteration | Non-blocking — falls back to interpreter | Cosmetic at threshold=1000 |

---

## 5. Deopt Backoff Mechanism

### 5.1 How It Works

The deopt backoff system suppresses JIT compilation for functions that repeatedly fail runtime guards.

```
1. JIT code runs → GuardType check fails
2. Context::recordDeopt(CodeRuntime*, deopt_idx) increments counter
3. Counter exceeds kDeoptBackoffThreshold (currently 1000)
4. CI_CO_SUPPRESS_JIT flag set on PyCodeObject
5. reoptFunc() checks flag → returns false → function stays interpreted
6. All function objects sharing that code object lose their compiled code
```

**Source:** `cinderx/Jit/context.h` (threshold definition), `cinderx/Jit/context.cpp` (recordDeopt implementation).

### 5.2 Threshold Values

- **100** (original): Effective for most deopt-active benchmarks. May suppress import-time functions prematurely in complex import graphs.
- **1000** (current): Performance-neutral compared to 100 for all 24 benchmarks. Bench-2 (threshold=100) and bench-3 (threshold=1000) show identical results within noise.

### 5.3 Two Independent Thresholds (Do Not Conflate)

| Threshold | Controls | Default | Set By |
|-----------|----------|---------|--------|
| `compile_after_n_calls` | When JIT first compiles a function | 1000 (via `auto()`) | `cinderjit.auto()` or `PYTHONJITAUTO=N` |
| `kDeoptBackoffThreshold` | When JIT gives up on a repeatedly-failing function | 1000 | Compile-time constant in `context.h` |

These are independent mechanisms. `compile_after_n_calls` controls JIT activation timing. `kDeoptBackoffThreshold` controls JIT abandonment after guard failures. Confusing them caused diagnostic errors in this project (decision D-1772191842).

---

## 6. Benchmark Categories

### 6.1 Expected Results (as of 0730c07e, aarch64)

| Category | Count | Expected Behaviour |
|----------|-------|--------------------|
| **Compute-heavy wins** | 5 | fibonacci 1.90x, richards_full 1.69x, nqueens 1.45x, spectral_norm 1.09x, dunder_protocol 1.07x |
| **Neutral** | 5 | richards_slots, float_arith, nbody, import_callee, store_subscr (within ±5%) |
| **Structural regressions** | 10 | func_calls, int_arith, gen_simple, gen_nested, list_comp, dict_ops, try_except_callee, context_manager, kwargs_dispatch, positional_dispatch |
| **Deopt-suppressed** | 4 | deep_class_super 0.91x, nn_module_forward 0.78x, decorator_chain 0.79x, pytorch_cm 0.70x |

### 6.2 Regression Root Causes

**Structural regressions** (zero deopts — JIT code quality vs adaptive interpreter):
- **Dispatch overhead:** positional_dispatch (0.63x), kwargs_dispatch (0.68x) — CPython 3.12 adaptive CALL_PY_EXACT_ARGS is faster than JIT dispatch
- **Missing JIT specialisations:** gen_simple, gen_nested (no FOR_ITER_GEN in JIT builder), list_comp (no LIST_APPEND specialisation)
- **Guard cost:** pytorch_cm (0.70x) — guard evaluation overhead, not deopt churn

**Deopt-suppressed regressions** (formerly deopt-caused, now backed off):
- deep_class_super: was 0.52x (1.1M deopts), now 0.91x (deopts suppressed, interpreter fallback)
- nn_module_forward: was 0.38x (40K deopts), now 0.78x
- decorator_chain: was 0.68x (50K deopts), now 0.79x

---

## 7. Canonical File Locations

| File | Location | Purpose |
|------|----------|---------|
| Benchmark script | aarch64 dev server: `benchmark_cinderx.py` in CinderX root | Canonical copy — run benchmarks from here |
| CinderX source | aarch64 dev server: `cinderx/` in CinderX root | Fork source code |
| Deopt backoff threshold | `cinderx/Jit/context.h` | `kDeoptBackoffThreshold` definition |
| Deopt backoff logic | `cinderx/Jit/context.cpp` | `recordDeopt()` implementation |
| JIT activation | `cinderx/Jit/pyjit.cpp` | `reoptFunc()`, `scheduleJitCompile()` |

**WARNING — Version Skew:** Multiple copies of `benchmark_cinderx.py` may exist across machines. These may have diverged. The aarch64 dev server copy is canonical. Before running on a different machine, verify the script matches the canonical version.

---

## 8. Cross-Machine Comparison Rules

1. **Never compare results across architectures.** aarch64 and x86_64 have different performance profiles. fibonacci is 2.06x on x86_64, 1.92x on aarch64 — these are not comparable.
2. **Same machine, same commit** for valid A/B comparisons. Thermal conditions, background load, and CPU frequency scaling affect results.
3. **ABBA methodology** controls for thermal drift: interleave A/B/B/A blocks rather than running all-A then all-B.
4. **Subprocess isolation** prevents JIT state leaking between benchmarks. Each benchmark runs in a fresh subprocess.

---

## 9. Verification Checklist (Pre-Run)

Before interpreting any benchmark results, verify:

- [ ] Running with `-S` flag (check `sys.flags.no_site` is True)
- [ ] `cinderjit` module is importable (confirms `_cinderx.so` is on path)
- [ ] `cinderjit.auto()` was called before benchmark imports
- [ ] `cinderx.init()` was NOT called (aarch64)
- [ ] Using `--compile=auto`, not `--compile=force`
- [ ] Correct branch (`aarch64-jit-generators`) checked out
- [ ] Optimised build (not debug)

**These are enforced as executable assertions in the benchmark script** (commit 857b3287). A benchmark that runs without these preconditions now exits with a clear error message instead of producing silently invalid results.

---

## 10. Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| SIGSEGV in `spec_from_loader` | Missing `-S` flag | Add `-S` to python command |
| SIGSEGV in `resumeInInterpreter` | `cinderx.init()` called on aarch64 | Remove `cinderx.init()` call |
| JIT not loading | `_cinderx.so` not on `sys.path` | Verify build completed, check `PYTHONPATH` |
| All benchmarks show 1.00x | JIT never activated | Verify `cinderjit.auto()` called before imports |
| PYTHONJIT=1 shows improvement but no JIT loaded | False positive — env var without `_cinderx.so` | Verify with `import cinderjit; print(cinderjit)` |
| Deopt debug noise in output | `_Environ.__iter__` at threshold=1000 | Cosmetic — ignore, does not affect results |
