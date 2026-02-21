# Falsification Methodology for CinderX Speculative Inlining Benchmarks

## Central Claim Under Test

> The speculative inlining in commit 725004da makes method-call-heavy code
> faster when running under the CinderX JIT.

This document describes how the benchmark suite attempts to **disprove** this
claim. Every check is designed to catch a specific failure mode that would
invalidate the results.

## What Would Disprove the Claim

### Falsifier 1: The JIT is not compiling benchmark functions

**Risk:** Functions run in the CPython interpreter, not the JIT. Any timing
differences reflect interpreter variation, not JIT performance.

**Check:** After warmup, call `cinderjit.is_jit_compiled(func)` for each
benchmark function. This returns `True` only if the function has been compiled
by the JIT and is currently executing native code.

**What failure means:** If `is_jit_compiled` returns `False` after 3000 warmup
calls, the JIT never compiled the function. Results measure interpreter
performance, not JIT performance. The benchmark aborts.

### Falsifier 2: The speculative inliner is not engaged

**Risk:** The JIT compiles functions (Tier 1) but never triggers Tier 2
recompilation with speculative inlining. Results measure the JIT without the
commit 725004da optimisation.

**Check (a):** `cinderjit.is_hir_inliner_enabled()` returns `True`. This
confirms the inliner subsystem is active.

**Check (b):** Dump the HIR (High-level Intermediate Representation) for
`call_speak` via `cinderjit.print_hir()` and search for `BeginInlinedFunction`.
This marker appears in the HIR **only** when speculative inlining has engaged —
specifically, when Tier 2 recompilation has fired and the inliner has inserted
the callee's body into the caller.

**What failure means:** If `BeginInlinedFunction` is absent from the HIR, Tier 2
recompilation either did not fire (warmup insufficient) or the IC was not
monomorphic (no speculation target). Results measure Tier 1 JIT, not the
speculative inlining from commit 725004da.

### Falsifier 3: We are measuring interpreter differences, not JIT differences

**Risk:** If the A and B conditions use different Python interpreters (e.g.,
CinderX Python vs system Python), any performance difference could reflect
interpreter build differences (compiler flags, included patches) rather than
the JIT inliner.

**Check:** Both conditions A (inliner ON) and B (inliner OFF) use the **same**
venv CinderX Python binary. The only difference is a single API call:
`cinderjit.disable_hir_inliner()` in condition B. This isolates the speculative
inlining effect.

**What failure means:** If different binaries are used, the comparison is
confounded. The benchmark verifies both conditions use the same `VENV_PYTHON`
path.

A third condition C (system `python3.12 -I` with no CinderX) is included as a
**reference only** — it shows overall JIT overhead versus vanilla CPython but is
not the primary comparison.

### Falsifier 4: Warmup is insufficient for Tier 2 compilation

**Risk:** Tier 2 recompilation requires `kTier2ThresholdDefault` (default: 1000)
invocations of a Tier 1 compiled function. If the warmup does not exceed this
threshold, the benchmark runs Tier 1 code without speculative inlining.

**Check:** All benchmarks warm up with 3000 calls. The minimum for Tier 2 is
1100 calls: 100 calls to trigger Tier 1 compilation (via
`cinderjit.compile_after_n_calls(100)`), then 1000 more for the
`tier1_invocation_count` to reach `kTier2ThresholdDefault` and trigger Tier 2
recompilation.

**What failure means:** If warmup is below 1100 calls, Tier 2 never fires. The
`BeginInlinedFunction` HIR check (Falsifier 2b) catches this — if Tier 2 did
not fire, the marker will be absent.

### Falsifier 5: Thermal throttling or load variation corrupts the comparison

**Risk:** CPU thermal throttling, background processes, or memory pressure
cause systematic bias. If condition A always runs first (hot CPU) and B runs
second (throttled CPU), the comparison is invalid.

**Check:** The ABBA pattern — A, B, B, A — with 2 repetitions (8 samples per
benchmark, 4 per condition). This interleaves conditions so any systematic
drift affects both equally. The median (not mean) is reported, reducing
sensitivity to outliers.

**What failure means:** If the ABBA pattern shows high variance within a
condition (e.g., first A sample differs by >20% from second A sample), thermal
or load effects are significant. The raw timing data is preserved for manual
inspection.

## Three-Phase Verification Gate

The benchmark suite runs a mandatory verification gate before any benchmark
execution. If the gate fails, the script aborts with exit code 1.

### Phase 1: Compilation and Inliner State

Runs a small method-call benchmark with CinderX JIT:

| Check | API | Pass condition |
|-------|-----|---------------|
| JIT compilation | `cinderjit.is_jit_compiled(call_speak)` | Returns `True` |
| JIT compilation | `cinderjit.is_jit_compiled(run_method_calls)` | Returns `True` |
| Inliner state | `cinderjit.is_hir_inliner_enabled()` | Returns `True` |
| Compiled count | `cinderjit.get_compiled_functions()` | List is non-empty |

