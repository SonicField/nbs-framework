# Team Audit: Zone-Based Codebase Hardening

> Audit the architecture, not just the files. Fix with tests, not with hope. Document for the next person, not for the build log.

## Why This Exists

`nbs-audit` is powerful: it applies engineering standards to every file in a codebase and produces concrete violations with specific fixes. It has proven itself on real codebases — bugs found, quality raised, AI-generated code brought to human standards. But it has structural weaknesses that compound on large codebases:

1. **File-level parallelism misses cross-cutting issues.** An API contract violation spanning three files is invisible to an agent reading one file. Data flow corruption, inconsistent error propagation, and architectural decay happen between files, not within them.

2. **Massive reports with no prioritisation context.** Fifty files produce fifty reports. Without understanding which modules are critical, which are high-churn, and which are stable, the team cannot distinguish a hardening suggestion in dead code from a bug in the hot path.

3. **Fixes without integrated TDD introduce subtle bugs.** nbs-audit writes tests, but the audit-then-fix pipeline means fixes are applied in bulk without the discipline of red-green-refactor. A fix that silently changes an exception type can break a caller three modules away.

4. **No documentation trail.** After hardening, the code has more assertions but less explanation of *why*. Future developers (human or AI) see the guard but not the reasoning. The hardening becomes opaque — technically correct but epistemically invisible.

5. **Not designed for teams.** nbs-audit uses sub-agents as parallel workers. Teams reason differently: they debate, verify each other's claims, catch architectural blind spots, and build shared understanding. Sub-agents finish and forget; teams accumulate institutional knowledge.

This document describes a zone-based, team-oriented approach that preserves nbs-audit's quality philosophy while addressing these weaknesses.

---

## Philosophy

These principles are inherited from the NBS engineering standards and are non-negotiable:

- **Falsifiability as foundation.** Every claim about the code carries three obligations: articulate what would prove it wrong, try to find that counterexample, report actual confidence. A finding without a test is speculation. A fix without a failing test is hope.

- **Verbs over nouns.** "This value was validated" matters. "This has type ValidatedInput" does not. Assertions are executable specifications — they verify themselves. Tests are falsification attempts — they try to break the code.

- **The verification cycle.** Design, Plan, Deconstruct into testable steps. For each step: Test first, Code to pass the test, Document learnings before moving on. This is not ceremony. It is the engine of quality.

- **Honest reporting.** Report what happened, not what you wanted to happen. A reviewer who discovers an undisclosed problem loses trust in everything else.

---

## Core Concepts

### Zones

A **zone** is a group of tightly-coupled files that share invariants and are audited as a unit. Zones are not language-level modules — they are logical groupings the team defines based on which files must be understood together to verify correctness.

Examples across different contexts:
- **C project:** Three `.c` files that implement a data pipeline share the invariant "stage N's output is valid input for stage N+1." Auditing any one in isolation cannot verify this chain.
- **Bash project:** Five deployment scripts that all read from the same config file share the invariant "config values are validated before use." The contract is implicit (no type system enforces it), which makes zoning them together *more* important.
- **Python project:** A data ingestion module where `reader.py`, `validator.py`, and `writer.py` form a pipeline. The validator promises "output records contain all required fields." Reader and writer depend on this promise.

The zone concept works regardless of whether the language has a formal module system. In languages without one (bash, Make, plain C without clean headers), the contracts are implicit — they live in the programmer's head or in comments. This makes the Phase 1 contract mapping critical, because tests become the *only* enforcement mechanism.

Zone properties:
- A zone contains **5-10 files**. If a logical grouping has more, subdivide it into audit zones.
- Every source file belongs to exactly one zone. Shell scripts, Makefiles, and other non-compiled sources are source files — they need zones too.
- Zone membership is determined by shared invariants, not directory structure or language features.
- The zone map is a hypothesis. Evidence from auditing and fixing refines it.

**Foundation zones.** Some zones are small (few files, low complexity) but imported or used by nearly everything else — utility libraries, shared headers, common helpers. These have outsized blast radius: a bug in the foundation propagates to every dependent zone. Foundation zones must be audited first regardless of file count, and receive full testkeeper verification on ALL findings (not just BUG/SECURITY), because a decoration defect in the foundation creates false confidence everywhere.

### Cross-Zone Contracts

A **contract** is what one zone promises another at their boundary.

Contracts come in several forms:

- **Function-level:** "Zone A's output function returns fully validated data or an error code — never partially validated data."
- **Environment/state-level:** "This script expects `$DATA_DIR` to be set and to point to an existing, writable directory."
- **Library-level:** "The shared library guarantees that all returned strings are valid UTF-8 and NULL-terminated."
- **Binary interface:** "This binary accepts `--config PATH` as its first argument, reads JSON from stdin, writes results to stdout, and exits 0 on success or 1 on error." Binary interface contracts arise when zones communicate via fork+exec of sibling binaries rather than function calls. The contract covers: expected arguments, stdin format, stdout/stderr format, and exit codes.

