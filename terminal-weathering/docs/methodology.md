# Methodology

## The Research Phase

Before any weathering begins, the target system must be characterised. The research phase determines *what kind of intervention to apply* — or whether any intervention will help at all. Skipping this phase to "just start converting" is the methodology failing.

### Step 1: Profile

Where is time spent? Use profiling tools appropriate to the system:

- **CPU hotspots**: `py-spy`, `cProfile`, `perf`
- **Memory**: `tracemalloc`, `memray`, `valgrind`
- **Latency distributions**: under realistic load, not synthetic microbenchmarks
- **Call frequency**: which functions or operations are invoked millions of times?

Do not hypothesise before profiling. The profile drives the hypothesis, not the other way around.

### Step 2: Classify

What kind of overhead does the profile reveal?

| Category | Characteristics | Example |
|----------|----------------|---------|
| **Structural** | Object model overhead — attribute access, type checking, memory layout | SOMA: millions of `cell.value` accesses through Python's attribute protocol |
| **Dispatch** | Call protocol overhead — type slot dispatch, MRO walk, bound method creation, frame setup | PyTorch: `tp_getattro` → `slot_tp_getattr_hook` → `_PyType_Lookup` → `call_attribute` → frame → body → teardown |
| **Computational** | Loop body overhead — the work inside functions dominates | Tight numerical loops, data transformation |
| **Algorithmic** | Complexity overhead — O(n²) where O(n log n) is possible | SOMA: `remove_half` at O(n² log n), accounting for 96% of benchmark time |

Most systems have multiple overhead categories. The classification identifies which is dominant and therefore which intervention will have the largest effect.

### Step 3: Hypothesise

Form a falsifiable hypothesis with a quantitative prediction:

> "The overhead mechanism is **X**, because **Y**. Intervention **Z** should reduce it by approximately **W**."

Examples:
- "The overhead is structural (attribute access on Cell/CellRef), because py-spy shows 60% of time in `__getattr__`. C extension types should reduce per-access cost from ~80ns to ~5ns, yielding approximately 2x overall speedup." (SOMA — confirmed)
- "The overhead is in dispatch (call protocol for `nn.Module.__getattr__`), because the speed-bump experiment shows 30.4% QPS sensitivity at function entry. Type slot replacement should reduce per-call cost from ~130ns to ~50ns." (PyTorch — the per-call prediction was correct, but the whole-system effect was unmeasurable because dispatch dynamism was load-bearing)

A hypothesis without a quantitative prediction is unfalsifiable and therefore useless.

### Step 4: Experiment

Design a falsification experiment that isolates the hypothesised mechanism:

- **Speed-bump tests**: Add artificial delay at the hypothesised bottleneck. If the system is sensitive, the mechanism is confirmed. If insensitive, the hypothesis is wrong.
- **Boundary-crossing benchmarks**: Measure the cost of crossing between Python and a compiled implementation. If crossing cost exceeds the savings, the approach is net negative.
- **Synthetic benchmarks**: Isolate the specific operation (field access, dispatch, computation) and measure it in controlled conditions.

Run the experiment. Let the result drive the approach.

### Step 5: Select or Stop

Two valid outcomes:

1. **Select**: Choose an architectural approach with a quantitative prediction. Document the evidence that supports it.
2. **Stop**: Conclude that no intervention will help — the overhead is load-bearing, the algorithm dominates, the system resists acceleration. Document the evidence that supports this.

"Stop" is not failure. It is the research phase preventing wasted effort. PyTorch's research phase correctly concluded "stop" — the overhead was real but could not be profitably addressed.

### Exit Criterion

Architectural approach selected with experimental evidence, or a documented conclusion that no intervention will help. In either case, the evidence is recorded in `research.md`.

### Falsifier

If the first weathering cycle does not produce improvement in the predicted range, reconsider the research phase conclusion. If three consecutive weathering cycles fail to match predictions, the diagnosis is wrong — return to the research phase.

---

## The Six Weathering Phases

Terminal weathering is iterative, not linear. Each cycle processes one candidate through six phases. The cycle repeats until the terminal goal is met or determined unachievable.

The unit of work is determined by the research phase output. For data containers, the unit is a type. For dispatch engines, the unit is a type slot or call protocol path. For compute kernels, the unit is a function body. The phases below use parameterised language — "candidate" and "replacement" rather than "type slot" or "C implementation" — because the specific mechanism depends on the selected approach.

### Survey

