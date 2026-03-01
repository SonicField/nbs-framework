# Session Summary: CinderX JIT Deopt Investigation

**Date:** 1 March 2026
**Terminal goal:** Get nn_module_forward (0.72x) and pytorch_cm (0.69x) to 1.25x JIT performance
**Machine:** build-host (aarch64, Grace Hopper GB200)
**Baseline:** commit 1fa46c9b, geomean ~0.98x across 24 benchmarks

---

## Key Findings

### 1. CALL Specialisation from Last Session Was Net Negative

The previous session's CALL_PY_EXACT_ARGS GuardIs specialisation (uncommitted on build-host caused:
- **kwargs_dispatch regression**: 0.98x → 0.77x
- **Additional deopts** on `_GeneratorContextManager.__exit__`
- **No improvement** on target benchmarks (nn_module_forward, pytorch_cm)

**Verdict:** CALL specialisation changes formally rejected. Must be reverted on build-host

### 2. Inliner Was Already Enabled (Not the Bottleneck)

- `PYTHONJITENABLEHIRINLINER` was unset, but `cinderjit.is_hir_inliner_enabled()` returned True — the inliner defaults to on.
- Inliner showed no improvement because target functions don't reach Tier 2 threshold (1000 invocations) under `auto()` mode at typical `n_iter` values.
- Even if Tier 2 triggered, inlining requires the caller to be compiled — not just the callee.

**Verdict:** Inlining is not blocked by configuration. It's blocked by the Tier 2 threshold for the specific benchmark call patterns.

### 3. Root Cause: Polymorphic Receiver + Simplifier Not Engaging

The warmed HIR dump of `Layer.forward()` revealed two independent problems:

#### Problem A: Polymorphic Receiver Causes Universal Deopts

`Layer.__init__()` is called with both `Layer` and `Network` (subclass) instances:

```
class Network(Layer):
    def __init__(self, name, feat, n=3):
        Layer.__init__(self, name, feat)  # self is type Network!
        self.layers = [Layer(f'{name}_{i}', feat) for i in range(n)]
```

The JIT emits `GuardType<ObjectUser[Layer:Exact]>` on `self`. This guard fails every time `self` is a `Network` instance (25% of calls). After 1000 guard failures → deopt backoff → JIT detaches from `Layer.__init__`.

**Empirically confirmed:**
- Monomorphic (only Layer, new instances per iteration) → NO deopts
- Polymorphic (Layer + Network subclass) → deopts on both `__init__` and `forward`

This is a **pre-existing JIT issue** (present in clean commit 1fa46c9b), not caused by any of our changes.

#### Problem B: Simplifier Does Not Convert LoadAttr → LoadField

Even with the warmed HIR showing `GuardType<ObjectUser[Layer:Exact]>`, `simplifyLoadAttrSplitDict` did not fire. Attribute access remains `LoadAttr` (generic CPython inline cache) instead of `LoadField` (direct split dict offset).

This means the JIT does not take advantage of split dict layout for attribute access on locally-defined classes, even when it knows the exact type. The generic `LoadAttr` path uses CPython's inline cache with type version checks, which is vulnerable to invalidation from unrelated type changes.

**Unexplained:** Why does `simplifyLoadAttrSplitDict` not engage when `GuardType<Exact>` is present? The precondition ("receiver has a known, exact type") appears to be met.

### 4. Layer.forward() Deopts Are Independently Unexplained

Theologian confirmed that `__init__` detachment does NOT cascade to `forward()` via type version invalidation — detachment modifies the function object, not the type object.

Yet `Layer.forward()` also reaches 1000 deopts in the polymorphic case. Forward is only called on `Layer` instances (never `Network`), so `GuardType<Layer:Exact>` should pass. The root cause of `forward()`'s independent guard failures remains undiagnosed.

Possible explanations:
- Type version counter invalidation from `__init__` deopts affecting `LoadAttr`'s internal CPython cache
- `SplitDictDeoptPatcher` firing due to keys version changes during instance creation
- Aggregate deopt count across warmup + steady state

### 5. Theologian's Architectural Analysis: Inlining Required for 1.25x

Even with perfect guards, CALL specialisation caps at ~0.95x (matching interpreter, not beating it). The JIT's per-call overhead (~25-35 cycles) exceeds the interpreter's CALL_PY_EXACT_ARGS fast path (~16-24 cycles).

To exceed 1.0x on call-heavy benchmarks, the JIT must **inline across call boundaries**, eliminating frame creation/teardown and enabling cross-function optimisation (LICM, constant folding).

### 6. Alex's Directive: Optimisations Default On

All JIT optimisations must be ON by default. Environment variables should only DISABLE optimisations, never enable them. This applies to `PYTHONJITENABLEHIRINLINER` and any other opt-in flags.

---

## Proposed Fix Direction

### Option C: Subtype Guard at Polymorphic Callsites (Theologian's Recommendation)

Replace `GuardType<Layer:Exact>` with an isinstance-compatible check for methods called with both parent and child instances. Keep exact-type guards for monomorphic callsites (preserving fibonacci at 2.03x).

**Safety analysis (theologian):** isinstance guards are safe for `Layer`/`Network` because subtypes add attributes at the end, preserving split dict offsets for inherited attributes. Not universally safe — a subclass that sets attributes in a different order could break offset assumptions. The general approach requires a runtime layout compatibility check.

**Gatekeeper constraint:** Fix must be targeted to polymorphic callsites only. Global weakening of guards would regress monomorphic benchmarks.