Contracts exist in every language, but languages without formal type systems or module boundaries (bash, Make, C without opaque types) have *implicit* contracts that are more dangerous precisely because nothing enforces them. Making these contracts explicit is the point of Phase 1.

**Zoning script-heavy codebases.** When a project contains shell scripts, Makefiles, or other non-compiled sources with significant logic, these need zones too. Zone scripts by shared state: scripts that read/write the same config files, environment variables, directories, or session data belong together. The header/#include model does not apply — use the shared-state and call-chain heuristics from Phase 1 instead.

Contracts are the key enabler for cross-cutting auditing. Without explicit contracts, the auditor knows where boundaries are but not what they should enforce. Every boundary identified in the survey must have its contract stated.

### Two Rhythms

The audit operates in two rhythms:

1. **Broad (read-only).** The architectural survey (Phase 1) and the zone audit (Phase 2) scan the codebase without changing it. These phases can be parallelised across zones because they produce no side effects. The broad pass gives the team a global picture before committing resources to fixes.

2. **Deep (per-zone, sequential).** The fix cycle (Phase 3) and zone gate (Phase 4) change code. Fixes can affect cross-zone contracts. So the team fixes one zone at a time, gates it, commits it, then moves to the next. The codebase is always stable between zones.

For large codebases, Phases 2-4 interleave per zone: audit zone A, fix zone A, gate zone A, then audit zone B with the benefit of what was learned. Phase 1 remains global. Phase 5 remains final.

### The Audit Progress Log

The **progress log** is the single source of truth for where the audit stands. Chat is discussion; the scribe decision log records rationale; the progress log records state. It is a persistent artefact that lets a team resume across sessions or a human check progress at a glance.

The progress log structure is defined in its own section below.

---

## Overview

```
Phase 1: Architectural Survey     (global, read-only)
Phase 2: Zone Audit               (per zone, read-only)
Phase 3: Zone Fix Cycle           (per zone, sequential)
Phase 4: Zone Gate                (per zone, after fix cycle)
Phase 5: Documentation & Report   (global, after all zones)
```

Phase 1 is performed once. Phases 2-4 repeat for each zone in priority order. Phase 5 is performed once after all zones are complete (or at sprint boundaries for large codebases).

### Why Phase Ordering Matters

Within a zone, the ordering is a dependency chain: audit findings feed the fix cycle, the fix cycle feeds the gate. Across zones, the ordering is progressive: learnings from zone N inform zone N+1.

The broad phases (1-2) are read-only and can overlap across zones. The deep phases (3-4) change code and must be sequential per zone.

---

## Team Roles Per Phase

Not every role is active in every phase. Idle roles should be explicitly stood down, not left to drift.

| Phase | Active Roles | Purpose |
|-------|-------------|---------|
| 1. Architectural Survey | Theologian (lead), Supervisor | Map zones, contracts, risk ranking |
| 2. Zone Audit | Generalist (lead), Theologian (review) | Find violations with cross-zone context |
| 3. Zone Fix Cycle | Generalist (lead), Testkeeper (verification) | TDD: test first, fix second, document third |
| 4. Zone Gate | Testkeeper (lead), Gatekeeper (review) | Full suite, integration, performance, commit |
| 5. Documentation & Report | Generalist (lead), Gatekeeper (review) | Document reasoning, produce final report |

**Throughout all phases:**
- **Supervisor** maintains terminal goal, monitors progress, captures 3Ws, manages the progress log
- **Gatekeeper** reviews commits before they are pushed
- **Scribe** records decisions in the decision log
- **Medic** monitors for hallucination and reasoning failures

---

## Phase 1: Architectural Survey

**Goal:** Understand the codebase's structure, define zones, map contracts, and rank zones by risk — before looking at individual files.

**Why this phase exists:** nbs-audit jumps straight to per-file analysis. This means a hardening suggestion in a rarely-used utility function gets the same weight as a bug in the core data pipeline. Without architectural context, prioritisation is impossible.

### Entry Criteria
- Terminal goal is stated and agreed
- Codebase is accessible and buildable
- Team roles are assigned

### Activities

1. **Source inventory.** Identify all source files, excluding vendored dependencies, generated code, and re-export stubs. Count them. This determines the scale of the audit and informs zone sizing.

2. **Zone definition.** Group files into zones of 5-10 files each. The grouping criterion is shared invariants: files that must be understood together to verify correctness belong in the same zone. Directory structure is a hint, not a rule.

   **When invariants are implicit or unknown** (common in bash, ad-hoc C, configuration-driven systems), use these heuristics as starting points:
   - **Shared state:** files that read/write the same data (config files, databases, environment variables, shared directories) belong together
   - **Call chains:** files that call each other directly (source/include, function calls, script invocations) belong together
   - **Shared purpose:** files that contribute to the same user-visible capability belong together
   - **Failure propagation:** if a bug in file A can cause file B to produce wrong output, they likely share an invariant

   These heuristics produce an initial zone map. The audit itself will reveal whether the boundaries are right — invariants you didn't know about will surface during Phase 2. The zone map is revisable (backtracking trigger 3 handles boundary revision).

   **Disagreement resolution:** if team members disagree on zone boundaries, the theologian makes the call and the team proceeds. The cost of a wrong boundary is low — the backtracking protocol handles revision. The cost of debating boundaries indefinitely is high.

