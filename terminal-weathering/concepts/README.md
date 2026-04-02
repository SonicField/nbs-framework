# Terminal Weathering — Concepts

Reference documents for performance optimisation methodology and C++ to C porting.

## Core

| Document | What it covers |
|----------|---------------|
| [Terminal Weathering](terminal-weathering.md) | The methodology — hypothesis-driven, evidence-gated architectural intervention |
| [C Extension Performance](c-extension-performance.md) | Performance patterns for CPython C extensions |

## C++ to C Porting Reference

A comprehensive guide to converting C++ codebases to pure C, anchored in the Phoenix project's conversion of the CinderX JIT compiler. Read in this order:

| # | Document | What it covers |
|---|----------|---------------|
| 1 | [Types](cpp-to-c-types.md) | Honest type definitions — the vocabulary for the domain |
| 2 | [Analysis](cpp-to-c-analysis.md) | Raw findings from Phoenix: 14 patterns, 10 build failures, with real code |
| 3 | [Patterns](cpp-to-c-patterns.md) | 19 conversion patterns — C++ feature → C replacement, risk levels, pitfalls |
| 4 | [Build Failures](cpp-to-c-build-failures.md) | 7+ failure classes with error messages, root causes, fixes, prevention |
| 5 | [Checklist](cpp-to-c-checklist.md) | 25-item pre-compile checklist for converted files |