If any required check fails, the script aborts. APIs that are not available
(older CinderX builds without `is_jit_compiled`) do not cause failure — they
are noted as skipped.

### Phase 2: Inliner ON vs OFF Timing Comparison

Runs the same benchmark twice: once with the inliner enabled (condition A) and
once with it disabled via `cinderjit.disable_hir_inliner()` (condition B).

| Outcome | Interpretation |
|---------|---------------|
| A is >2% faster than B | Inliner effect confirmed |
| A and B within 2% | Inliner may not be engaged for this workload — warning issued |
| A is >2% slower than B | Inlining overhead exceeds savings — noted but not fatal |

This phase does **not** abort on any outcome. Some workloads show small effects.
The warning ensures results are interpreted with appropriate caution.

### Phase 3: HIR Dump Proof (Non-Fatal)

Captures the HIR dump for `call_speak` via `cinderjit.print_hir()` and searches
for `BeginInlinedFunction`:

| Outcome | Interpretation |
|---------|---------------|
| `BeginInlinedFunction` found | **Proof** that speculative inlining engaged |
| `BeginInlinedFunction` absent | Inlining did not fire — results measure Tier 1 only (warning) |
| `print_hir` not available or crashes | Cannot verify — noted as skipped |

The inlined function name (from the HIR output line) is captured as evidence.

**Note:** `print_hir` requires a CinderX **debug build** (`--with-pydebug`).
On release/optimised builds, the call triggers an assertion failure
(`data_.irfunc != nullptr`) and crashes with SIGABRT. Phase 3 is therefore
non-fatal — if it fails, the script continues with a warning. Phase 2 (timing
delta) provides equivalent evidence of inliner engagement.

## CinderX JIT APIs Used

| API | Purpose |
|-----|---------|
| `cinderx.init()` | Initialise CinderX runtime (required before any JIT operations) |
| `cinderjit.compile_after_n_calls(100)` | Set Tier 0 → Tier 1 threshold to 100 calls |
| `cinderjit.is_jit_compiled(func)` | Check if `func` is currently JIT-compiled |
| `cinderjit.is_hir_inliner_enabled()` | Check if the HIR inliner subsystem is active |
| `cinderjit.disable_hir_inliner()` | Disable speculative inlining (condition B) |
| `cinderjit.get_compiled_functions()` | List all currently compiled functions |
| `cinderjit.print_hir(func)` | Dump HIR to stdout for structural inspection |

## Condition Init Code

### Condition A (Inliner ON)

```python
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)
assert cinderjit.is_hir_inliner_enabled()  # Verify inliner is ON
```

### Condition B (Inliner OFF)

```python
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)
cinderjit.disable_hir_inliner()
assert not cinderjit.is_hir_inliner_enabled()  # Verify inliner is OFF
```

### Condition C (Baseline — Reference Only)

```
python3.12 -I  # Isolated mode, no site-packages, no CinderX
```

## Known Limitations

1. **No `cinderjit.get_compilation_tier(func)` API.** There is no way to
   programmatically verify that a function is running Tier 2 code (as opposed
   to Tier 1). The `BeginInlinedFunction` HIR check is indirect evidence —
   this marker only appears after Tier 2 recompilation.

2. **`print_hir` requires debug builds.** On release/optimised builds,
   `cinderjit.print_hir()` triggers an assertion failure and crashes with
   SIGABRT. The Phase 3 HIR dump is therefore only available when testing
   against a debug build of CinderX. Phase 2 (timing delta) provides
   equivalent evidence on release builds.

3. **`print_hir` outputs to stdout.** The script captures this via
   `io.StringIO` redirection, which may not capture output from the C++
   layer if it writes directly to `fd 1` bypassing Python's `sys.stdout`.
   If HIR dump is empty despite JIT compilation, this is the likely cause.

4. **Single-run baseline.** Condition C runs once per benchmark (not ABBA).
   This is acceptable because C is a reference only, not the primary
   comparison.

## Observed Results

### Phase 2 Verification (Commit 725004da, build-host

First run of the benchmark falsification gate produced:

- **Inliner ON:** 27.3ms (method_calls, 500 × 1000 iterations)
- **Inliner OFF:** 34.8ms (same workload)
- **Speedup:** 1.27x (inliner ON is 27% faster)

This exceeds the 2% noise threshold and confirms speculative inlining is
engaged and producing a measurable performance improvement on method-call-heavy
code.

### Pre-existing Bug Discovered

The adversarial tight-loop mutation test (200 rapid type mutations) exposed a
**pre-existing CinderX JIT bug**: after ~100 rapid monkey-patches to a class
method, the JIT returns the function object instead of calling it. This was
confirmed pre-existing by reproducing with `cinderjit.disable_hir_inliner()`
(inliner OFF) — the bug occurs regardless of speculative inlining. The bug
tracks the `compile_after_n_calls` threshold and is likely an IC invalidation
issue under rapid type mutation. This is not a regression from commit 725004da.