3. **Dependency mapping.** Identify which zones depend on which. Note the direction of dependencies. Identify circular dependencies if any.

4. **Cross-zone contract mapping.** For every boundary between zones, state the contract explicitly:
   - What does zone A promise to zone B at this boundary?
   - What does zone B assume about zone A's output?
   - How is this contract currently enforced (assertion, type, convention, nothing)?

5. **Risk ranking.** Rank zones by risk using observable evidence:

   | Factor | What to measure | Why it matters |
   |--------|----------------|----------------|
   | Blast radius | How many other zones depend on this one? | A bug here propagates widely |
   | Complexity | LOC, cyclomatic complexity, public interface count | Complex code hides more bugs |
   | Churn | Git log frequency — how often has this zone changed? | High churn = more opportunity for introduced bugs |
   | Boundary exposure | Does this zone handle external input? Network? User data? | External input is adversarial |
   | Test coverage | Does this zone have tests? How comprehensive? | Thin coverage = unverified claims |

6. **Known issues inventory.** Collect TODOs, FIXMEs, HACKs, known bugs, and technical debt. These are not findings — they are context for the audit.

### Deliverable

The **initial audit progress log** (see Progress Log section), containing:
- Zone map with files, status (all `unmapped`), priority rank, blast radius
- Cross-zone contract list
- Risk ranking with evidence
- Known issues inventory

### Exit Criteria
- Zone map reviewed by supervisor
- Risk ranking agreed by team
- Cross-zone contracts stated for every boundary
- Decision recorded by scribe: "Audit will proceed in this zone order: [ranked list]"

---

## Phase 2: Zone Audit

**Goal:** Systematically find engineering standards violations, working through zones in risk-priority order.

**Why this phase differs from nbs-audit:** Instead of one agent per file in parallel, the team audits one zone at a time. The auditor reads all files in the zone together and can identify cross-file issues: inconsistent error handling, broken API contracts, data flow violations that span files.

### Entry Criteria
- Phase 1 complete with agreed zone priority order
- Engineering standards document available

### Activities

For each zone, in priority order:

1. **Read all files in the zone.** Not one at a time — all of them. Understand how they work together. Understand the zone's internal invariants and its contracts with other zones.

2. **Audit against engineering standards.** For each violation found, record:

   | Field | Content |
   |-------|---------|
   | Finding ID | Zone-prefixed identifier (e.g. Z1-F1) |
   | Location | File and line number(s) |
   | Standard violated | Precondition, postcondition, invariant, silent failure, unfalsifiable claim, etc. |
   | Severity | BUG (wrong now), SECURITY (silent failure in security path), HARDENING (missing guard) |
   | Cross-zone impact | Does this affect other zones? If yes, which and how? |
   | Specific fix | What exactly should change |

3. **Check zone boundaries.** At every boundary identified in Phase 1:
   - Are inputs validated at the boundary?
   - Are outputs verified before crossing?
   - Is error propagation consistent?
   - Are there implicit contracts that are not enforced by assertions?

4. **Identify invariants.** List every invariant the zone must maintain. This list becomes the testkeeper's coverage checklist in Phase 3. An invariant without a test is an unverified claim.

5. **Post zone audit to chat.** Summarise findings with counts by severity. Flag any cross-zone issues that affect zones not yet audited.

6. **Update the progress log.** Set zone status to `audited`. Record findings in the zone's findings table. Record identified invariants.

### Parallelisation

Zone audits are read-only. Multiple zones can be audited in parallel by different workers if:
- The zones are independent (no shared files)
- Each worker posts findings to chat as they complete
- The theologian reviews all zone audits for cross-zone coherence

For small codebases, audit all zones before fixing any. For large codebases, begin fixing the highest-priority zone as soon as its audit is complete (see Adapting to Codebase Size).

### Deliverable

Per zone: findings list with severity, cross-zone flags, and invariant list.

Consolidated summary:

| Zone | BUG | SECURITY | HARDENING | Cross-Zone | Invariants |
|------|-----|----------|-----------|------------|------------|
| Z1   | 3   | 1        | 7         | 2          | 5          |
| Z2   | 0   | 0        | 4         | 0          | 3          |

### Exit Criteria (per zone)
- All files in zone audited together
- Findings posted with severity classification
- Invariants listed
- Cross-zone issues flagged
- Theologian has reviewed for architectural coherence
- Progress log updated

---

## Phase 3: Zone Fix Cycle

**Goal:** Fix all findings in a zone using strict TDD discipline: test first, fix second, document third.

**Why this phase differs from nbs-audit:** nbs-audit's fix phase uses separate sub-agents per file, each writing tests and fixes independently. Nobody checks whether the test is genuinely falsifiable. Nobody checks whether the assertion guards a condition that can actually occur. Nobody reviews whether the fix preserves cross-zone contracts.

