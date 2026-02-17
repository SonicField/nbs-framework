---
description: Progressive Python-to-C conversion targeting CPython call protocol paths via type slot replacement
allowed-tools: Read, Write, Edit, Glob, Grep, AskUserQuestion, Bash(git:*), Bash(python:*), Bash(pytest:*), Bash(gcc:*), Bash(cc:*), Bash(clang:*), Bash(make:*), Bash(./*), Bash(perf:*), Bash(py-spy:*), Bash(hyperfine:*), Bash(valgrind:*)
---

# NBS Terminal Weathering

**MANDATORY FIRST ACTION — DO NOT SKIP**

Read these documents before proceeding:

1. `{{NBS_ROOT}}/terminal-weathering/concepts/terminal-weathering.md` — the philosophy
2. `{{NBS_ROOT}}/terminal-weathering/concepts/c-extension-performance.md` — the cost model for C extensions

This document defines what you DO.

Then detect context and dispatch.

---

## Context Detection and Dispatch

Run these checks immediately:

```
1. Check for .nbs/terminal-weathering/ directory
2. If exists, read .nbs/terminal-weathering/status.md
3. git branch --show-current
```

Dispatch based on results:

| Signal | Dispatch |
|--------|----------|
| No `.nbs/terminal-weathering/` directory | **New session** → Phase 1: Goal Setting |
| Directory exists, `candidates.md` empty or absent | **Survey needed** → Phase 2: Survey |
| Candidates ranked, on `main`/`master` branch | **Select next** → Phase 3: Expose |
| On a `weathering/*` branch | **In-progress conversion** → Phase 4: Weather (continue) |
| Conversion complete on `weathering/*` branch | **Evidence gate** → Phase 5: Assess |
| Back on main, terminal goal not met | **Advance** → Phase 6: Advance |
| Terminal goal met | **Done** → Phase 7: Final Report |

**Do not ask the human which phase to enter.** The signals are unambiguous. Detect and route.

---

## Phase 1: Goal Setting

A new terminal weathering session. No state exists yet.

**CRITICAL — C + CPYTHON TYPE API IS MANDATORY. READ THIS BEFORE ANYTHING ELSE.**

Terminal weathering targets the **CPython call protocol** via direct **C extensions** that replace type slots. Not Cython. Not ctypes. Not cffi. **C against CPython's type API. NOTHING ELSE.**

Before proceeding past this phase, you MUST verify:

```bash
# All three must succeed or you HARD STOP

# 1. C compiler available
gcc --version || clang --version

# 2. CPython headers available
python3-config --includes   # Must return a path containing Python.h

# 3. ASan available — compile and run a trivial C file with sanitisers
cat > /tmp/_asan_check.c << 'EOF'
#include <stdio.h>
int main(void) { printf("ASan OK\n"); return 0; }
EOF
cc -fsanitize=address -fsanitize=undefined -o /tmp/_asan_check /tmp/_asan_check.c && /tmp/_asan_check
rm -f /tmp/_asan_check /tmp/_asan_check.c
```

If ANY of these checks fail: **STOP. DO NOT PROCEED.** Tell the human what is missing and provide installation guidance for the specific missing component.

> **WHY C, NOT RUST?**
>
> Terminal weathering originally used Rust via PyO3. Four leaf function conversions validated the methodology — correctness passed (52/52 tests), but ABBA benchmarking showed no significant performance effect (mean -1.4%, p > 0.05). The speed-bump experiment revealed 30.4% QPS sensitivity at **function entry** — the call protocol dispatch chain, not the function bodies PyO3 replaces. PyO3 cannot access CPython's type slots (`tp_getattro`, `tp_setattro`, etc.) directly. To replace the dispatch overhead that actually matters, type slots must be installed via C against CPython's type API.
>
> Independent validation from SOMA: a Rust/PyO3 extension was 6% slower than pure Python; a C extension was 2.06x faster than Rust — uniformly across all operations. See the evidence directory for full data.
>
> See the concept document for the full evidence chain.

**What to do:**

1. **Ask the human** for the terminal goal. Not "rewrite in C" — that is instrumental. The terminal goal is a measurable system improvement:
   - "Reduce P99 latency from 45ms to 15ms"
   - "Reduce peak memory from 2GB to 500MB"
   - "Eliminate GIL contention under concurrent load"

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
**Falsifier**: [what would prove the goal is not achievable via type slot replacement]
**Phase**: Survey
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

**Status**: Awaiting survey

| Rank | Call Protocol Path | Module | Hit Count (measured) | Leaf Slot? | Baseline | Notes |
|------|-------------------|--------|---------------------|------------|----------|-------|
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

8. **Proceed to Phase 2.**

---

## Phase 2: Survey

Identify existing cracks. Not "what could be C" but "where is the dispatch overhead hurting."

**What to do:**

1. **Profile performance.** Run or ask the human to run profiling tools:
   - CPU hotspots: `py-spy`, `cProfile`, `perf`
   - Memory: `tracemalloc`, `memray`, `valgrind`
   - Latency distributions under load

2. **Analyse the call protocol.** This is not just "which functions are slow" — it is "which type slot dispatch chains have high hit counts." Identify:
   - High-frequency `tp_getattro` / `tp_setattro` dispatches
   - `slot_tp_*` chains where the slot dispatcher → MRO walk → descriptor protocol → frame setup overhead dominates the function body
   - Type slots where the Python-side function body is simple but the dispatch chain to reach it is expensive relative to the body

3. **Map the slot graph.** Identify leaf type slots — those whose replacement does not require replacing other slots first. A `tp_getattro` that internally relies on `tp_descr_get` behaviour is not a leaf unless the `tp_descr_get` behaviour is preserved.

4. **Identify code already marked problematic.** Search for TODOs, FIXME, HACK, performance comments, open issues.

5. **Rank candidates.** For each candidate, record in `candidates.md`:
   - Measured dispatch overhead (not "probably slow" — actual hit counts and timing)
   - Whether it is a leaf slot
   - The dispatch chain it replaces (e.g., `tp_getattro` → `slot_tp_getattr_hook` → `call_attribute` → `__getattr__`)
   - Baseline measurements

6. **Present ranked list to human.** Get confirmation before proceeding.

7. **Update `status.md`**: Phase → Expose.

**Falsifier**: If profiling reveals no measurable dispatch overhead, stop. There is nothing to weather. Report this honestly.

---

## Phase 3: Expose

Select a single candidate for conversion.

**What to do:**

1. **Select the highest-ranked candidate** that is:
   - A leaf type slot in the dispatch graph
   - Measurably problematic (numbers recorded)
   - Small enough to convert in one verification cycle

2. **Record baseline measurements.** These are the numbers the conversion must beat:
   - Execution time (distribution, not single run)
   - Memory usage
   - Any domain-specific metrics

3. **Check trust level** for this conversion type in `trust-levels.md`. This determines behaviour in Phase 4.

4. **Create branch:**
   ```bash
   git checkout -b weathering/<type>/<slot>
   ```

5. **Create conversion record** in `.nbs/terminal-weathering/conversions/<type>-<slot>.md`:

```markdown
# Conversion: <type>.<slot>

**Candidate Rank**: [N]
**Branch**: weathering/<type>/<slot>
**Trust Level**: [from trust-levels.md]
**Started**: [date]

## Baseline
- Dispatch chain: [e.g., tp_getattro → slot_tp_getattr_hook → call_attribute → __getattr__]
- Execution time: [measurement]
- Memory: [measurement]
- [other metrics]

## Hypothesis
"Replacing <slot> with a direct C implementation via CPython's type API will [specific measurable improvement]."

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

**Falsifier**: If the candidate cannot be isolated as a leaf slot, it is not ready. Choose another or decompose further.

---

## Phase 4: Weather

Execute the verification cycle on the selected candidate. Behaviour depends on trust level.

### Trust Level: Tight

Confirm every step with the human before proceeding.

1. **Design**: C implementation replacing a type slot directly via CPython's type API. Present design to human. Specify the **overlay mechanism** — how is the C slot installed alongside the Python implementation? (conditional installation via C extension module, runtime slot swap, dual implementation with switch).
2. **Plan**: Work through the **mandatory correctness checklist** (see below). Present plan to human.
3. **Deconstruct**: Break into testable steps. Present breakdown to human.
4. **Test**: Write tests exercising the Python API through the C-backed type slot. **Run the entire existing test suite against both implementations.** Write benchmarks. Show tests to human.
5. **Code**: Implement C extension. The Python layer remains until proven redundant. Show code to human.
6. **Document**: Record measurements in the conversion record. Show measurements to human.

#### Mandatory Correctness Checklist (Phase 4, Plan)

Before writing any code, enumerate risks in these categories. This is not optional — "identify what could go wrong" is too abstract without it.

| Category | What to check |
|----------|--------------|
| **Shared types** | Which types cross the conversion boundary? If the target shares types with unconverted code, those types must remain compatible across both implementations |
| **Reference semantics** | Does the Python code use reference/pointer indirection (e.g., objects wrapping mutable references)? These pass basic tests but break subtly under aliasing |
| **Type identity** | Does any code use `isinstance`, `type()`, or class identity checks against the target? C-backed type slot modifications must preserve type identity — `PyType_Modified` must be called after slot changes |
| **Overlay mechanism** | How will both implementations coexist? Define the switch: C extension module that installs/removes slots, conditional installation, or wrapper |
| **Canary tests** | Which existing tests exercise the conversion target most aggressively? Identify these before conversion — they are the primary regression gate |
| **Existing test suite** | The full existing test suite must pass against both the Python and C implementations. Not just new tests — all existing tests |
| **ASan gate** | All C code must compile and pass tests with `-fsanitize=address -fsanitize=undefined`. This is non-negotiable. ASan is the C equivalent of Rust's borrow checker — it catches memory safety bugs that tests miss. Without it, the trust gradient cannot advance past Tight |
| **Leak analysis** | Run under `valgrind --leak-check=full` or equivalent. Zero leaks required before proceeding to Assess. Memory leaks in C extensions are silent, cumulative, and invisible to correctness tests |
| **Refcount discipline** | Verify `Py_INCREF`/`Py_DECREF` balance. Document ownership for every `PyObject*` parameter, return value, and local variable. Refcount errors are the single biggest risk in CPython C extensions — they silently corrupt memory and may not manifest until long after the buggy code runs |

### Trust Level: Gate

Run the full verification cycle autonomously. Do not interrupt the human at each step. Present complete evidence at Phase 5 (Assess).

### Trust Level: Batch

This level applies when the supervisor assigns multiple conversions. Execute each conversion's Weather phase autonomously. Present batch evidence at Phase 5.

### Trust Level: Review

Run continuously. Only flag anomalies — unexpected test failures, performance regressions, semantic mismatches. The human spot-checks.

**For all levels:**

- Update the conversion record's Weather Log with observations at each step
- If anything unexpected occurs, stop and consult the human regardless of trust level
- The Python API must remain unchanged — the C type slot overlays, it does not replace yet
- Update `status.md` as work progresses
- **ASan, leak analysis, and refcount verification are mandatory at all trust levels.** These gates do not relax with increased trust. The trust gradient controls human oversight frequency, not safety gate strictness.

---

## Phase 5: Assess

The evidence gate. This is where conversions live or die.

**What to do:**

1. **Correctness gate (must pass before performance is considered):**
   - Full existing test suite passes against the C type slot implementation
   - Canary tests identified in Phase 4 pass
   - Shared-type compatibility verified across conversion boundary
   - Reference semantics behave identically (aliasing, mutation visibility)
   - Type identity checks (`isinstance`, `type()`) pass
   - **ASan clean**: All C code compiles and passes all tests with `-fsanitize=address -fsanitize=undefined` with zero errors
   - **Leak-free**: `valgrind --leak-check=full` (or equivalent) confirms zero leaks
   - **Refcount verified**: `Py_INCREF`/`Py_DECREF` balance documented and confirmed for every `PyObject*`
   - If the correctness gate fails, verdict is **falsified** regardless of performance

2. **Collect performance evidence:**
   - Post-conversion benchmarks (same conditions as baseline)
   - Memory measurements
   - Edge case coverage

3. **Compare against baseline.** Use statistical methods where appropriate — single-run comparisons are insufficient.

4. **Determine verdict.** Three outcomes, no others:

| Verdict | Criterion | Action |
|---------|-----------|--------|
| **Benefit confirmed** | Measurements show improvement beyond noise | Mark permanent. Merge branch. Proceed. |
| **Benefit unclear** | Measurements are ambiguous | More data needed. Do not merge. |
| **Benefit falsified** | No improvement, or regression | Revert. Document learnings. Choose next candidate. |

5. **Record verdict** in the conversion record with full evidence.

6. **If benefit falsified**: This is not failure. This is the methodology working. Document what was learned — "this call protocol path resists type slot replacement because of X" is valuable.

7. **Present verdict to human** (at all trust levels — the evidence gate always involves the human unless at Review level).

8. **Update `trust-levels.md`:**
   - Success: increment consecutive successes for this conversion type
   - Failure: reset to Tight for this conversion type, reset consecutive successes to 0

9. **Return to main branch:**
   ```bash
   git checkout main  # or master
   ```

10. **Proceed to Phase 6.**

**Falsifier**: If you cannot distinguish the three verdicts with evidence, your measurement methodology is wrong. Fix that before proceeding.

---

## Phase 6: Advance

Update the landscape and select the next candidate.

**What to do:**

1. **Update the slot graph.** Proven type slot replacements may have exposed new leaf slots.

2. **Update `candidates.md`.** Re-rank based on:
   - New leaf slots now accessible
   - Patterns from completed conversions
   - Remaining distance to terminal goal

3. **Check terminal goal progress.** Is the system measurably closer to the goal? Update `status.md`.

4. **If terminal goal met**: Proceed to Phase 7.

5. **If terminal goal not met**: Return to Phase 3 (Expose) with updated candidate list.

---

## Phase 6b: Fuse

When sufficient contiguous type slot coverage exists within a type, consider removing the Python dispatch layer entirely. This is a separate verification cycle with its own evidence gate — not an automatic consequence of successful slot replacements.

**Risks specific to fusion:**
- Python-side consumers that import or subclass the type directly
- Dynamic dispatch that routes through the Python layer
- Monkey-patching in test fixtures
- Implicit interface contracts the Python layer satisfies but the C slots do not
- Subclass slot inheritance — `PyType_Modified` must propagate changes correctly

**When to fuse:** Only after multiple contiguous slot replacements on the same type have passed their evidence gates. Fuse is opportunistic, not scheduled.

**Falsifier:** "Removing the Python layer does not break any consumer" — test exhaustively. If any consumer breaks, the Python layer remains.

---

## Phase 7: Final Report

Terminal goal achieved (or determined unachievable).

**What to do:**

1. **Compile final report** summarising:
   - Terminal goal and whether it was met
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

The supervisor holds the terminal goal, the ranked candidate list, and the evidence gates.

**Responsibilities:**
- Maintain `status.md`, `candidates.md`, `trust-levels.md`
- Select candidates and assign conversions to workers
- Adjudicate at the Assess phase — workers report evidence, the supervisor decides
- Track trust levels and adjust oversight accordingly
- Run the epistemic garbage collector (see below)
- Escalate to the human when uncertain

**The supervisor does not convert code.** She delegates, monitors, and decides.

### Conversion Workers

Each worker executes one conversion on an isolated `weathering/<type>/<slot>` branch.

**Responsibilities:**
- Execute the full Weather phase (Phase 4)
- Record observations in the conversion record
- Return evidence to the supervisor at Phase 5
- Operate within the trust level assigned by the supervisor

**Workers do not adjudicate.** They report evidence. The supervisor (and ultimately the human) decides.

### Compression Worker

A periodic, pure role that distils raw learnings into compressed patterns.

**Responsibilities:**
- Read all conversion records in `.nbs/terminal-weathering/conversions/`
- Extract patterns: which conversion types succeed, which fail, common pitfalls, useful techniques
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

**Why every 3?** Frequent enough to catch drift before it compounds. Infrequent enough not to dominate the work. This matches the nbs-teams self-check cadence.

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

**The gradient applies per conversion type, not globally.** `tp_getattro` replacements may earn Gate level while `tp_setattro` replacements remain at Tight. Each slot type builds its own trust independently.

**The trust gradient controls human oversight frequency, not safety gate strictness.** ASan, leak analysis, and refcount verification are mandatory at every level. What changes is whether the human reviews every step (Tight) or only the final evidence (Gate/Batch/Review).

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
weathering/<type>/<slot>
```

Examples:
- `weathering/module/tp_getattro`
- `weathering/pytree/tp_richcompare`
- `weathering/tensor/tp_as_number`

This enables parallel workers on different leaf slots without conflicts. Each worker operates on her own branch. Merges to main happen only after the Assess phase confirms benefit.

---

## Rules

- **C AGAINST CPYTHON'S TYPE API. MANDATORY ASAN AND LEAK GATES.** All conversions use C extensions that replace type slots directly via CPython's type API. If a C compiler, CPython headers, or ASan are not available, hard abort. Do not collapse to ctypes, cffi, or Cython under any circumstances.
- **ASan and leak analysis are non-negotiable.** All C code must compile and pass tests with `-fsanitize=address -fsanitize=undefined`. All C code must pass `valgrind --leak-check=full` with zero leaks. These are the C equivalent of Rust's borrow checker — without them, memory safety bugs are silent and catastrophic.
- **Refcount discipline is mandatory.** Every `PyObject*` must have documented ownership. Every `Py_INCREF` must have a corresponding `Py_DECREF`. Refcount errors are the single biggest risk in CPython C extensions.
- **Evidence over authority.** "C is faster" is Ethos. "This type slot replacement reduces dispatch time from 80ns to 5ns under production load" is Logos. Only the second is acceptable.
- **Leaf-first, always.** Never replace a type slot with unconverted dependencies on other slots. Decompose or wait.
- **The Python layer remains until proven redundant.** Overlay, do not replace, until evidence confirms the conversion.
- **Failed conversions are not failures.** They are the methodology working. Document and learn.
- **No blanket rules.** "`tp_getattro` replacements always help" is a hypothesis to test per candidate, not a policy.
- **Report all outcomes.** A conversion log showing 100% success rate is either dishonest or insufficiently ambitious.
- **State lives in `.nbs/terminal-weathering/`.** Not in conversation history, not in your memory. Read the files.
- **The evidence gate is non-negotiable.** Every conversion passes through Assess. No exceptions.
- **The epistemic garbage collector is mandatory.** Every 3 workers, compress and audit. No skipping.
- **Trust is slow to build and fast to lose.** One failure reverts the trust level for that conversion type.
- **When in doubt, escalate.** Ask the human rather than guess.

---

## The Contract

The human defines "benefit." The AI implements and reports evidence. Neither trusts the other's assertions — both trust evidence.

The terminal goal is system improvement. Type slot replacement is instrumental. If the system is not measurably better, the conversion has no purpose.

_Seek to falsify each conversion. Record what you observe. Let the evidence speak._
