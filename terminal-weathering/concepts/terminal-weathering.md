# Terminal Weathering

Rock does not shatter because someone decided it should be gravel. It transforms through sustained exposure — water finding existing cracks, dissolving weaker material, leaving stronger structures behind. The process is gradual, irreversible once proven, and indifferent to impatience.

Code transforms the same way, or it does not transform at all.

> **Archival note (February 2026):** The C-quality engineering standards from this methodology
> (ASan, leak analysis, refcount discipline, calling convention discipline) have been migrated
> to `concepts/engineering-standards.md` in the NBS framework. This methodology document is
> retained for reference but the engineering standards document is now the canonical source
> for C/CPython quality gates.

## The Anti-Pattern

"Rewrite for performance" is demolition. It assumes the replacement is better before evidence exists. It replaces an understood system with an unproven one in a single commitment. It is Ethos — trusting the authority of a language's reputation over measured outcomes.

The failure mode is predictable:

| Phase | What happens |
|-------|-------------|
| Announcement | "We are rewriting for performance" |
| Honeymoon | Early modules convert easily; team reports progress |
| Plateau | Complex modules resist; edge cases multiply; Python semantics prove non-trivial |
| Sunk cost | Too much invested to stop; too little working to ship |
| Outcome | Two half-working systems instead of one working one |

The root cause is not the target language. It is the absence of evidence gates. No individual conversion was required to prove its value. The decision was made once, at the top, and everything downstream was committed before falsification could occur.

But there is a subtler anti-pattern: **optimising the wrong mechanism**. Even with evidence gates, even with falsifiable hypotheses and rigorous measurement, if the intervention targets the wrong layer, the work produces correct, safe, well-measured code that does not help. The rewrite anti-pattern skips evidence. The wrong-mechanism anti-pattern skips diagnosis.

## The Metaphor

Geological weathering operates on three principles that map directly:

1. **Existing cracks first.** Water does not attack solid rock. It finds joints, faults, grain boundaries — places where the material is already weak. In code: the specific overhead mechanisms that profiling reveals.

2. **Surface inward.** Weathering works from exposed surfaces toward the interior. In code: the outermost measurable overhead first, then progressively inward as evidence accumulates.

3. **Differential erosion.** Weaker material dissolves; stronger material remains. In code: some overhead mechanisms resist intervention because they derive genuine value from Python's dynamism. This is information, not failure.

## The Research Phase

Before weathering begins, the system must be characterised. The decision about *what kind of intervention to apply* is the decision that determines success or failure. Two projects demonstrated this empirically.

**PyTorch `nn.Module`**: Four leaf functions were converted to Rust via PyO3. Correctness passed (52/52, full suite 88/88). ABBA benchmarks showed no significant performance effect (mean −1.4%, p > 0.05). The speed-bump experiment revealed 30.4% QPS sensitivity at function entry — the overhead was in CPython's call protocol dispatch chain, not in function bodies. The research diagnosis was correct: dispatch engine overhead. But type slot replacement also produced no measurable whole-system effect, because `nn.Module`'s dynamism (arbitrary `__getattr__` overrides, deep MRO hierarchies, descriptor protocol interactions) is load-bearing. The correct conclusion was "stop" — this system resists acceleration at this layer. See [evidence/weathering-at-the-right-layer.md](../evidence/weathering-at-the-right-layer.md).

**SOMA VM**: The same investigation was applied to SOMA's interpreter. The overhead mechanism was different: data container field access. A Rust/PyO3 extension was 6% *slower* than pure Python (PyO3's per-access safety abstractions exceeded any savings). A C extension using direct struct access was 2.06x faster — uniformly across all operations. The research phase correctly identified data containers as the target and C extension types as the approach. Subsequent dispatch optimisation (C builtins, inline caches, register fast paths) added a further 6% improvement. See [evidence/soma-weathering.md](../evidence/soma-weathering.md).

The difference between these outcomes was not in the weathering methodology — both projects used the same evidence gates, falsification discipline, and measurement rigour. The difference was in the initial diagnosis. PyTorch's overhead mechanism was correctly identified but could not be profitably addressed. SOMA's overhead mechanism was correctly identified *and* successfully addressed. In both cases, the research phase produced the critical information.

### The Five Steps

1. **Profile.** Where is time spent? Data access, dispatch, computation, I/O? Use profiling tools appropriate to the system (`py-spy`, `perf`, `cProfile`, `tracemalloc`, `memray`). Do not hypothesise before profiling.