### Entry Criteria (per zone)
- Zone audit complete with accepted findings
- Build is clean and existing tests pass
- Baseline established: test count, pass count, build warnings
- If project has benchmarks: baseline performance numbers captured before any fixes are applied to this zone

### Step 0: Baseline Smoke Test (if zone has no existing tests)

Many zones in real codebases have zero tests. When `existing tests pass` is trivially true because no tests exist, Phase 3 has no regression baseline — step 2d ("run the zone's test suite") runs nothing.

If the zone has no existing tests, write at least one integration-level smoke test before proceeding. This test should exercise the zone's primary code path end-to-end — call the main entry point with valid input and verify it produces expected output. This establishes a baseline that subsequent fixes cannot regress against.

The smoke test does not need to be adversarial. It needs to be real: if the zone's core functionality breaks, this test fails.

### Step 1: Test Planning

Before writing any tests, plan the test approach for the zone's findings:

For each finding, determine:
- **Test type:** assertion test, silent failure test, unfalsifiable claim test, boundary test, or integration test
- **Adversarial inputs:** empty/null/zero, boundary values (MAX_INT, MIN_INT), type confusion (bool-as-int), malformed data (invalid UTF-8, truncated input), resource edge cases (very large inputs)
- **Expected behaviour before fix** (what happens now)
- **Expected behaviour after fix** (what should happen)

Map each finding to the invariants identified in Phase 2. Every invariant must have at least one test. Unmapped invariants are coverage gaps — report them.

### Step 2: TDD Execution

For each finding, in severity order (BUG first, then SECURITY, then HARDENING):

**2a. Write the test first.** The generalist (or assigned worker) writes both the test and the fix. The testkeeper verifies falsifiability — the testkeeper does not write production tests or fixes. Implement the test. Run it. Confirm it demonstrates the problem:
- For missing assertions: test should show that invalid input is silently accepted
- For silent failures: test should show that the error is swallowed
- For unfalsifiable claims: test should attempt to falsify the claimed property

**2b. Apply fix and verify falsifiability.** The worker applies the fix. The testkeeper then verifies the test is genuinely falsifiable using this concrete procedure:

1. With the fix applied, run the test — it **MUST pass**
2. Remove the fix (revert the code change)
3. Run the test again — it **MUST fail**
4. Re-apply the fix (the final state is fix-applied, test-passing)

If the test passes both with and without the fix, it is **decoration**. Decoration is a defect, not a warning — it is classified at the same severity as a missing test because it creates false confidence. The finding goes back to the worker for rework. A test that always passes is worse than no test.

**Testkeeper verification scope:** The testkeeper performs the three-step falsifiability check on **100% of BUG and SECURITY findings**. For HARDENING findings, the testkeeper spot-checks a sample: minimum 30% of findings or 3 findings, whichever is greater. If any sampled HARDENING test fails the falsifiability check, the testkeeper expands to 100% for that zone.

**Non-deterministic falsifiability (concurrency).** The three-step procedure assumes deterministic tests: remove the fix, test fails every time. Concurrency tests are different — a race condition may only manifest under specific thread interleavings. For concurrency-related findings, adapt the procedure:

- Run the test **N times** (not once) with the fix removed, to confirm it *can* fail
- Use **ThreadSanitizer (TSan)** as a falsifiability amplifier — TSan detects race conditions at the access level rather than waiting for the symptom to manifest. "This test fails under TSan when the fix is removed" is sufficient evidence of falsifiability
- Accept **probabilistic falsifiability** for concurrency tests: the test does not need to fail every run without the fix, but it must fail reliably under TSan or within N runs

This applies to any concurrent codebase, not just specific projects.

**2c. Document the fix.** Add a brief inline comment at the fix site explaining:
- **Why** this assertion/guard exists (not restating the code)
- **What contract** it enforces
- **What class of bugs** it prevents
- **What would break** if it were removed

Examples in different languages:

```c
/* Guard: upstream stage must produce a valid record before this stage
 * processes it. Without this, a NULL record from malformed input causes
 * segfault in process_record() (Z1-F1, found during audit).
 */
assert(record != NULL && "upstream produced NULL record");
```

```python
# Guard: config values must be validated before use. Without this,
# an empty string from a missing env var silently produces a broken
# output path (Z3-F2, found during audit).
assert output_dir and os.path.isdir(output_dir), \
    f"output_dir must be a valid directory, got {output_dir!r}"
```

```bash
# Guard: DATA_DIR must exist before pipeline runs. Without this,
# scripts silently write to CWD when the variable is unset (Z2-F1).
assert_dir_exists "DATA_DIR" "$DATA_DIR"
```

**2d. Run the zone's test suite.** Confirm no regressions within the zone after each finding is fixed.

### Step 3: Zone Completion

After all findings in the zone are fixed:

1. **Run the full project test suite.** Catch cross-zone regressions.
2. **Run with sanitisers.** ASan, UBSan, TSan (if applicable), Valgrind — whatever the project's dynamic analysis tooling supports. A fix that introduces undefined behaviour is not a fix.

   **Sanitiser suppressions** are acceptable only when the hypothesis "this is a false positive" survives falsification. If a sanitiser flags an issue and you can construct a scenario where the flagged access causes observable wrong behaviour, the suppression is unjustified — fix the issue. If you genuinely cannot demonstrate harm (e.g., the sanitiser does not understand a custom synchronisation primitive like an epoch-based barrier), document the reasoning and suppress. Every suppression must record: (a) what the sanitiser reported, (b) why it is a false positive, (c) what would falsify that claim. A suppression without this evidence is dressing up incomplete analysis as success.
3. **Update the progress log.** Set zone status to `fixing → hardened`. Update finding statuses to `verified`. Record invariant coverage (invariants identified vs invariants tested).

### Invariant Coverage

The right coverage metric for an audit is **invariant coverage**, not line or branch coverage.

- Phase 2 produces a list of invariants per zone
- Phase 3 maps tests to invariants
- The ratio (invariants with tests / total invariants) is the zone's coverage score
- Untested invariants are coverage gaps — report them to chat

Line/branch coverage may be 100% while missing the property that actually matters. Invariant coverage measures what the audit cares about: are the things that must be true actually verified?

### Exit Criteria (per zone)
- All findings fixed with passing tests
- All tests verified falsifiable by testkeeper (three-step procedure)
- Invariant coverage reported (every invariant mapped to at least one test, or gap justified)
- No decoration defects remaining
- Full project test suite passes
- Sanitisers pass
- Inline documentation added for every fix

---

## Phase 4: Zone Gate

**Goal:** Verify the hardened zone does not break anything outside itself, then commit.

### Entry Criteria
- Phase 3 complete for this zone
- Full test suite and sanitisers pass

### Activities

1. **Cross-zone contract tests.** For each contract this zone has with other zones (identified in Phase 1):
   - Write or run integration tests that exercise the contract
   - Verify that preconditions on the receiving zone match postconditions on the sending zone
   - Verify error propagation is consistent across the boundary

   **Contract-level falsifiability** is verified differently from Phase 3's assertion-level procedure. For cross-zone tests, removing a single assertion may not cause the integration test to fail. Instead, verify falsifiability by deliberately violating the contract at the boundary — returning invalid data, omitting required fields, injecting an error where success was expected — and confirming the test catches the violation. This proves the test can detect contract breaches, which is the property that matters at the integration level.

2. **Performance regression check.** If the project has benchmarks:
   - Run an **ABBA-interleaved** comparison against the pre-zone baseline
   - ABBA methodology: alternate between baseline and current builds to control for thermal drift and system load variation. Sequential A-then-B runs produce misleading deltas when baseline drift exceeds 1%
   - If assertions cause measurable regression above the project-defined threshold, the team must choose: restructure (compile-time guard, debug-only assert, optimised check) or accept the cost with explicit justification
   - The threshold is project-defined, not hardcoded — the document cannot know whether 2% or 10% is acceptable for a given project

3. **Adversarial end-to-end testing.** If the project has an entry point (CLI, API, service), feed it adversarial inputs and verify the new assertions catch bad input at the boundary rather than deep in the call stack.

4. **Commit.** One commit per zone, with a message listing:
   - Zone name and files changed
   - Violation categories addressed (BUG/SECURITY/HARDENING counts)
   - Test count delta
   - Invariant coverage score

5. **Gatekeeper review.** Gatekeeper reviews the commit against the five standard criteria (correctness, file locations, sensitive info, documentation, completeness) before the team moves to the next zone.

   **Push authorisation.** If the project requires human approval before pushing, the authorization must come as a chat message from the human — not terminal input, which cannot be independently verified. Both the gatekeeper and supervisor must independently confirm the authorization is genuine. If the medic raises a concern about the authorization's authenticity, it is treated as revoked.

6. **Post 3Ws to chat.** What went well, what didn't, what to do better. Supervisor captures learnings. Scribe records decisions.

7. **Update the progress log.** Set zone status to `hardened`. Record commit hash, test delta, invariant coverage score.

### Exit Criteria
- Cross-zone contract tests pass
- Performance impact assessed (acceptable or justified)
- Commit reviewed by gatekeeper
- Progress log updated
- 3Ws posted

---

## Phase 5: Documentation and Report

**Goal:** Produce documentation that explains the reasoning behind the hardening, and a final report summarising what was done.

**Why this phase exists:** nbs-audit produces hardened code with more assertions but no explanation of why those assertions exist. Future developers see `assert count > 0` and wonder: was this a real bug, or just defensive programming? Without documentation, the hardening is epistemically invisible.

### Entry Criteria
- All zones hardened (or sprint boundary reached for large codebases)
- All commits reviewed by gatekeeper

### Activities

1. **Inline documentation review.** For every fix applied, verify that:
   - The assertion/guard has a comment explaining *why* it exists
   - The comment references the class of bug it prevents
   - If the fix was prompted by a real bug found during audit, the comment says so

