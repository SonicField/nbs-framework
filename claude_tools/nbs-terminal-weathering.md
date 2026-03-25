---
description: Hypothesis-driven performance optimisation of Python systems through evidence-gated architectural intervention
allowed-tools: Read, Write, Edit, Glob, Grep, AskUserQuestion, Bash(git:*), Bash(python:*), Bash(pytest:*), Bash(gcc:*), Bash(cc:*), Bash(clang:*), Bash(make:*), Bash(./*), Bash(perf:*), Bash(py-spy:*), Bash(hyperfine:*), Bash(valgrind:*), Bash(cargo:*), Bash(maturin:*), Bash(rustc:*)
---

# NBS Terminal Weathering

**MANDATORY FIRST ACTION — DO NOT SKIP**

Read this document before proceeding:

1. `/home/alexturner/.nbs/terminal-weathering/concepts/terminal-weathering.md` — the philosophy

This document defines what you DO.

Then detect context and dispatch.

---

## Context Detection and Dispatch

Run these checks immediately:

```
1. Check for .nbs/terminal-weathering/ directory
2. If exists, read .nbs/terminal-weathering/status.md
3. If exists, check for .nbs/terminal-weathering/research.md
4. git branch --show-current
```

Dispatch based on results:

| Signal | Dispatch |
|--------|----------|
| No `.nbs/terminal-weathering/` directory | **New session** → Phase 0: Goal Setting |
| Directory exists, no `research.md` | **Research needed** → Phase 1: Research |
| `research.md` exists with selected approach, `candidates.md` empty or absent | **Survey needed** → Phase 2: Survey |
| Candidates ranked, on `main`/`master` branch | **Select next** → Phase 3: Expose |
| On a `weathering/*` branch | **In-progress conversion** → Phase 4: Weather (continue) |
| Conversion complete on `weathering/*` branch | **Evidence gate** → Phase 5: Assess |
| Back on main, terminal goal not met | **Advance** → Phase 6: Advance |
| Terminal goal met | **Done** → Phase 8: Final Report |

**Do not ask the human which phase to enter.** The signals are unambiguous. Detect and route.

---

## Phase 0: Goal Setting

A new terminal weathering session. No state exists yet.

**What to do:**

1. **Ask the human** for the terminal goal. Not "rewrite in C" — that is instrumental. The terminal goal is a measurable system improvement:
   - "Reduce P99 latency from 45ms to 15ms"
   - "Reduce peak memory from 2GB to 500MB"
   - "Achieve 2x throughput on the RB-tree benchmark"

2. **Confirm the goal is falsifiable.** If the human says "make it faster" — push back. Faster than what? By how much? Measured how?

3. **Create state directory:**

```
.nbs/terminal-weathering/
├── status.md
├── candidates.md
├── trust-levels.md
├── patterns.md
└── conversions/
```

4. **Write `status.md`:**

```markdown
# Terminal Weathering Status

**Terminal Goal**: [one sentence, confirmed by human]
**Falsifier**: [what would prove the goal is not achievable]
**Phase**: Research
**Started**: [date]
**Workers Since Check**: 0

## Active Workers
[none]

## Completed Conversions
[none]
```

5. **Write `candidates.md`:**

```markdown
# Conversion Candidates

**Status**: Awaiting research phase

| Rank | Target | Module | Overhead (measured) | Leaf? | Baseline | Notes |
|------|--------|--------|---------------------|-------|----------|-------|
```

6. **Write `trust-levels.md`:**

```markdown
# Trust Levels

All conversion types start at **Tight**.

| Conversion Type | Level | Consecutive Successes | Last Failure |
|----------------|-------|----------------------|--------------|
| [none yet] | Tight | 0 | — |

## Level Definitions

- **Tight**: Confirm every step with the human
- **Gate**: Run conversion autonomously, present evidence at Assess
- **Batch**: Assign multiple conversions, present batch evidence
- **Review**: Run continuously, flag anomalies only
```

7. **Write empty `patterns.md`:**

```markdown
# Compressed Patterns

**Status**: No conversions completed yet. Patterns will be distilled after first compression cycle.
```

8. **Proceed to Phase 1.**

---

## Phase 1: Research

The research phase characterises the target system and selects an architectural approach. **This phase must be completed before any weathering begins. Skipping the research phase to "just start converting" is the methodology failing.**

**What to do:**

1. **Profile the system.** Run or ask the human to run profiling tools:
   - CPU hotspots: `py-spy`, `cProfile`, `perf`
   - Memory: `tracemalloc`, `memray`, `valgrind`
   - Latency distributions under realistic load
   - Call frequency analysis

2. **Classify the overhead.** Based on the profile, determine the dominant overhead category:

| Category | Characteristics | Suggested approach |
|----------|----------------|-------------------|
| **Structural** | Object model overhead — attribute access, type checking, memory layout | C extension types |
| **Dispatch** | Call protocol — type slot dispatch, MRO walk, bound method creation, frame setup | Type slot replacement (if dynamism is not load-bearing) |
| **Computational** | Loop body overhead — the work inside functions | Body replacement (Rust/PyO3, Cython, C) |
| **Algorithmic** | Complexity — O(n²) where O(n log n) is possible | Algorithm change (no language conversion needed) |

3. **Form a hypothesis.** State it explicitly with a quantitative prediction:
   > "The overhead mechanism is **X**, because **Y**. Intervention **Z** should reduce it by approximately **W**."

4. **Design and run a falsification experiment.** The experiment must isolate the hypothesised mechanism:
   - **Speed-bump tests**: Add artificial delay at the hypothesised bottleneck
   - **Boundary-crossing benchmarks**: Measure per-crossing overhead
   - **Synthetic benchmarks**: Isolate the specific operation

5. **Select an approach or stop.**
   - If the experiment supports the hypothesis: select the approach, document the quantitative prediction
   - If the experiment falsifies the hypothesis: revise or stop
   - "Stop" is a valid outcome — it prevents wasted effort

6. **Verify the toolchain** for the selected approach:

   **For C extension types:**
   ```bash
   # All must succeed or HARD STOP for this approach
   gcc --version || clang --version
   python3-config --includes
   echo 'int main(){return 0;}' | cc -fsanitize=address -x c - -o /dev/null
   ```

   **For Rust/PyO3:**
   ```bash
   rustc --version
   cargo --version
   ```

   If the required toolchain is not available: tell the human what is missing and provide installation guidance.

7. **Write `research.md`:**

```markdown
# Research Phase

**Date**: [date]
**System**: [target system description]

## Profile Summary
[Key profiling results — where time is spent, call frequencies, memory patterns]

## Overhead Classification
**Dominant category**: [Structural / Dispatch / Computational / Algorithmic]
**Evidence**: [specific profiling data supporting this classification]

## Hypothesis
"The overhead mechanism is [X], because [Y]. Intervention [Z] should reduce it by approximately [W]."

## Falsification Experiment
**Design**: [what was tested and how]
**Result**: [what the experiment showed]
**Conclusion**: [does the result support or falsify the hypothesis?]

## Selected Approach
**Approach**: [C extension types / Type slot replacement / Body replacement / Algorithm change / STOP]
**Quantitative prediction**: [expected improvement range]
**Toolchain verified**: [yes/no, with details]

## Falsifier
If the first weathering cycle does not produce improvement in the range of [W], reconsider this conclusion.
```

8. **Read the C extension performance document** (if C is selected):
   `/home/alexturner/.nbs/terminal-weathering/concepts/c-extension-performance.md`

9. **Update `status.md`**: Phase → Survey.

10. **Proceed to Phase 2.**

---

## Phase 2: Survey

Identify existing cracks within the domain the research phase identified.

**What to do:**

1. **Profile the specific domain.** The research phase identified the overhead category. Now find the specific targets:
   - If structural: which types have the highest field access frequency?
   - If dispatch: which dispatch chains have the highest hit counts?
   - If computational: which loop bodies dominate runtime?

2. **Map the dependency graph.** Identify leaf candidates — those whose replacement does not depend on other unreplaced units.

3. **Quantify per-candidate overhead.** For each candidate, measure:
   - Per-operation cost (distribution, not single run)
   - Hit count under realistic load
   - Total contribution to runtime

4. **Rank candidates** by total overhead eliminated (frequency × per-operation cost).

5. **Record in `candidates.md`:**

| Rank | Target | Module | Overhead (measured) | Leaf? | Baseline | Notes |
|------|--------|--------|---------------------|-------|----------|-------|

6. **Present ranked list to human.** Get confirmation before proceeding.

7. **Update `status.md`**: Phase → Expose.

**Falsifier**: If profiling reveals no measurable overhead in the identified domain, stop. There is nothing to weather. Report this honestly.

---

## Phase 3: Expose

Select a single candidate for conversion.

**What to do:**

1. **Select the highest-ranked candidate** that is:
   - A leaf in the dependency graph
   - Measurably problematic (numbers recorded)
   - Small enough to convert in one verification cycle

2. **Record baseline measurements.** These are the numbers the conversion must beat:
   - Per-operation cost (distribution, not single run)
   - Hit count
   - Any domain-specific metrics

3. **Check trust level** for this conversion type in `trust-levels.md`. This determines behaviour in Phase 4.

4. **Create branch:**
   ```bash
   git checkout -b weathering/<target>/<component>
   ```

5. **Create conversion record** in `.nbs/terminal-weathering/conversions/<target>-<component>.md`:

```markdown
# Conversion: <target>.<component>

**Candidate Rank**: [N]
**Branch**: weathering/<target>/<component>
**Trust Level**: [from trust-levels.md]
**Selected Approach**: [from research.md]
**Started**: [date]

## Baseline
- Target: [what is being replaced]
- Per-operation cost: [measurement]
- Hit count: [measurement]
- [other metrics]

## Hypothesis
"Replacing <target> with [approach] will [specific measurable improvement]."

## Research Phase Prediction
[Expected improvement range from research.md]

## Falsifier
"This conversion does NOT help if [specific condition]."

## Weather Log
[To be filled during Phase 4]

## Evidence
[To be filled during Phase 5]

## Verdict
[To be filled during Phase 5]
```

6. **Proceed to Phase 4.**

**Falsifier**: If the candidate cannot be isolated as a leaf, it is not ready. Choose another or decompose further.

---

## Phase 4: Weather

Execute the verification cycle on the selected candidate. Behaviour depends on trust level.

### Trust Level: Tight

Confirm every step with the human before proceeding.

1. **Design**: Implementation using the approach from `research.md`. Present design to human.
2. **Plan**: Work through the **mandatory correctness checklist** (see below). Present plan to human.
3. **Deconstruct**: Break into testable steps. Present breakdown to human.
4. **Test**: Write tests exercising the Python API through the replacement backend. **Run the entire existing test suite against both implementations.** Write benchmarks. Show tests to human.
5. **Code**: Implement the replacement. The Python layer remains until proven redundant. Show code to human.
6. **Document**: Record measurements in the conversion record. Show measurements to human.

#### Mandatory Correctness Checklist (Phase 4, Plan)

Before writing any code, enumerate risks in these categories. This is not optional.

| Category | What to check |
|----------|--------------|
| **Shared types** | Which types cross the conversion boundary? If the target shares types with unconverted code, those types must remain compatible across both implementations |
| **Reference semantics** | Does the Python code use reference/pointer indirection? These pass basic tests but break subtly under aliasing |
| **Type identity** | Does any code use `isinstance`, `type()`, or class identity checks against the target? |
| **Overlay mechanism** | How will both implementations coexist? Define the switch mechanism |
| **Canary tests** | Which existing tests exercise the conversion target most aggressively? Identify these before conversion |
| **Existing test suite** | The full existing test suite must pass against both implementations |

**Approach-specific safety checks:**

When the selected approach involves **C extensions**:

| Category | What to check |
|----------|--------------|
| **ASan gate** | All C code must compile and pass tests with `-fsanitize=address -fsanitize=undefined`. Non-negotiable |
| **Leak analysis** | Run under `valgrind --leak-check=full`. Zero leaks required |
| **Refcount discipline** | Verify `Py_INCREF`/`Py_DECREF` balance. Document ownership for every `PyObject*` |
| **Calling convention** | Use `METH_FASTCALL`. `METH_VARARGS` is banned. `PyArg_ParseTuple` is banned. `PyBool_FromLong` is banned. See `c-extension-performance.md` |

When the selected approach involves **Rust/PyO3**:

| Category | What to check |
|----------|--------------|
| **Clippy** | `cargo clippy` must pass with no warnings |
| **Miri** | Run miri on any unsafe code |
| **Boundary overhead** | Measure per-crossing cost. If it exceeds savings, the approach is wrong |

### Trust Level: Gate

Run the full verification cycle autonomously. Do not interrupt the human at each step. Present complete evidence at Phase 5 (Assess).

### Trust Level: Batch

This level applies when the supervisor assigns multiple conversions. Execute each conversion's Weather phase autonomously. Present batch evidence at Phase 5.

### Trust Level: Review

Run continuously. Only flag anomalies — unexpected test failures, performance regressions, semantic mismatches. The human spot-checks.

**For all levels:**

- Update the conversion record's Weather Log with observations at each step
- If anything unexpected occurs, stop and consult the human regardless of trust level
- The Python API must remain unchanged — the replacement overlays, it does not remove yet
- Update `status.md` as work progresses
- **Safety gates are mandatory at all trust levels.** The trust gradient controls human oversight frequency, not safety gate strictness

---

## Phase 5: Assess

The evidence gate. This is where conversions live or die.

**What to do:**

1. **Correctness gate (must pass before performance is considered):**
   - Full existing test suite passes against the replacement implementation
   - Canary tests identified in Phase 4 pass
   - Shared-type compatibility verified across conversion boundary
   - Reference semantics behave identically
   - Type identity checks pass
   - **All approach-appropriate safety gates pass** (ASan/Valgrind/refcount for C; clippy/miri for Rust)
   - If the correctness gate fails, verdict is **falsified** regardless of performance

2. **Collect performance evidence:**
   - Post-conversion benchmarks (same conditions as baseline)
   - Memory measurements
   - Edge case coverage

3. **Compare against baseline and research phase prediction.** Use statistical methods — single-run comparisons are insufficient.

4. **Determine verdict.** Three outcomes, no others:

| Verdict | Criterion | Action |
|---------|-----------|--------|
| **Benefit confirmed** | Measurements show improvement beyond noise | Mark permanent. Merge branch. Proceed. |
| **Benefit unclear** | Measurements are ambiguous | More data needed. Do not merge. |
| **Benefit falsified** | No improvement, or regression | Revert. Document learnings. Choose next candidate. |

5. **Track prediction accuracy.** Does the actual result match the research phase prediction? If not, note the discrepancy. If three consecutive conversions miss their predictions, the research phase diagnosis must be reconsidered.

6. **Record verdict** in the conversion record with full evidence.

7. **If benefit falsified**: This is not failure. Document what was learned.

8. **Present verdict to human** (at all trust levels — the evidence gate always involves the human unless at Review level).

9. **Update `trust-levels.md`:**
   - Success: increment consecutive successes for this conversion type
   - Failure: reset to Tight for this conversion type, reset consecutive successes to 0

10. **Return to main branch:**
    ```bash
    git checkout main  # or master
    ```

11. **Proceed to Phase 6.**

**Falsifier**: If you cannot distinguish the three verdicts with evidence, your measurement methodology is wrong. Fix that before proceeding.

---

## Phase 6: Advance

Update the landscape and select the next candidate.

**What to do:**

1. **Update the dependency graph.** Proven replacements may have exposed new leaf candidates.

2. **Update `candidates.md`.** Re-rank based on:
   - New leaf candidates now accessible
   - Patterns from completed conversions
   - Remaining distance to terminal goal

3. **Check terminal goal progress.** Is the system measurably closer to the goal? Update `status.md`.

4. **If terminal goal met**: Proceed to Phase 8.

5. **If terminal goal not met**: Return to Phase 3 (Expose) with updated candidate list.

---

## Phase 7: Fuse

When sufficient contiguous coverage exists within a type or module, consider removing the Python layer entirely. This is a separate verification cycle with its own evidence gate — not an automatic consequence of successful conversions.

**Risks specific to fusion:**
- Python-side consumers that import or subclass the type directly
- Dynamic dispatch that routes through the Python layer
- Monkey-patching in test fixtures
- Implicit interface contracts the Python layer satisfies but the replacement does not
- Subclass inheritance interactions

**When to fuse:** Only after multiple contiguous replacements on the same type/module have passed their evidence gates. Fuse is opportunistic, not scheduled.

**Falsifier:** "Removing the Python layer does not break any consumer" — test exhaustively. If any consumer breaks, the Python layer remains.

---

## Phase 8: Final Report

Terminal goal achieved (or determined unachievable).

**What to do:**

1. **Compile final report** summarising:
   - Terminal goal and whether it was met
   - Research phase findings: overhead classification, selected approach, quantitative prediction
   - Prediction accuracy: how well did the research phase prediction match actual results?
   - All conversions attempted (successes, failures, reversions)
   - Total measured improvement
   - Patterns learned
   - Trust levels achieved per conversion type
   - Failed conversions and what they taught

2. **Update `status.md`**: Phase → Complete.

3. **Present to human.**

---

## The Three Roles

Terminal weathering at scale uses three roles.

### Supervisor

The supervisor holds the terminal goal, the research phase output, the ranked candidate list, and the evidence gates.

**Responsibilities:**
- Lead the research phase (workers are not spawned until an approach is selected)
- Maintain `status.md`, `candidates.md`, `trust-levels.md`, `research.md`
- Select candidates and assign conversions to workers
- Adjudicate at the Assess phase — workers report evidence, the supervisor decides
- Track trust levels and adjust oversight accordingly
- Track research phase prediction accuracy
- Run the epistemic garbage collector (see below)
- Escalate to the human when uncertain

**The supervisor does not convert code.** She delegates, monitors, and decides.

### Conversion Workers

Each worker executes one conversion on an isolated `weathering/<target>/<component>` branch.

**Responsibilities:**
- Execute the full Weather phase (Phase 4), including mandatory safety gates
- Record observations in the conversion record
- Return evidence to the supervisor at Phase 5
- Operate within the trust level assigned by the supervisor

**Workers do not adjudicate.** They report evidence. The supervisor (and ultimately the human) decides.

### Compression Worker

A periodic, pure role that distils raw learnings into compressed patterns.

**Responsibilities:**
- Read all conversion records in `.nbs/terminal-weathering/conversions/`
- Extract patterns: which conversion types succeed, which fail, common pitfalls, useful techniques
- Track research phase prediction accuracy
- Write compressed patterns to `patterns.md`
- This is a pure function: raw learnings in, compressed patterns out

**The compression worker does not make decisions.** She summarises.

---

## Epistemic Garbage Collector

Every 3 conversion workers, the supervisor must:

1. **Spawn a compression worker** to distil `conversions/` → `patterns.md`
2. **Run `/nbs`** for goal alignment and drift detection
3. **Reset the counter** (`workers_since_check` in `status.md` → 0)

This is mandatory, not optional. The counter is tracked in `status.md`. The compression worker is pure — she handles pattern extraction. `/nbs` handles the epistemic audit separately.

**Why every 3?** Frequent enough to catch drift before it compounds. Infrequent enough not to dominate the work.

---

## Trust Gradient Runtime Behaviour

The trust gradient is tracked in `trust-levels.md` and adjusts tool behaviour per level.

### Level Transitions

| Transition | Requirement |
|-----------|------------|
| Tight → Gate | N consecutive successes where human review found no issues the evidence gate missed. N is project-dependent — ask the human. |
| Gate → Batch | Further consecutive successes at Gate level. Supervisor may batch-assign. |
| Batch → Review | Extensive track record. Mature measurement infrastructure. Human explicitly approves. |
| Any → Tight | Single failure where oversight level was insufficient — the human discovers a problem the evidence gate missed. |

**Transitions are earned, not assumed.** The human can say "get on with it" to signal readiness for transition, but only if evidence supports it.

**The gradient applies per conversion type, not globally.** Each domain builds its own trust independently.

**The trust gradient controls human oversight frequency, not safety gate strictness.** Safety gates are mandatory at every level. What changes is whether the human reviews every step (Tight) or only the final evidence (Gate/Batch/Review).

### Behavioural Adjustments

| Level | Phase 4 (Weather) | Phase 5 (Assess) | Human Interaction |
|-------|-------------------|-------------------|-------------------|
| **Tight** | Confirm every step | Present all evidence | Continuous |
| **Gate** | Run autonomously | Present evidence at gate | At Assess only |
| **Batch** | Run autonomously, multiple | Present batch evidence | At batch Assess |
| **Review** | Run continuously | Flag anomalies only | On exception |

---

## Branch Pattern

All conversion work happens on branches following this pattern:

```
weathering/<target>/<component>
```

Examples:
- `weathering/cell/c-extension-type`
- `weathering/store/c-extension-type`
- `weathering/module/tp_getattro`
- `weathering/pytree/loop-body`

This enables parallel workers on different candidates without conflicts. Each worker operates on her own branch. Merges to main happen only after the Assess phase confirms benefit.

---

## Rules

- **The research phase must be completed before any weathering begins.** Skipping the research phase to "just start converting" is the methodology failing. There is no shortcut.
- **Evidence over authority.** "C is faster" is Ethos. "This replacement reduces per-operation cost from 80ns to 5ns under production load" is Logos. Only the second is acceptable.
- **The approach is determined by evidence, not assumption.** The research phase selects the approach. If the research phase concludes "stop", that is the correct outcome. Do not override it.
- **Leaf-first, always.** Never replace a target with unconverted dependencies. Decompose or wait.
- **The Python layer remains until proven redundant.** Overlay, do not replace, until evidence confirms the conversion.
- **Failed conversions are not failures.** They are the methodology working. Document and learn.
- **No blanket rules.** "This type of replacement always works" is a hypothesis to test per candidate, not a policy.
- **Report all outcomes.** A conversion log showing 100% success rate is either dishonest or insufficiently ambitious.
- **State lives in `.nbs/terminal-weathering/`.** Not in conversation history, not in your memory. Read the files.
- **The evidence gate is non-negotiable.** Every conversion passes through Assess. No exceptions.
- **The epistemic garbage collector is mandatory.** Every 3 workers, compress and audit. No skipping.
- **Trust is slow to build and fast to lose.** One failure reverts the trust level for that conversion type.
- **When in doubt, escalate.** Ask the human rather than guess.

**When the selected approach involves C extensions:**
- **C against CPython's type API. Mandatory ASan and leak gates.** If a C compiler, CPython headers, or ASan are not available, hard abort for C work.
- **ASan and leak analysis are non-negotiable.** All C code must compile and pass tests with `-fsanitize=address -fsanitize=undefined`. All C code must pass `valgrind --leak-check=full` with zero leaks.
- **Refcount discipline is mandatory.** Every `PyObject*` must have documented ownership. Every `Py_INCREF` must have a corresponding `Py_DECREF`.
- **Calling convention discipline is mandatory.** `METH_FASTCALL` for all functions. `METH_VARARGS` is banned. `PyArg_ParseTuple` is banned. `PyBool_FromLong` is banned. See `c-extension-performance.md`.

---

## The Contract

The human defines "benefit." The AI implements and reports evidence. Neither trusts the other's assertions — both trust evidence.

The terminal goal is system improvement. The architectural approach is determined by the research phase. Language replacement is instrumental. If the system is not measurably better, the conversion has no purpose.

_Characterise the system. Form a hypothesis. Test it. Let the evidence decide._
