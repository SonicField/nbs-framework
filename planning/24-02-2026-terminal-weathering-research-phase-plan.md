# Terminal Weathering: Research Phase Restructuring

**Date**: 24-02-2026
**Project**: terminal-weathering
**Location**: nbs-framework
**Previous plan**: `planning/09-02-2026-terminal-weathering-plan.md` (preserved as historical record)

## Context

Terminal weathering's methodology (six phases, evidence gates, trust gradient, epistemic GC) is sound and validated across two projects. However, the 09-02-2026 plan hardcoded the architectural answer — "replace call protocol paths with C type slots" — before any investigation of the target system. This worked for SOMA (data containers → C extension types → 2x speedup) but produced no measurable effect for PyTorch (dispatch engine → body replacement → no effect; type slot replacement → also no measurable whole-system effect because the dynamism was load-bearing).

The evidence from both projects shows that the decision about *what kind of intervention to apply* is the decision that determines success or failure. The previous methodology had no phase for making this decision empirically.

## What Changed

A **Research Phase** was added before the existing weathering phases. The research phase:

1. **Profiles** the target system to determine where time is spent
2. **Classifies** the overhead mechanism (structural, dispatch, computational, algorithmic)
3. **Forms a hypothesis** with a quantitative prediction
4. **Runs a falsification experiment** to test the hypothesis
5. **Selects an approach** supported by evidence — or concludes "stop"

The weathering phases now operate parameterised by the research output, rather than hardcoding C type slot replacement.

## Evidence Base

| Source | Finding | Impact on restructuring |
|--------|---------|------------------------|
| PyTorch `nn.Module` | Call protocol overhead was real (30.4% QPS sensitivity) but type slot replacement produced no measurable whole-system effect because dynamism is load-bearing | The correct research phase output for PyTorch is "stop" — the methodology previously had no way to reach this conclusion before investing effort |
| SOMA VM | Data container field access was the overhead mechanism; C extension types produced 2.06x speedup (CV < 1.3%); Rust/PyO3 was 6% slower than Python | The correct research phase output for SOMA is "C extension types for data containers" — the previous methodology would have started with type slot replacement, which is the wrong approach for this system |
| SOMA dispatch optimisation | Phase 3 dispatch optimisation produced ~6% test suite improvement but was invisible in benchmarks dominated by O(n² log n) `remove_half` | Algorithmic overhead is a distinct category that the research phase must identify before investing in language-level optimisation |
| boundary-crossing-bench | GC tracking (16 bytes/object) caused 50% throughput reduction at L1 cache boundary; INCREF/DECREF added 0.6 ns/node independent of cache | System-specific optimisations (Layer 3) emerge from accumulated evidence, not from predetermined rules |

## Files Modified (6 files rewritten, 1 new)

| File | Change | Lines (before → after) |
|------|--------|----------------------|
| `terminal-weathering/concepts/terminal-weathering.md` | Rewritten. Added Research Phase, Architectural Patterns, Layered Progression. Replaced "Why C, Not Rust" with research-first framing | 241 → ~280 |
| `terminal-weathering/docs/methodology.md` | Rewritten. Added Research Phase before six weathering phases. Parameterised all phases by research output | 289 → ~355 |
| `terminal-weathering/docs/overview.md` | Rewritten. Emphasises hypothesis-driven optimisation. Two anti-patterns (rewriting without evidence, optimising without diagnosis) | 93 → ~84 |
| `terminal-weathering/docs/getting-started.md` | Rewritten. Prerequisites restructured (profiling first, toolchain deferred). Research phase walkthrough with worked examples | 185 → ~181 |
| `claude_tools/nbs-terminal-weathering.md` | Major rewrite. Description changed. Research phase added as Phase 1. Context detection includes `research.md`. C-specific rules conditional on approach | 555 → ~645 |
| `claude_tools/nbs-terminal-weathering-review.md` | Extended. Added Research Phase Discipline and Prediction Tracking sections. Correctness checks parameterised by approach (C vs Rust sections) | 97 → ~156 |
| `planning/24-02-2026-terminal-weathering-research-phase-plan.md` | NEW. This document | — |