2. **Zone-level documentation.** For each zone that received significant changes:
   - Update or create a zone-level comment explaining the zone's contracts
   - List the preconditions for using the zone's public API
   - List the postconditions the zone guarantees
   - Note the invariants the zone maintains

3. **Final report.** Produce a summary document containing:
   - Architectural survey summary (from Phase 1)
   - Findings summary by zone and severity (from Phase 2)
   - Fix summary: changes made, tests added, violation categories addressed
   - Invariant coverage scores per zone
   - Cross-zone validation results (from Phase 4)
   - Performance impact assessment
   - Known limitations: what was not addressed and why
   - Learnings: consolidated 3Ws from all fix cycles
   - Backtrack events: what was reopened and why

4. **Independent documentation verification.** A team member who did not write the documentation reads it and verifies every factual claim against the source code. Claims that cannot be verified are removed or marked as unverified.

### Deliverable
- Updated inline documentation
- Zone-level documentation
- Final audit report
- Progress log in final state

### Exit Criteria
- All documentation verified against source code
- Final report reviewed by supervisor
- Gatekeeper reviews documentation commits
- Human reviews and approves the final report
- Decision recorded: "Team audit complete."

---

## The Audit Progress Log

The progress log is maintained throughout the audit. It is the single source of truth for audit state. Chat is for discussion. The scribe decision log is for rationale. The progress log is for status.

### Structure

```markdown
# Audit Progress Log

## Meta
- Terminal goal: [one sentence]
- Started: [date]
- Current sprint: [N]
- Current phase: [phase and zone]

## Zone Map

| Zone | Files | Status | Priority | Blast Radius |
|------|-------|--------|----------|--------------|
| Z1-core | core.c, util.c, types.c | hardened | 1 | high (12 dependents) |
| Z2-io | read.c, write.c, format.c | fixing | 2 | high (8 dependents) |
| Z3-config | config.c, env.c | audited | 3 | medium (5 dependents) |

Status values: unmapped → audited → fixing → hardened → reopened → re-hardened
For context zones (partial-codebase audits): unmapped → boundary-audited → boundary-hardened

## Cross-Zone Contracts

| From | To | Contract | Enforced By |
|------|----|----------|-------------|
| Z1-core | Z2-io | process() returns complete result or error, never partial | assertion (Z1-F3) |
| Z3-config | Z1, Z2 | config values validated before use | precondition (Z3-F1) |

## Zone Findings

### Z1-core

| ID | Location | Severity | Description | Status | Test |
|----|----------|----------|-------------|--------|------|
| Z1-F1 | core.c:142 | BUG | No null check on input record | verified | test_core_null_input |
| Z1-F2 | types.c:89 | HARDENING | Missing postcondition on output count | verified | test_types_count_postcondition |
| Z1-F3 | core.c:210 | BUG | Can return partial result on timeout | verified | test_core_timeout_completeness |

### Z2-io
(findings table)

## Invariant Coverage

| Zone | Invariants Identified | Invariants Tested | Coverage | Gaps |
|------|----------------------|-------------------|----------|------|
| Z1-core | 5 | 5 | 100% | none |
| Z2-io | 8 | 6 | 75% | Z2-I4 (timeout handling), Z2-I7 (encoding edge case) |

## Backtrack Log

### [date] — Z1-core reopened
- **Trigger:** Contract discovery — Z2-io audit found process() can return partial result on timeout
- **Action:** Added Z1-F3 (core.c:210, postcondition on result completeness)
- **Resolution:** Test added, fix verified, zone re-hardened

## Sprint Summary (for large codebases)

### Sprint 1
- Zones targeted: Z1, Z2, Z3
- Findings resolved: 14/14
- Backtrack events: 1 (Z1 reopened from Z2 audit)
- Learnings: timeout handling was a systemic pattern — added to checklist for remaining zones
```

### Maintenance Rules

- The supervisor owns the progress log
- Update after every phase transition (zone status change, finding status change)
- The backtrack log is append-only — never delete entries
- The zone map is updatable — zone boundaries can change as evidence accumulates
- Finding status transitions: `open → test-written → fixed → verified` (or `→ decoration-defect → reworked → verified`)

---

## Backtracking Protocol

The zone map is a hypothesis. Evidence from auditing and fixing refines it. Backtracking is not a failure — it is the process working.

### Three Triggers

**1. Contract discovery.** Fixing zone B reveals that zone A's fix broke a contract zone B depends on. Example: zone A added an assertion that throws on invalid input, but zone B was handling that invalid input gracefully and now crashes.

**2. Pattern propagation.** Auditing zone B reveals a pattern (e.g. "timeout handling is missing everywhere") that also applies to zone A but was missed in zone A's audit.

**3. Invariant revision.** Fixing reveals that the zone map was wrong — files that were in separate zones actually share invariants that must be maintained together. Zone boundaries need to shift.

### Backtrack Procedure

