# Terminal Weathering -- Concepts

Reference documents for the terminal weathering methodology. For the
full directory guide including docs/ and evidence/, see the
[root index](../index.md).

---

## The Methodology

| Document | What it covers |
|----------|---------------|
| [Terminal Weathering](terminal-weathering.md) | The complete methodology -- research phase, weathering cycles, evidence gates, trust gradient, NBS alignment |

---

## Python to C -- Writing Fast C Extensions

| Document | What it covers |
|----------|---------------|
| [C Extension Performance](c-extension-performance.md) | Calling conventions, argument handling, return values, attribute access, GC tracking, reference counting |

---

## C++ to C -- Converting a C++ Codebase to Pure C

| # | Document | What it covers |
|---|----------|---------------|
| 1 | [Types](cpp-to-c-types.md) | Honest type definitions -- the shared vocabulary |
| 2 | [Patterns](cpp-to-c-patterns.md) | 19 conversion patterns -- C++ feature to C replacement, risk levels, pitfalls |
| 3 | [Build Failures](cpp-to-c-build-failures.md) | 11 build failure classes with error messages, root causes, fixes, prevention rules |
| 4 | [Checklist](cpp-to-c-checklist.md) | Pre-compile checklist for converted files |

### Internal Reference

| Document | What it covers |
|----------|---------------|
| [Analysis](cpp-to-c-analysis.md) | Raw analysis findings -- evidence base for the patterns and build failures |