### Fix B: Eager Version Tag Assignment (Theologian's Late-Session Finding)

Even with the guard fix, `LoadAttr` (generic) instead of `LoadField` (direct) is leaving performance on the table. The warmed HIR dump confirmed that `simplifyLoadAttrSplitDict` does not fire despite `GuardType<Layer:Exact>` being present.

**Root cause hypothesis (theologian, high confidence):** The simplifier call chain (`simplifyLoadAttrInstanceReceiver` → `simplifyLoadAttrSplitDict`, simplify.cpp:1446-1475) has four preconditions. The fourth is a **version tag check** (lines 1458-1466):

- If threaded compilation is active: `Ci_Type_HasValidVersionTag(py_type)` — **read-only**, cannot assign tags
- If single-threaded: `ensureVersionTag(py_type)` — assigns tag if missing

CPython assigns version tags **lazily**. Locally-defined classes that have never triggered a version tag assignment will fail the read-only check during threaded compilation. The result: the entire split dict optimisation path is **silently skipped**, falling through to generic `LoadAttr`.

**Fix:** Eagerly assign version tags for types before JIT compilation begins (e.g. when the type is first seen by the JIT). Must be done at a thread-safe point — the read-only check exists deliberately to avoid races on `tp_version_tag` during background compilation.

**Gatekeeper constraint:** Any fix must not introduce thread-safety issues. If the version tag is assigned eagerly at a safe point (before background compilation starts), the threaded path can then use the read-only check without issue.

**Diagnostic to confirm:** Add `JIT_LOG` at simplify.cpp:1461-1465 printing type name and version tag validity. If it prints `Layer: no valid version tag`, this hypothesis is confirmed.

### Combined Three-Part Fix Plan

With both Fix A and Fix B, plus inlining:

1. **Fix A** — Subtype guard for polymorphic callsites → eliminates `__init__` deopts
2. **Fix B** — Eager version tag assignment → enables `LoadField` (direct split dict access)
3. **Inlining** — With A+B fixed, run high-n_iter benchmark for Tier 2 → eliminates call overhead

This is the most concrete path to 1.25x identified this session.

---

## What Was Falsified

| Hypothesis | Status | Evidence |
|-----------|--------|----------|
| CALL specialisation improves call-heavy benchmarks | FALSIFIED | 0.72x → 0.76x (within noise), plus kwargs_dispatch regression |
| func_version_cache collisions prevent CALL spec activation | UNTESTED | Diagnostic counters added but atexit output not captured (subprocess isolation) |
| Inliner is disabled by default | FALSIFIED | `is_hir_inliner_enabled() = True` on build-host |
| Inliner not firing = inlining doesn't help | INCONCLUSIVE | Inliner was active but Tier 2 threshold not reached; test was contaminated by CALL spec deopts |
| Unwarmed HIR dump represents auto() behaviour | FALSIFIED | Warmed dump shows GuardType; unwarmed shows LoadAttrCached (generic) |
| __init__ detachment cascades to forward() | NOT CONFIRMED | Theologian: detachment modifies function, not type; cascade mechanism unknown |
| Version tag missing prevents split dict optimisation | HIGHLY LIKELY | Theologian: threaded compile uses read-only tag check; locally-defined classes may lack tags. Diagnostic proposed but not yet run |

---

## Completed This Session

| Item | Status | Details |
|------|--------|---------|
| Statistical significance gate | IMPLEMENTED | Sign test (13/15), 2% min effect, bootstrap 95% CI. Self-tested. scripts/significance_gate.py |
| CALL spec changes review | REJECTED | Caused kwargs_dispatch regression + additional deopts |
| CinderX docs in nbs-framework | REMOVED | Commit 05d7635 (push blocked by proxy) |
| Hostname sanitisation | COMMITTED | Commit 6552405 |
| Warmed HIR dump of Layer.forward | OBTAINED | GuardType<Layer:Exact> + LoadAttr (not LoadField) |
| Polymorphic receiver diagnosis | CONFIRMED | Empirical: monomorphic = no deopts, polymorphic = deopts |
| Inliner status verification | CONFIRMED | is_hir_inliner_enabled() = True |

---

## Outstanding Items

1. **Fix A:** Implement subtype guard (Option C) for polymorphic callsites — requires JIT code change in builder.cpp
2. **Fix B:** Investigate why `simplifyLoadAttrSplitDict` doesn't fire with `GuardType<Exact>` present
3. **Layer.forward() independent deopts:** Root cause still unknown — need to identify which guard or cache check fails
4. **Clean inlining test:** Run with guards fixed + sufficient n_iter for Tier 2 — tests whether inlining delivers the 1.25x
5. **kwargs_dispatch:** Revert CALL spec changes on build-host to restore 0.98x
6. **Optimisation-default-on audit:** Check all env var gated optimisations, flip defaults
7. **Statistical gate integration:** Apply to benchmark_cinderx.py on build-host
8. **git push blocked by proxy 403:** Commits 05d7635 need manual push

---

## Session Metrics

- **Duration:** ~60 minutes active investigation
- **Team:** supervisor, theologian, generalist, testkeeper, gatekeeper, scribe (+ fixup, pythia, sidecar)
- **Agent stalls:** 6 (generalist ×3, theologian ×1, testkeeper ×1, scribe ×1) — all from notification modal stalls, recovered by fixup L1/L4
- **Hypotheses tested:** 6
- **Hypotheses falsified:** 3
- **Root causes identified:** 2 (polymorphic receiver, simplifier not engaging)
- **Root causes still open:** 1 (forward() independent deopts)
