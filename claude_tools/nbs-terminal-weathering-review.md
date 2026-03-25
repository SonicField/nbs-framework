---
description: Correctness review for terminal weathering sessions
allowed-tools: Read, Glob, Grep, AskUserQuestion, Bash(git log:*), Bash(git status:*), Bash(git diff:*), Bash(git branch:*)
---

# Terminal Weathering Review

You are reviewing a terminal weathering session. This review is dispatched by `/nbs` when it detects a `weathering/*` branch or `.nbs/terminal-weathering/` directory.

**Apply this review IN ADDITION TO the normal NBS review, not instead of it.**

Read these documents if you have not already this session:

1. `~/.nbs/terminal-weathering/concepts/terminal-weathering.md` — the philosophy

---

## Research Phase Discipline

The research phase must be completed before any weathering begins. Check for these failure modes:

| Check | What to look for |
|-------|-----------------|
| **Research phase completed** | Does `research.md` exist? Was it completed before any conversion work began? If conversions exist but `research.md` does not, the methodology was violated. |
| **Profiling evidence** | Was the system profiled before a hypothesis was formed? Look for profiling data in `research.md`. If the hypothesis was formed without profiling, the research phase was skipped. |
| **Overhead classification** | Is the overhead classified (structural, dispatch, computational, algorithmic)? Is the classification supported by profiling evidence? |
| **Falsifiable hypothesis** | Does the hypothesis include a quantitative prediction? "C will be faster" is unfalsifiable. "C extension types should reduce per-access cost from ~80ns to ~5ns" is falsifiable. |
| **Falsification experiment** | Was an experiment designed and run to test the hypothesis? Was the result used to drive the approach? Or was the approach assumed before any experiment? |
| **Approach justified by evidence** | Is the selected approach (C extension types, type slot replacement, body replacement, algorithm change, stop) supported by experimental evidence? Or was it assumed because "that's what terminal weathering does"? |
| **"Stop" considered** | Was "stop" considered as a valid outcome? If not, the research phase may have been performative rather than genuine. |

---

## Prediction Tracking

The research phase produces a quantitative prediction. Track whether actual results match.

| Check | What to look for |
|-------|-----------------|
| **Prediction exists** | Does `research.md` contain a quantitative prediction (expected improvement range)? |
| **Results compared** | Are conversion results in the Assess phase compared against the research phase prediction? |
| **Discrepancies flagged** | If results do not match the prediction, is the discrepancy noted? |
| **Diagnosis reconsidered** | If three or more consecutive conversions miss their predictions, has the research phase diagnosis been reconsidered? Or is the team pressing forward despite evidence that the diagnosis is wrong? |

---

## Correctness Checks (Approach-Parameterised)

Read `.nbs/terminal-weathering/research.md` to determine the selected approach. Apply the checks appropriate to that approach.

### For All Approaches

| Check | What to look for |
|-------|-----------------|
| **Shared types** | Are types crossing the conversion boundary identified? Are they verified compatible across both implementations? |
| **Reference semantics** | Has reference/pointer indirection been analysed? Do aliasing and mutation visibility behave identically? |
| **Type identity** | Have `isinstance`, `type()`, and class identity checks been verified against the replacement? |
| **Overlay mechanism** | Is there a clear mechanism for installing and removing the replacement alongside the Python implementation? |
| **Existing test suite** | Has the full existing test suite been run against both implementations? Not just new tests. |
| **Correctness vs performance** | Is the Assess phase checking correctness before performance? Correctness gate must pass before performance is even considered. |
| **Failed conversion analysis** | Are failed/reverted conversions documented with what they taught? Are negative results being reported? |

### When C Extensions Are Selected

| Check | What to look for |
|-------|-----------------|
| **ASan cleanliness** | Does all C code pass tests when compiled with `-fsanitize=address -fsanitize=undefined`? ASan is the C equivalent of Rust's borrow checker — without it, memory safety bugs are invisible. Non-negotiable. |
| **Leak analysis** | Has `valgrind --leak-check=full` (or equivalent) confirmed zero leaks? Memory leaks in C extensions are silent, cumulative, and invisible to correctness tests. |
| **Refcount discipline** | Is `Py_INCREF`/`Py_DECREF` balance documented and verified for every `PyObject*`? Every parameter, return value, and local variable holding a `PyObject*` must have documented ownership semantics (borrowed vs owned). |
| **Calling convention discipline** | Is `METH_FASTCALL` used for all functions taking positional arguments? Is `PyArg_ParseTuple` absent? Is `Py_BuildValue` absent for single return values? Is `PyBool_FromLong` absent? See `c-extension-performance.md` for the full cost model. |
| **PyType_Modified** | Has `PyType_Modified` been called after slot changes to propagate through the MRO? |

### When Rust/PyO3 Is Selected