## Files Unchanged (4 files)

| File | Reason |
|------|--------|
| `evidence/weathering-at-the-right-layer.md` | Historical evidence document. Now referenced as a worked example of the research process (PyTorch) |
| `evidence/soma-weathering.md` | Historical evidence document. Now referenced as a worked example (SOMA) |
| `concepts/c-extension-performance.md` | Still valid as the C extension discipline reference. Applies whenever the research phase selects C |
| `.nbs/reference/weathering-at-the-right-layer.md` | Reference copy, unchanged |

## Design Decisions

1. **Tool scope: research drives all.** Terminal weathering becomes the general methodology for hypothesis-driven Python performance work. The research phase can recommend any approach — C extension types, type slot replacement, body replacement (PyO3/Cython), algorithmic changes, or "stop". It is no longer limited to C extensions. The tool description, mandatory checks, and rules all reflect this.

2. **Archetypes are prior knowledge, not taxonomy.** The patterns observed in PyTorch (dispatch engine) and SOMA (data container) inform but do not constrain the research phase. Each system gets its own empirical investigation. The archetypes help form initial hypotheses, not classify into predetermined categories.

3. **Toolchain checks deferred.** The previous tool spec checked for C compiler + ASan + CPython headers in Phase 1 (Goal Setting), before any profiling. The restructured tool defers toolchain checks until the research phase selects an approach. If the approach is "algorithm change", no C compiler is needed.

4. **C-specific rules conditional.** `METH_VARARGS` banned, ASan mandatory, refcount discipline mandatory — all still apply, but only when the selected approach involves C extensions. When Rust is selected, equivalent safety gates (clippy, miri, boundary overhead measurement) apply instead.

5. **Prediction tracking added.** The research phase produces a quantitative prediction. The Assess phase now compares actual results against this prediction. If three consecutive conversions miss their predictions, the diagnosis must be reconsidered. This is the research phase's own evidence gate.

## Falsification of This Restructuring

This restructuring is wrong if:

1. **The research phase adds overhead that exceeds the benefit of better approach selection.** If the profiling, hypothesis formation, and falsification experiment take longer than the wasted effort they prevent, the restructuring is net negative. Falsifier: measure the wall-clock time of the research phase on the next project and compare against the previous "start converting immediately" approach.

2. **The parameterisation makes the methodology too abstract to follow.** The previous methodology was concrete: "C type slot replacement." The restructured methodology is parameterised: "the approach selected by the research phase." If this abstraction makes the tool harder to use or less effective, the restructuring is wrong. Falsifier: observe whether AI workers follow the parameterised instructions as effectively as the concrete ones.

3. **The architectural patterns become a taxonomy despite the warning.** If future users treat "data container → C extension types" as a rule rather than a hypothesis, the restructuring has failed to communicate its own principle. Falsifier: review future research phases and check whether they actually profile or just pattern-match.

4. **"Stop" is never selected.** If no project ever reaches a "stop" conclusion, either every system benefits from intervention (unlikely) or the research phase is performative (likely). Falsifier: track the ratio of "select" to "stop" outcomes across projects.

## Verification Checklist

- [ ] All 7 files reference the research phase consistently
- [ ] No file still assumes "type slot replacement" as the default approach
- [ ] Every claim traces to evidence from PyTorch, SOMA, or boundary-crossing-bench
- [ ] The research phase has its own falsifier (first cycle must match prediction)
- [ ] The restructured methodology has its own falsifier (research phase overhead vs benefit)
- [ ] Old planning document preserved as historical record
- [ ] Evidence documents referenced as worked examples, not rewritten
- [ ] Installed copies of tool specs updated