2. **Classify.** What kind of overhead is this?
   - *Structural*: Object model overhead — attribute access, type checking, memory layout
   - *Dispatch*: Call protocol overhead — type slot dispatch, MRO walk, bound method creation, frame setup
   - *Computational*: Loop body overhead — the actual work inside functions
   - *Algorithmic*: Complexity overhead — O(n²) where O(n log n) is possible

3. **Hypothesise.** "The overhead mechanism is X, because Y. Intervention Z should reduce it by approximately W." Name the expected magnitude. A hypothesis without a quantitative prediction is unfalsifiable.

4. **Experiment.** Design a falsification experiment. Speed-bump tests, boundary-crossing benchmarks, synthetic workloads isolating the hypothesised mechanism. Run it. Let the result drive the approach, not the other way around.

5. **Select or Stop.** Select an architectural approach with a quantitative prediction. Or conclude that no intervention will help — the overhead is load-bearing, the algorithm dominates, the system resists acceleration. Both are valid outcomes. "Stop" is not failure; it is the research phase preventing wasted effort.

### Exit Criterion

Architectural approach selected with experimental evidence and a quantitative prediction documented. Or: a documented conclusion that no intervention will help, with the evidence that supports this.

### Falsifier

If the first weathering cycle does not produce improvement in the predicted range, reconsider the research phase conclusion. If three consecutive weathering cycles fail to match predictions, the diagnosis is wrong — return to the research phase.

## Architectural Patterns

The following patterns emerged from empirical work. They are prior knowledge — useful for forming initial hypotheses, not for classifying systems into predetermined categories. Each new system gets its own research phase.

### Data Containers

**Pattern**: The system spends most time in object field access — reading and writing attributes on Python objects millions of times per operation.

**Observed in**: SOMA (Cell/CellRef/Store/Register types). Data container field access dominated runtime.

**Effective approach**: C extension types. Replace Python classes with C structs where field access compiles to pointer dereference instead of attribute protocol traversal. `((CellObject *)cell)->value` is one instruction; `cell.value` through Python is ~80ns of attribute lookup.

**Evidence**: SOMA C extension: 2.06x faster than Rust, 1.94x faster than Python, CV < 1.3% across all operation types.

### Dispatch Engines

**Pattern**: The system's overhead is in call dispatch — the machinery that routes method calls through type slots, MRO, descriptor protocol, and frame setup.

**Observed in**: PyTorch `nn.Module`. Call protocol dispatch dominated per-call cost (~80ns dispatch vs ~50ns body).

**Effective approach**: Type slot replacement *if* the dispatch dynamism is not load-bearing. If the system relies on arbitrary `__getattr__` overrides, deep MRO hierarchies, or descriptor protocol interactions, the dispatch is doing necessary work and replacing it is not viable. The correct outcome may be "stop".

**Evidence**: PyTorch — type slot replacement hypothesis was technically correct (the overhead *was* in dispatch) but practically unproductive because the dynamism was load-bearing.

### Compute Kernels

**Pattern**: The system spends most time in loop bodies — tight inner loops doing arithmetic, comparisons, or data transformation.

**Hypothetical approach**: Body replacement via Rust/PyO3, Cython, or C. The dispatch overhead is a small fraction of total cost, so replacing the body provides direct benefit.

**Evidence**: Not yet tested empirically in this methodology. The hypothesis is that systems with heavy computation and light dispatch will benefit from body replacement, but this requires experimental validation.

### Algorithmic Overhead

**Pattern**: The system is dominated by algorithmic complexity — O(n²) where O(n log n) is possible, or unnecessary recomputation.

**Observed in**: SOMA's `remove_half` operation (O(n² log n), accounting for 96% of benchmark time at N=100).

**Effective approach**: Fix the algorithm. No amount of C conversion will help when the algorithm itself is the bottleneck.

**Evidence**: SOMA Phase 3 dispatch optimisation was invisible in benchmarks because `remove_half`'s O(n² log n) complexity dominated. All dispatch improvements were within noise (CV ~2%).

## Layered Progression

When the research phase identifies data structures as the target (the data container pattern), the weathering proceeds in layers. Each layer builds on the previous and has diminishing returns.

**Layer 0 — Data Structures.** Replace Python classes with C extension types. Field access becomes pointer dereference. This is where the largest gains appear — SOMA's 2x speedup came almost entirely from this layer.

**Layer 1 — API Surface Discipline.** Ensure the C extension uses fast calling conventions (`METH_FASTCALL`, direct struct access, interned strings). The `right_foot.fast_isinstance` function demonstrated that calling convention changes alone can recover 96ns per call (see `concepts/c-extension-performance.md`, "Evidence: right-foot" section).