Identify existing cracks within the domain the research phase identified.

**Activities:**
- Profile the specific domain: data container access patterns if the research identified structural overhead; dispatch chains if dispatch overhead; loop bodies if computational overhead
- Map the dependency graph. Identify leaf candidates — those whose replacement does not depend on other unreplaced units
- Quantify per-candidate overhead: hit counts, per-operation cost, total contribution to runtime
- Search for code already marked problematic: TODOs, FIXMEs, HACKs, performance comments, open issues

**Exit criterion:** Ranked list of candidates with quantified overhead. Each candidate identifies the target, the overhead mechanism, and baseline measurements.

**Falsifier:** If no measurable overhead exists in the domain the research phase identified, stop. There is nothing to weather. Report this honestly — it is not failure, it is the survey doing its job.

### Expose

Select a single candidate from the ranked list.

The candidate must be:
- A leaf in the dependency graph: no deeper dependencies that must be replaced first
- Measurably problematic — not "might be slow" but "accounts for X% of runtime, measured"
- Small enough to convert in one verification cycle

Baseline measurements are recorded: per-operation cost (distribution, not single run), hit count under realistic load, and any domain-specific metrics. These are the numbers the conversion must beat.

Work happens on an isolated branch:
```
weathering/<target>/<component>
```

A conversion record is created in `.nbs/terminal-weathering/conversions/` containing the hypothesis and its falsifier.

**Exit criterion:** Single candidate selected with baseline measurements recorded.

**Falsifier:** If the candidate cannot be isolated as a leaf in the dependency graph, it is not ready. Decompose further or choose another.

### Weather

Apply the verification cycle to the selected candidate.

The six steps of the verification cycle apply directly:

1. **Design** — Implementation replacing the target using the approach selected by the research phase. For C extension types: C struct with direct field access. For type slot replacement: C function installed at the slot level via `PyType_Modified`. For body replacement: compiled implementation via the appropriate toolchain.
2. **Plan** — Identify what could go wrong. The risk profile depends on the approach:
   - *C extensions*: Reference counting errors, exception propagation, descriptor protocol compliance, MRO invalidation, thread safety under free-threaded CPython
   - *Rust/PyO3*: Ownership semantics, GIL interactions, boundary crossing overhead
   - *Any replacement*: Semantic drift between Python and compiled implementation, edge cases the Python version handles implicitly
3. **Deconstruct** — Break the conversion into testable steps. Each step is small enough to verify independently.
4. **Test** — Write tests exercising the Python API through the replacement backend. Write benchmarks. Apply safety gates appropriate to the approach:
   - *C extensions*: ASan verification (mandatory), leak analysis (mandatory), refcount verification (mandatory)
   - *Rust/PyO3*: `cargo clippy`, miri for unsafe code, PyO3 safety checks
   - *All approaches*: Full existing test suite against both implementations
5. **Code** — Implement the replacement. The Python layer remains — the replacement overlays, it does not remove the Python module. Consumers continue to import from Python. The replacement is internal.
6. **Document** — Record measurements in the conversion record. Baseline versus post-conversion, under the same conditions. Include safety gate output as evidence artefacts.

The level of human interaction during Weather depends on the trust level (see below).

**Exit criterion:** Tests pass, safety gates clean, benchmarks collected, Python API unchanged.

**Falsifier:** "This conversion provides measurable benefit" — attempt to falsify by benchmarking under realistic load, testing edge cases the Python implementation handled implicitly, and measuring total system impact rather than isolated component performance.

### Assess

The evidence gate. Every conversion passes through this phase. No exceptions.

**Mandatory checks (approach-dependent):**

| Check | Criterion | Gate |
|-------|-----------|------|
| **Correctness** | All tests pass through the replacement backend | Hard gate |
| **Safety gates** | All approach-appropriate safety checks pass (ASan for C, clippy/miri for Rust, etc.) | Hard gate |
| **Memory safety** | No leaks, no undefined behaviour (Valgrind for C, miri for Rust) | Hard gate |
| **Performance** | Measurements show improvement beyond noise | Evidence gate |

**Three outcomes for performance, no others:**

| Verdict | Criterion | Action |
|---------|-----------|--------|
| **Benefit confirmed** | Measurements show improvement beyond noise | Mark permanent. Merge branch. |
| **Benefit unclear** | Measurements are ambiguous | More data needed. Do not merge. |
| **Benefit falsified** | No improvement, or regression | Revert. Document learnings. |

