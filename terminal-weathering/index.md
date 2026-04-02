# Terminal Weathering

Hypothesis-driven performance optimisation of Python systems through
evidence-gated architectural intervention.

---

## Start Here

New to terminal weathering? Read these first.

| Document | What you will learn |
|----------|---------------------|
| [Overview](docs/overview.md) | What terminal weathering is, the problem it solves, how it relates to NBS |
| [Getting Started](docs/getting-started.md) | Prerequisites, starting a session, the research phase, a single conversion cycle |
| [Methodology](docs/methodology.md) | The full methodology -- research phase, weathering phases, trust gradient, epistemic garbage collection |

The overview is a 5-minute read. The getting-started guide walks you
through your first session. The methodology document is the complete
reference for practitioners.

---

## Porting Paths

Terminal weathering produces different kinds of conversion work
depending on what the research phase finds. These reference documents
cover the two most common paths.

### Python to C -- Writing Fast C Extensions

**When to use:** Profiling shows structural overhead (object field
access, attribute protocol, call convention dispatch) and the research
phase selects C extension types as the approach.

| Document | What it covers |
|----------|---------------|
| [C Extension Performance](concepts/c-extension-performance.md) | Calling conventions, argument handling, return values, attribute access, GC tracking, reference counting -- with measured costs on each |

This is a performance discipline, not a tutorial. It explains *why*
`METH_FASTCALL` exists, *what* `PyArg_ParseTuple` costs, and *how* to
avoid the slow patterns AI systems reach for by default.

### C++ to C -- Converting a C++ Codebase to Pure C

**When to use:** You have a C++ codebase (or C++ components within a
larger system) that you are converting to C -- for example, eliminating
a C++ dependency from a C project, or reducing build complexity.

Read these documents in order:

| # | Document | What it covers |
|---|----------|---------------|
| 1 | [Types](concepts/cpp-to-c-types.md) | Honest type definitions -- the shared vocabulary for all other documents |
| 2 | [Patterns](concepts/cpp-to-c-patterns.md) | 19 conversion patterns -- C++ feature to C replacement, risk levels, pitfalls |
| 3 | [Build Failures](concepts/cpp-to-c-build-failures.md) | 11 build failure classes with error messages, root causes, fixes, prevention rules |
| 4 | [Checklist](concepts/cpp-to-c-checklist.md) | Pre-compile checklist for converted files -- mechanical yes/no checks |

The patterns document covers the *how*; the build failures document
covers the *what goes wrong*; the checklist covers the *did I miss
anything*. Types provides the shared vocabulary that ties them together.

#### Internal Reference

| Document | What it covers |
|----------|---------------|
| [Analysis](concepts/cpp-to-c-analysis.md) | Raw analysis findings -- 14 patterns, 10 build failure classes, working evidence base |

The analysis document is the evidence base from which the patterns and
build failures were derived. Consult it when you need to trace a
pattern back to its source.

---

## Evidence

Case studies with measured results. These are not tutorials -- they are
the empirical record that the methodology references.

| Document | What it demonstrates |
|----------|---------------------|
| [Data Container Optimisation](evidence/soma-weathering.md) | Three approaches tried (Rust, C, dispatch). Two falsified. One succeeded (2.06x). Shows the methodology working end to end. |
| [Weathering at the Right Layer](evidence/weathering-at-the-right-layer.md) | Correct diagnosis, correct mechanism, no whole-system effect. Shows the methodology correctly concluding "stop". |

---

## The Core Methodology

For the full conceptual treatment -- the geological metaphor,
architectural patterns, layered progression, weathering phases, C
safety gates, the trust gradient, and NBS alignment:

| Document | What it covers |
|----------|---------------|
| [Terminal Weathering (full reference)](concepts/terminal-weathering.md) | The complete methodology document -- read after the overview and getting-started guide |

---

## Quick Reference

| I want to... | Start with |
|--------------|------------|
| Understand the methodology | [Overview](docs/overview.md) |
| Run my first session | [Getting Started](docs/getting-started.md) |
| Write a fast C extension | [C Extension Performance](concepts/c-extension-performance.md) |
| Port C++ code to C | [Types](concepts/cpp-to-c-types.md) then [Patterns](concepts/cpp-to-c-patterns.md) then [Build Failures](concepts/cpp-to-c-build-failures.md) then [Checklist](concepts/cpp-to-c-checklist.md) |
| See evidence of the methodology working | [Data Container Optimisation](evidence/soma-weathering.md) |
| See evidence of the methodology stopping | [Weathering at the Right Layer](evidence/weathering-at-the-right-layer.md) |
| Read the full methodology reference | [Terminal Weathering](concepts/terminal-weathering.md) |