**Layer 2 — Dispatch.** Replace high-frequency dispatch paths: C builtins bypassing Python method dispatch, inline caches for type checks, register fast paths. SOMA's Phase 3 achieved ~6% test suite improvement from this layer.

**Layer 3 — Specialisation.** System-specific optimisations informed by accumulated evidence: GC tracking elimination for acyclic types, borrowed reference traversal in hot loops, factory functions bypassing `tp_call`. Returns are smaller and more context-dependent.

Each layer has its own evidence gate. Diminishing returns are expected and acceptable — the question is whether the marginal benefit justifies the marginal complexity.

## The Weathering Phases

Terminal weathering is iterative, not linear. Each cycle processes one candidate through six phases, parameterised by the research phase output.

### Survey

Identify existing cracks within the domain the research phase identified. The survey targets what the research phase found — data container access paths if the pattern is structural, dispatch chains if the pattern is dispatch, loop bodies if the pattern is computational.

**Exit criterion**: Ranked list of candidates with quantified overhead.

**Falsifier**: If no measurable overhead exists in the identified domain, stop. There is nothing to weather.

### Expose

Select a single candidate from the ranked list. It must be a leaf — a unit of work whose replacement does not depend on other unreplaced units. What constitutes a "leaf" depends on the approach: a leaf type for data containers, a leaf type slot for dispatch engines, a leaf function for compute kernels.

**Exit criterion**: Single candidate selected with baseline measurements recorded.

**Falsifier**: If the candidate cannot be isolated as a leaf, it is not ready. Decompose further or choose another.

### Weather

Apply the verification cycle to the selected candidate:

1. **Design**: Implementation replacing the target at the layer identified by the research phase
2. **Plan**: Identify what could go wrong — the risks depend on the approach (reference counting for C, ownership for Rust, semantic drift for any replacement)
3. **Deconstruct**: Break into testable steps
4. **Test**: Write tests exercising the Python API through the replacement backend; write benchmarks; apply safety gates appropriate to the approach
5. **Code**: Implement the replacement; the Python layer remains until proven redundant
6. **Document**: Record baseline versus post-conversion measurements

**Exit criterion**: Tests pass, safety gates clean, benchmarks collected, Python API unchanged.

**Falsifier**: "This replacement provides measurable benefit" — attempt to falsify by benchmarking under realistic load, testing edge cases the Python implementation handled implicitly, and measuring total system impact rather than isolated component performance.

### Assess

The evidence gate. Three outcomes, no others:

1. **Benefit confirmed**: Measurements show improvement beyond noise. Safety gates clean. Mark conversion as permanent. Proceed.
2. **Benefit unclear**: Measurements are ambiguous. More data needed. Do not proceed until resolved.
3. **Benefit falsified**: Measurements show no improvement, or regression, or safety violations. Revert. Document what was learned.

Outcome 3 is not failure. It is the methodology working. A reverted conversion that taught us "this target resists replacement because of X" is more valuable than a committed conversion nobody measured.

**Falsifier**: If we cannot distinguish outcomes 1–3 with evidence, our measurement methodology is wrong. Fix that before proceeding with any conversion.

### Advance

With proven replacements, the next layer becomes accessible:

- New candidates may now be leaves, their dependencies already replaced
- Patterns emerge: which types of replacement yield benefit, which do not
- Rules of thumb develop — but each conversion still passes its own evidence gate

**Exit criterion**: Next candidate selected based on updated dependency map and accumulated evidence.

### Fuse

When sufficient contiguous coverage exists within a type or module, consider removing the Python layer entirely. This is a separate verification cycle with its own evidence gate.

Risks specific to fusion: Python-side consumers, dynamic dispatch, monkey-patching in test fixtures, implicit interface contracts, subclass slot inheritance.

**Falsifier**: "Removing the Python layer does not break any consumer" — test exhaustively. If any consumer breaks, the Python layer remains.

## C Safety Gates

When the research phase selects C extension types as the approach, additional safety gates apply. C lacks Rust's compile-time memory safety. The response is not to ignore the risk but to address it with different tools:

- **AddressSanitizer (ASan)** is mandatory during the correctness phase. Every C extension must compile and pass its test suite with ASan enabled. ASan catches heap buffer overflows, use-after-free, double-free, stack buffer overflows, and memory leaks at runtime.
- **Memory leak analysis** is mandatory at the Assess phase. Any C extension that leaks memory under the test suite fails the evidence gate, regardless of performance.
- **Reference count auditing** is required for all CPython API calls. Every `Py_INCREF` must have a corresponding `Py_DECREF` on every code path, including error paths.