| Check | What to look for |
|-------|-----------------|
| **Clippy clean** | Does `cargo clippy` pass with no warnings? |
| **Miri clean** | Has miri been run on any unsafe code? |
| **Boundary overhead measured** | Has the per-crossing overhead been measured? If it exceeds savings, the approach is wrong (this is what happened with SOMA Rust — PyO3 getter overhead exceeded Python attribute access). |
| **GIL interactions** | Are GIL token interactions correct? Are there unnecessary GIL acquisitions in hot paths? |

---

## Leaf Discipline

AIs revert to traditional development thinking under context pressure. This is the most common drift pattern in terminal weathering. Watch for these specific failure modes:

| Drift | Correction |
|-------|-----------|
| **Converting coupled targets instead of leaves** | A leaf is a single unit of work — one type, one slot, one function body. If the candidate depends on other unconverted targets, mock the boundary. Do not convert the dependency too. |
| **Avoiding mocks to "do it properly"** | Mocks at the conversion boundary are not a shortcut — they are the methodology. Each leaf must be proven correct in isolation before fusing. |
| **Treating boundary overhead as a problem** | Boundary crossings during the correctness phase have different characteristics than the overhead being eliminated. During correctness, boundary overhead is irrelevant. If the AI is optimising for performance before all leaves are correct, it has lost the plot. |
| **Fusing before correctness is proven** | Correctness → Fuse → Performance is a phase sequence, not a balance. Fusing is only permitted after every leaf passes its correctness gate independently. |
| **Conflating correctness and performance** | "It works but it's slower" is a **success** during the correctness phase. If the AI treats this as failure, it has confused the terminal goal of the current phase. |
| **Skipping the research phase** | If conversions exist but `research.md` does not, or if `research.md` was written after conversions began, the methodology was violated. The research phase must come first. |

### C-Extension-Specific Drift (when C is selected)

| Drift | Correction |
|-------|-----------|
| **Skipping ASan because "it compiles fine"** | Compiling without errors is necessary but not sufficient. C code that compiles cleanly can contain use-after-free, buffer overflows, and undefined behaviour that only ASan catches. |
| **Undocumented refcount ownership** | Every `PyObject*` must have documented ownership. If the AI is writing C that manipulates Python objects without documenting who owns each reference, it is accumulating silent refcount bugs. |
| **Tutorial calling conventions** | `METH_VARARGS`, `PyArg_ParseTuple`, `Py_BuildValue` for single values, and `PyBool_FromLong` are the patterns most represented in training data. They are also the slow patterns. See `c-extension-performance.md`. |
| **Reflexive INCREF/DECREF in traversal loops** | In tight traversal loops over C-struct-backed types where the GIL guarantees object lifetime, per-node INCREF/DECREF is 0.6 ns/node of unnecessary overhead. Verify the AI has considered the lifetime guarantee rather than reflexively refcounting. See `c-extension-performance.md`. |

---

## The Phase Separation

Terminal weathering has two distinct phases that must not be conflated:

**Correctness phase**: Replace each leaf individually with the selected approach. Mock dependencies at the boundary. Prove semantic identity with the Python implementation. Accept any performance penalty — it does not matter yet. Apply all safety gates.

**Fuse phase**: Once all leaves in a region are individually proven correct, remove the mocks and connect the replacements directly. This is where performance improvement appears — the overhead is removed.

If the AI is discussing performance during the correctness phase, it has drifted. Pull it back.

If the AI is skipping safety gates during either phase, it has drifted critically. This must be corrected immediately.

---

## Output

Include a **Terminal Weathering Correctness** section in the review output, after the normal NBS review dimensions:

```markdown
## Terminal Weathering Correctness

### Research Phase Discipline
[Was the research phase completed before weathering began? Is the hypothesis falsifiable? Is the approach justified by experimental evidence? Was "stop" considered?]

### Prediction Tracking
[Does the research phase prediction exist? Are results being compared against it? Are discrepancies flagged?]

### Leaf Discipline
[Are conversions targeting actual leaves? Are mocks in place? Is the AI conflating correctness with performance?]

### Boundary Safety
[Are shared types, reference semantics, and type identity being checked?]

### Safety Gates
[Are the approach-appropriate safety gates being applied? For C: ASan, Valgrind, refcount. For Rust: clippy, miri, boundary overhead.]

### Calling Convention Discipline (C only)
[Is METH_FASTCALL used everywhere? Are tutorial patterns absent? In tight loops, is the code using borrowed references where lifetime is guaranteed?]

### Phase Clarity
[Is the current phase (research vs correctness vs fuse) clear? Is the AI working within it?]
```

---

## The Contract

Correctness first. Performance is a consequence of correct fusing, not a goal of individual replacement. An AI that produces a semantically identical but slower replacement has succeeded. An AI that produces a faster but subtly different implementation has failed. An AI that skips the research phase has not started the methodology. An AI that produces code without the appropriate safety gates has not completed the correctness phase, regardless of test results.
