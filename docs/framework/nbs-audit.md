# /nbs-audit

An automated audit of a codebase against the engineering standards, followed by adversarial TDD to fix what it finds.

## What It Does

`/nbs-audit` reads every source file in a project and checks it against the engineering standards. It reports violations with line numbers, severities, and specific fixes. Then it writes adversarial tests and applies those fixes.

It is a machine. It does not interpret intent. It checks whether assertions exist, whether errors are swallowed, whether postconditions are verified, whether docstrings make claims the code cannot support.

## How It Differs from /nbs

`/nbs` reviews reasoning. It asks whether goals have drifted, whether confidence is performed rather than actual, whether the human and the agent are solving the right problem. It audits the epistemics.

`/nbs-audit` reviews code. It asks whether preconditions are checked, whether failures are silent, whether invariants are asserted at state transitions. It audits the engineering.

| Dimension | /nbs | /nbs-audit |
|-----------|------|------------|
| Target | Reasoning and process | Source code |
| Finds | Goal drift, bullshit, blind spots | Missing assertions, silent failures, unfalsifiable claims |
| Fixes | Recommendations (human decides) | Code changes with adversarial tests (machine applies) |
| Scope | One session | Entire codebase |

## The Cycle

**Phase 1: Discover.** Find all source files. Exclude generated code, vendored dependencies, build artefacts.

**Phase 2: Audit.** Launch one sub-agent per file, all in parallel. Each agent reads the engineering standards and reports violations grouped by severity: BUG, SECURITY, HARDENING. The consolidated report goes to `.nbs/audit-report.md`.

**Phase 3: Plan.** Bugs first, then security, then hardening. Every fix gets an adversarial test requirement: a test that proves the assertion fires on bad input and does not fire on good input. A test that passes regardless of whether the assertion exists is decoration, not verification.

**Phase 4: Fix.** One sub-agent per file, all in parallel. Each agent writes tests first (TDD), applies fixes, runs tests. After all agents complete, the full test suite runs. Adding assertions to one file can change exception types that other files depend on.

## When to Use

After a major refactor. Before a release. When onboarding a new codebase. Periodically, because engineering standards drift whether or not anyone notices.

## What It Produces

An audit report with violation counts. Adversarial tests proving every assertion works. A hardened codebase with executable specifications at every boundary.

The tests are the real output. The fixes are just the assertions. The tests prove the assertions do something.