1. The discoverer posts the backtrack trigger to chat with evidence
2. The theologian assesses: is this a genuine contract/invariant issue or a false alarm?
3. If genuine, the supervisor reopens the affected zone (status → `reopened`)
4. The team runs a **mini-cycle** on the reopened zone: new finding → test → fix → verify → document
5. The mini-cycle follows the same TDD discipline as Phase 3 (testkeeper verifies falsifiability)
6. The zone is re-gated (Phase 4) and re-committed
7. The backtrack is recorded in the progress log's backtrack log

### Constraint

**The test suite must be green at all times during backtracking.** A backtrack adds or modifies tests and fixes. It does not disable existing tests. If a backtrack-fix breaks an existing test, either:
- The existing test was testing the wrong property → update it with documented rationale
- The backtrack-fix is wrong → reject it

---

## Partial-Codebase Auditing

Not every audit targets an entire codebase. A common scenario: new code has been added to a large existing project, and the goal is to audit the new code and its interactions with the existing code — without auditing everything.

Examples:
- A new subsystem added to a large C project (new GC implementation added to a runtime)
- A feature branch with significant changes to an established codebase
- A library integration where the glue code and interaction points need auditing

### Target Zones vs Context Zones

The zone map distinguishes two classes:

**Target zones** contain the new or modified code. They receive the full audit: all phases, all findings, full TDD, full testkeeper verification. These are the zones the team is responsible for hardening.

**Context zones** contain existing code that target zones interact with. They are NOT fully audited. They are mapped in Phase 1 for one purpose: to identify the contracts at the boundary between new and existing code. Context zones receive boundary-only audit — checking that the interaction points are sound and that the existing code's assumptions still hold with the new code present.

The zone map marks each zone as `target` or `context`:

| Zone | Type | Files | Status |
|------|------|-------|--------|
| Z1-new-gc | target | gc_parallel.c, gc_parallel.h, ... | audited |
| Z2-existing-gc | context | gc.c, gcmodule.c, ... | boundary-audited |

### Git-Driven Scope Definition

For audits of new code within a larger project, Phase 1 includes a pre-step:

**Step 0: Identify the audit surface.** Use git analysis (diff against upstream, `git log --name-only`, merge-base comparison) to determine which files are new or significantly modified. These form the target zones. Then trace their dependencies — which existing files do they include, call, or depend on? Those form the context zones.

This produces a scope boundary: everything inside is audited (target or context); everything outside is out of scope.

### Asymmetric Audit Depth

| Aspect | Target Zones | Context Zones |
|--------|-------------|---------------|
| Phase 2 audit | Full engineering standards | Boundary-only: contracts, assumptions, error propagation at interaction points |
| Phase 3 fixes | Full TDD, all findings | Hardening at boundaries only, with higher bar for correctness (you are adding assertions to code you did not write) |
| Phase 4 gate | Full: cross-zone contracts, performance, sanitisers | Contract verification at interaction points only |
| Documentation | Full inline + zone-level docs | Document the interaction contracts and any hardening added |

### The Interaction Surface Is the Critical Deliverable

In a partial audit, the cross-zone contracts between target and context zones are the most important output. These contracts answer:

- What does the new code promise to the existing code?
- What does the new code assume about the existing code?
- Where do those assumptions break under edge cases, concurrency, or error conditions?
- What hardening in the existing code is needed to make the interaction safe?

The theologian's primary focus in a partial audit shifts from mapping the full architecture to mapping the interaction surface exhaustively.

### Build Variants

Some codebases compile under different modes that change the invariant sets: debug vs release, GIL vs free-threaded, platform-specific builds, feature flags via `#ifdef`. When the same code has different correctness properties depending on build configuration:

- Each invariant in the progress log gets a **mode** field indicating which build variant it applies to (e.g. "GIL-only", "free-threaded-only", "all modes")
- Phase 3 tests must specify which build mode they verify
- Phase 4 gate must run under **all relevant build modes** if the zone has mode-specific invariants — passing under one mode does not validate another
- The invariant coverage matrix tracks coverage per mode, not just per zone

This applies beyond conditional compilation. Any project with runtime feature flags, configuration-dependent behaviour, or platform-specific code paths has the same challenge: the invariant set is not fixed, it varies with the configuration.

---

## Adapting to Codebase Size

### Small codebase (< 15 source files)

- Phase 1 can be brief: one-paragraph survey, 2-3 zones, informal contract list
- Phase 2 can audit all zones before fixing any
- Phases 3-4 proceed zone by zone
- Phase 5 may be minimal
- Total effort: hours

### Medium codebase (15-50 source files)

- Phase 1 identifies 4-10 zones
- Phase 2 audits all zones, then Phases 3-4 proceed zone by zone in priority order
- Consider parallelising Phase 2 across 2-3 workers if zones are independent
- Total effort: one to several sessions

### Large codebase (50+ source files)

- Phase 1 is critical — risk ranking determines where effort goes
- **Phases 2-4 interleave per zone:** audit zone A, fix zone A, gate zone A, then audit zone B. Learnings from zone A inform zone B's audit
- Use the **sprint model:**

  **Sprint 1:** Phase 1 (map all zones) → Phase 2 (audit top 5 zones) → Phases 3-4 (fix top 5 zones)
  **Sprint 2:** Re-assess zone map → Phase 2 (audit next 5) → Phases 3-4 (fix next 5) → backtrack any sprint 1 zones
  **Sprint N:** Continue until coverage target met or diminishing returns