A conversion that passes all hard gates but fails the performance evidence gate is still reverted — correct replacement code that provides no benefit is complexity without value. However, the correctness evidence is preserved.

**"Benefit falsified" is not failure.** It is the methodology working.

**Prediction tracking:** Compare the actual result against the research phase prediction. If the result does not match the predicted range, flag this. If three consecutive conversions miss their predictions, the research phase diagnosis must be reconsidered.

Evidence quality matters. Single-run comparisons are insufficient. Use statistical methods — distributions, confidence intervals, multiple runs under varying load.

After the verdict:
- Update the conversion record with full evidence
- Update `trust-levels.md` (success increments consecutive count; failure resets to Tight)
- Return to main branch

### Advance

Update the landscape and select the next candidate.

With proven replacements, the dependency graph changes:
- New candidates become accessible: dependencies already replaced
- Patterns emerge: which types of replacement yield benefit, which do not
- The candidate list is re-ranked based on newly accessible candidates, accumulated evidence, and remaining distance to the terminal goal

No blanket rules. "This type of replacement always works" is a hypothesis to test per candidate, not a policy to apply wholesale.

**Exit criterion:** Next candidate selected based on updated dependency graph and accumulated evidence.

### Fuse

When sufficient contiguous coverage exists within a type or module, consider removing the Python layer entirely. This is a separate verification cycle with its own evidence gate — not an automatic consequence of successful conversions.

**What fusion means in practice:** When all significant components of a type or module have been replaced, the Python definitions can be removed. The replacement becomes the primary implementation. Consumers still import from the same module — the interface is unchanged.

**Risks specific to fusion:**
- Python-side consumers that subclass the type
- Dynamic dispatch that routes through the Python layer
- Monkey-patching in test fixtures
- Implicit interface contracts that the Python layer satisfies but the replacement does not
- MRO interactions with other Python types in the hierarchy

**Falsifier:** "Removing the Python layer does not break any consumer" — test exhaustively. If any consumer breaks, the Python layer remains.

---

## The Trust Gradient

Human oversight is expensive. Applying full oversight to every step of every conversion does not scale. But removing oversight without evidence is negligence.

### What Enables Trust

The trust gradient controls human oversight frequency, not safety gate strictness. Safety gates (ASan for C, clippy/miri for Rust, full test suite for all approaches) are mandatory at every level. What changes is whether the human reviews every step or only the final evidence.

### The Four Levels

| Level | Oversight | When |
|-------|-----------|------|
| **Tight** | Human reviews every step of every conversion | Initial conversions; no evidence base yet |
| **Gate** | Human reviews evidence at Assess phase only | Pattern of successful conversions established |
| **Batch** | Human reviews evidence for batches of conversions | Strong evidence base; consistent patterns |
| **Review** | Human spot-checks; AI flags anomalies | Extensive track record; mature measurement infrastructure |

### Behavioural Differences

| Level | During Weather | During Assess | Human Interaction |
|-------|---------------|---------------|-------------------|
| **Tight** | Confirm every step | Present all evidence | Continuous |
| **Gate** | Run autonomously | Present evidence at gate | At Assess only |
| **Batch** | Run autonomously, multiple | Present batch evidence | At batch Assess |
| **Review** | Run continuously | Flag anomalies only | On exception |

### Transitions

**Transitions are earned, not assumed.**

| Transition | Requirement |
|-----------|------------|
| Tight → Gate | N consecutive successes where human review found no issues the evidence gate missed. N is project-dependent — ask the human. |
| Gate → Batch | Further consecutive successes at Gate level. Supervisor may batch-assign. |
| Batch → Review | Extensive track record. Mature measurement infrastructure. Human explicitly approves. |
| Any → Tight | Single failure where oversight level was insufficient — the human discovers a problem the evidence gate missed. |

**Transitions are reversible.** A single conversion where the oversight level was insufficient — where the human discovers a problem the evidence gate missed — reverts the level. Trust is slow to build and fast to lose.

**The gradient applies per conversion type, not globally.** Each domain of conversion builds its own trust independently.

---

## The Epistemic Garbage Collector

Raw conversion records accumulate. Patterns hide in the noise. Without periodic compression, the evidence base becomes unwieldy and drift goes undetected.

### The Mechanism

Every three conversion workers, the supervisor must:

1. **Spawn a compression worker.** This is a pure role — it reads all conversion records in `.nbs/terminal-weathering/conversions/`, extracts patterns (which replacements succeed, which fail, common pitfalls, useful techniques), and writes compressed patterns to `patterns.md`. It does not make decisions. It summarises.

2. **Run `/nbs`.** The standard NBS audit checks goal alignment, falsifiability discipline, and drift detection. This is separate from the compression worker — compression handles patterns, `/nbs` handles epistemics.

3. **Reset the counter.** `workers_since_check` in `status.md` returns to zero.

### Why Every Three

Frequent enough to catch drift before it compounds. Infrequent enough not to dominate the work. This matches the NBS teams self-check cadence.

### What Good Compression Looks Like

- "Three out of four C extension type conversions showed >2x reduction in per-access overhead. The exception was a type with dynamic attribute generation that required the Python descriptor protocol."
- "All C conversions required explicit reference count management for the return value. Two out of five initial implementations had refcount leaks caught by Valgrind."
- "Research phase prediction accuracy: 4/5 conversions fell within the predicted range. The outlier was a type where the profile underestimated cold-cache effects."

### What Bad Compression Looks Like

- "Conversions are going well." (No falsifiable content.)
- "C is faster than Python." (Ethos, not evidence.)

---

## NBS Teams Integration

For codebases at scale — millions of lines — terminal weathering maps onto the supervisor/worker pattern.

### Supervisor

The supervisor holds the terminal goal, the research phase output, the ranked candidate list, and the evidence gates.

**Responsibilities:**
- Lead the research phase (workers are not spawned until an approach is selected)
- Maintain `status.md`, `candidates.md`, `trust-levels.md`, `research.md`
- Select candidates and assign individual conversions to workers
- Adjudicate at the Assess phase — workers report evidence, the supervisor decides
- Track trust levels and adjust oversight accordingly
- Track prediction accuracy — compare each conversion result against the research phase prediction
- Run the epistemic garbage collector every three workers
- Escalate to the human when uncertain

**The supervisor does not write code.** It delegates, monitors, and decides.

### Conversion Workers

Each worker executes one conversion on an isolated branch.

**Responsibilities:**
- Execute the full Weather phase, including mandatory safety gates
- Record observations in the conversion record
- Return evidence to the supervisor at Assess
- Operate within the trust level assigned by the supervisor

**Workers do not adjudicate.** They report evidence. The supervisor (and ultimately the human) decides.

### Compression Worker

A periodic, pure role that distils raw learnings into compressed patterns. Spawned by the supervisor every three conversion workers as part of the epistemic garbage collector.

**Responsibilities:**
- Read all conversion records
- Extract patterns: which replacements succeed, which fail, common pitfalls, safety gate findings, useful techniques
- Track research phase prediction accuracy
- Write compressed patterns to `patterns.md`

**The compression worker does not make decisions.** It summarises.

---

## Evidence Gates

### What Good Evidence Looks Like

- Benchmarks run under realistic load, not synthetic microbenchmarks alone
- Statistical distributions, not single-run numbers
- Memory measurements under sustained operation, not just peak
- Safety gate output showing clean results (ASan for C, clippy/miri for Rust)
- Edge cases explicitly tested — the ones the Python implementation handled implicitly
- Total system impact measured, not just isolated component performance
- Comparison conditions identical to baseline (same hardware, same data, same load, same Python version)
- Research phase prediction compared against actual result

### What Bad Evidence Looks Like

- "It feels faster" (not measured)
- A single benchmark run showing 2x improvement (not statistically significant)
- Microbenchmark in isolation without system-level measurement (does not capture integration costs)
- Missing edge case coverage
- Different conditions from baseline (invalidates comparison)
- Safety gates not run ("it compiled cleanly" is not evidence of memory safety)
- Research phase prediction not tracked (how do we know the diagnosis was correct?)

### Failed Conversions as Positive Outcomes

A conversion that fails the evidence gate teaches something. Document it:

- **What was the hypothesis?** "Replacing Cell with a C extension type will reduce per-access overhead by 90%."
- **What did the evidence show?** "Per-access overhead reduced by 15%, within noise. The attribute protocol is only three levels deep for this type — most overhead is in the descriptor protocol, which remains in Python."
- **What did we learn?** "Shallow attribute chains are poor candidates for this approach. Target types where the access depth exceeds five levels, or where the access frequency is extremely high."

This is more valuable than a successful conversion that nobody examined critically. A conversion log showing 100% success rate is either dishonest or insufficiently ambitious.