These gates replace the compile-time guarantees that Rust provides. They are not optional extras. Without them, C conversion is precisely the unsupervised gambling that motivates choosing Rust.

See `concepts/c-extension-performance.md` for the full C extension cost model and calling convention discipline.

## The Trust Gradient

Human oversight is expensive. Applying full oversight to every conversion does not scale. But removing oversight without evidence is negligence.

Terminal weathering defines four oversight levels, ordered from tightest to loosest:

| Level | Oversight | When |
|-------|-----------|------|
| Tight | Human reviews every step of every conversion | Initial conversions; no evidence base yet |
| Gate | Human reviews evidence at Assess phase only | Pattern of successful conversions established |
| Batch | Human reviews evidence for batches of conversions | Strong evidence base; consistent patterns |
| Review | Human spot-checks; AI flags anomalies | Extensive track record; mature measurement infrastructure |

**Transitions are earned, not assumed.** Moving from Tight to Gate requires N consecutive conversions where the human's review found no issues the evidence gates missed. The specific N is project-dependent.

**Transitions are reversible.** A single conversion where oversight level was insufficient — the human discovers a problem the evidence gate missed — reverts the level. Trust is slow to build and fast to lose.

**The gradient applies per conversion type, not globally.** Attribute access conversions may earn Gate level while call protocol conversions remain at Tight. Each domain of conversion builds its own trust independently.

## NBS Alignment

Terminal weathering is not a new methodology. It is the existing NBS pillars applied to hypothesis-driven performance optimisation.

| Pillar | Application |
|--------|-------------|
| Goals | The terminal goal is system improvement. Language replacement is instrumental. If the system is not measurably better, the conversion has no purpose |
| Falsifiability | Each conversion carries a falsifiable claim. The research phase has its own falsifier (prediction must match first cycle results). The Assess phase exists to attempt falsification |
| Rhetoric | "C is faster" is Ethos. "Replacing data container field access with C struct dereference reduces per-access cost from 80ns to 2ns under production load" is Logos. Only the second is acceptable as evidence |
| Bullshit Detection | Report failed conversions. Report ambiguous results. Report safety gate findings. A conversion log showing 100% success rate is either dishonest or insufficiently ambitious |
| Verification Cycle | Each conversion is one full cycle: Design, Plan, Deconstruct, Test, Code, Document. Safety gates are part of Test. No shortcuts |
| Zero-Code Contract | The Engineer selects targets and defines "benefit". The Machinist implements and reports evidence. Neither trusts the other's assertions |

## NBS Teams Integration

For codebases at scale — millions of lines — terminal weathering maps onto the supervisor/worker pattern:

**Supervisor** maintains the research phase output, the ranked candidate list, assigns individual conversions to workers, aggregates evidence across conversions, detects cross-conversion patterns, and holds the evidence gates. The Supervisor decides whether a conversion passes the Assess phase — workers report, they do not adjudicate. The research phase is supervisor-led; workers are not spawned until an approach is selected.

**Workers** execute individual verification cycles. One conversion per worker. Each worker operates on an isolated branch, runs the full Weather phase, and returns evidence to the Supervisor.

The trust gradient applies at the Supervisor level. As evidence accumulates, the Supervisor may batch-assign conversions at Gate or Batch oversight levels, but retains the ability to revert to Tight for any conversion type where evidence is thin.

## The Practical Questions

1. What is the overhead mechanism? Can I point to a profile, a benchmark, a measurement — not a hunch?
2. What kind of overhead is this? Structural, dispatch, computational, algorithmic?
3. What intervention does the evidence support? C extension types, type slot replacement, body replacement, algorithm change, or "stop"?
4. Is this candidate a leaf? If not, what must be replaced first?
5. What would prove this replacement does not help? Have I tried to prove it?
6. Am I converting because of evidence, or because "C is faster" and I have not questioned that?
7. What oversight level has this type of conversion earned? What evidence supports that level?
8. What failed conversions have I documented? What did they teach me?
9. Have I run the appropriate safety gates? What did they find?
10. Does the research phase prediction match my results? If not, should I reconsider the diagnosis?

---

## Pillar Check

Have you read all pillars in this session?

- goals.md
- falsifiability.md
- rhetoric.md
- bullshit-detection.md
- verification-cycle.md
- zero-code-contract.md
- pte.md
- terminal-weathering.md *(you are here)*
- engineering-standards.md

If you cannot clearly recall reading each one, read them now. Next: `goals.md`