- Each sprint boundary is a natural checkpoint for human review
- The progress log persists across sprints — a new team can pick up where the previous left off
- Consider auditing only the top-priority zones in the first sprint; some low-risk zones may not be worth auditing at all
- Total effort: multiple sessions across days

### When to Parallelise

| What | Parallelise? | Why |
|------|-------------|-----|
| Phase 2 across zones | Yes — read-only, no interference | Multiple workers can audit different zones simultaneously |
| Phase 3 across zones | No — fixes change code, contracts can break | Sequential per zone prevents cross-zone regressions |
| Phase 3 across findings within a zone | Carefully — only if findings affect different files | Within-zone parallelism risks merge conflicts |
| Phase 4 across zones | No — each zone must be gated before the next begins | Gate is a serialisation point |

---

## Comparison with nbs-audit

| Dimension | nbs-audit | Team Audit |
|-----------|-----------|------------|
| Unit of analysis | Single file | Zone (group of files sharing invariants) |
| Parallelism | One agent per file, all at once | Broad audit can parallelise; fixes are sequential per zone |
| Cross-cutting issues | Not detected | Explicitly identified via contracts and cross-zone tests |
| Fix approach | Parallel fix agents per file | TDD per finding, zone by zone |
| Test discipline | Tests written alongside fixes | Tests written first, verified falsifiable by three-step procedure |
| Test quality gate | None — self-reported | Decoration is a defect; testkeeper verifies falsifiability |
| Coverage metric | Line/branch coverage (if any) | Invariant coverage (invariants identified vs tested) |
| Prioritisation | File count ranking | Risk-based zone ranking with blast radius evidence |
| Regression detection | Full suite after all fixes | Full suite after each zone |
| Performance monitoring | None | ABBA-interleaved benchmarks at zone gates |
| Documentation | Audit report only | Inline comments (why), zone docs (contracts), final report |
| Progress tracking | None | Formal audit progress log with zone status and backtrack history |
| Backtracking | Not supported | Structured protocol with three triggers and mini-cycles |
| Institutional memory | None (sub-agents forget) | Scribe decision log, 3Ws, progress log, chat history |
| Verification | Self-reported by fix agents | Independent verification (testkeeper, gatekeeper, theologian) |
| Team learning | None | Cumulative via 3Ws, scribe records, and sprint retrospectives |
| Session resumption | Not designed for it | Progress log enables pickup across sessions |

---

## Failure Modes to Watch For

1. **Phase skipping.** "We know where the bugs are, let's just fix them." Every phase produces artefacts the next phase needs. Skipping Phase 1 means fixing zones in random order instead of by risk priority. Skipping test planning means fixes without falsifiable tests.

2. **Scope creep during audit.** The auditor finds a bug and fixes it immediately. Now the audit report doesn't match the code, the fix has no test, and no one reviewed it. Audit is read-only. Fixes happen in Phase 3.

3. **Test-after-fix.** The pressure to "just fix it and write the test after" is strong. But a test written after the fix is a confirmation test, not a falsification test. It confirms the fix works — it doesn't prove the bug existed. The three-step falsifiability verification catches this: if the test passes both with and without the fix, it is decoration.

4. **Hardening without documentation.** Adding fifty assertions without explaining why they exist creates code that is technically safer but harder to maintain. Future developers will remove assertions they don't understand. Phase 3 step 2d is not optional.

5. **Report inflation.** Treating every missing assertion as equally urgent obscures the real bugs. The severity classification (BUG > SECURITY > HARDENING) and the blast-radius ranking exist to prevent this.

6. **Consensus cascade at phase boundaries.** One agent says "Phase 2 looks done," others agree without verifying. The supervisor verifies exit criteria are met before declaring a phase complete.

7. **Zone boundaries treated as permanent.** The zone map is a hypothesis. If evidence shows the boundaries are wrong — files share invariants across zones, contracts were missed — revise the map. Rigid boundaries prevent learning.

8. **Ignoring backtrack triggers.** When zone B's audit reveals a problem in zone A, the team may resist reopening zone A because it feels like going backward. But a contract violation in a hardened zone is worse than in an unhardened one — it carries false confidence. Backtracking is forward progress.

---

## Getting Started

1. State the terminal goal: what does "audited and improved" mean for this specific codebase?
2. Assign team roles per the table above
3. Begin Phase 1: Architectural Survey
4. Initialise the audit progress log
5. Proceed through phases, meeting exit criteria before advancing
6. For large codebases, plan sprints and review at sprint boundaries
7. After Phase 5, the supervisor asks the human to review the final report

The approach is designed to be resumed across sessions. The audit progress log, scribe decision log, and chat history persist. A new session — or a new team — can pick up where the previous one left off by reading the progress log and the most recent phase deliverables.
