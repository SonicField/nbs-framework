---
description: Audit codebase against engineering standards, then fix with adversarial TDD
allowed-tools: Bash, Read, Write, Edit, Glob, Grep, Task, AskUserQuestion
---

# NBS Engineering Standards Audit

This tool audits an entire codebase against the engineering standards, then produces a parallel fix plan with adversarial TDD.

**Budget:** A typical codebase produces 50–100+ violations. The audit phase launches one sub-agent per source file in parallel. The fix phase launches parallel sub-agents per file to write tests and apply fixes.

---

## Phase 1: Discover Source Files

Identify the project's language and source layout. Find all source files excluding re-export stubs, vendored dependencies, and generated code.

Examples by language:

```bash
# Python
find <src_dir> -name "*.py" -not -path "*__pycache__*" -not -path "*vendor*" -not -path "*generated*" | sort

# Rust
find <src_dir> -name "*.rs" -not -path "*target*" | sort

# C/C++
find <src_dir> \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) -not -path "*build*" | sort

# TypeScript/JavaScript
find <src_dir> \( -name "*.ts" -o -name "*.tsx" \) -not -path "*node_modules*" -not -path "*dist*" | sort
```

Exclude trivial files (pure re-exports, empty `__init__.py`, etc.) from the audit list.

Count the files. If more than 30, ask the user whether to proceed (this will launch 30+ parallel agents).

---

## Phase 2: Parallel Audit

Launch **one sub-agent per source file**, all in parallel, using the Task tool with `run_in_background=true`. Use the latest, most powerful model available.

Each agent receives the same prompt template with the file path substituted:

### Agent Prompt Template

```
You are auditing `{FILE_PATH}` against engineering standards.

Read the engineering standards from `~/.nbs/concepts/engineering-standards.md`.
If a project-level CLAUDE.md references a different engineering-standards.md, read that too and apply the union of both.

Apply every standard in that document to this audit. If the file cannot be read,
abort with an error — do not fall back to hardcoded standards.

## Your Task

Read the source file. Produce a concise report listing ONLY concrete violations:

For each violation, state:
1. **Line number(s)** in the source file
2. **Standard violated** (precondition, postcondition, invariant, silent failure, unfalsifiable claim, assertion message)
3. **Severity**: BUG (correctness issue), SECURITY (security-relevant silent failure), or HARDENING (missing assertion/guard)
4. **Specific fix** — the exact code change needed

Focus on:
- Missing preconditions (assert on entry)
- Missing postconditions (assert on exit)
- Missing invariants (assert at state transitions)
- Silent error swallowing (bare except, pass after catch)
- Assertions without meaningful messages
- Unfalsifiable claims in docstrings
- Unreachable code paths / guards

Do NOT report: style issues, naming, documentation gaps (unless unfalsifiable claims), or things already correct.

Output format: Markdown list grouped by severity (BUG first, then SECURITY, then HARDENING). Include a summary table at the end with violation counts by category.
```

### Collecting Results

Wait for all agents to complete using `TaskOutput`. Collect all reports.

Write the consolidated audit report to `.nbs/audit-report.md` so fix agents can read it.

### Consolidated Summary

After all agents complete, produce a consolidated summary:

1. **Bug count** — actual correctness issues (wrong behaviour, unreachable guards, data corruption)
2. **Security count** — silent failures in security-relevant paths
3. **Hardening count** — missing assertions that would catch future bugs
4. **File ranking** — files ordered by violation count (highest first)
5. **Unfalsifiable claims** — docstrings making unverified promises

Present this summary to the user before proceeding to Phase 3.

---

## Phase 3: Fix Plan

After the user reviews the audit summary, produce a fix plan:

### Priority Order

1. **BUGS** — fix immediately, these are wrong now
2. **SECURITY** — fix next, silent failures in security paths
3. **HARDENING** — systematic pass, file by file

### Plan Structure

For each file to fix, define:

| Field | Content |
|-------|---------|
| File | Path to source file |
| Violations | Count and categories |
| Test file | Path to test file (existing or new) |
| Approach | TDD — write adversarial tests FIRST, then apply fixes |

### Adversarial Test Requirements

Every fix MUST have a test that:

1. **Proves the assertion fires** — call the function with invalid input and verify the assertion/error is raised with the correct message
2. **Proves the assertion does NOT fire** — call the function with valid input and verify it succeeds
3. **Is adversarial** — uses boundary values, type confusion, empty inputs, None, and other inputs designed to break the code

The test must be **falsifiable**: if the assertion were removed, the test must fail. A test that passes regardless of whether the assertion exists is not a test — it is decoration.

#### Example Patterns (adapt to project language)

**Precondition test — subclass confusion:**
```python
def test_validate_port_rejects_bool():
    """Bool is subclass of int — must be caught before int check."""
    with pytest.raises(ValueError, match="got bool"):
        validate_port(True)
```

**Silent failure test — verify logging:**
```python
def test_config_load_logs_permission_error(tmp_path, caplog):
    """Permission-denied file must produce a warning, not silence."""
    config_file = tmp_path / "config"
    config_file.write_text("key = value")
    config_file.chmod(0o000)
    try:
        with caplog.at_level(logging.WARNING):
            load_config(config_file)
        assert "Could not read" in caplog.text
    finally:
        config_file.chmod(0o644)
```

**Unfalsifiable claim test — verify or remove:**
```python
def test_collector_concurrent_access():
    """Docstring claims thread-safety — prove it or delete the claim."""
    import threading
    collector = MyCollector()
    errors = []

    def hammer():
        try:
            for i in range(1000):
                collector.add(i)
        except Exception as e:
            errors.append(e)

    threads = [threading.Thread(target=hammer) for _ in range(10)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert not errors, f"Thread-safety claim violated: {errors}"
    assert len(collector.items) == 10000, \
        f"Items lost under concurrent access: {len(collector.items)}"
```

**Postcondition test — verify invariant on output:**
```python
def test_normalise_hostname_postcondition():
    """Normalised hostname must be lowercase and within length bounds."""
    result = validate_hostname("MyServer.Example.COM")
    assert result == result.lower(), "Postcondition: result not lowercase"
    assert 0 < len(result) <= 253, "Postcondition: length out of range"
```

---

## Phase 4: Parallel Fix Execution

Launch **one sub-agent per file** to apply fixes, all in parallel. Each agent:

1. Reads its section of `.nbs/audit-report.md`
2. Reads the source file and existing test file (if any)
3. **Writes adversarial tests FIRST** (TDD — tests must demonstrate the gap before the fix)
4. Applies the fixes from the audit report
5. Runs the tests for that file to verify
6. Reports: tests written, fixes applied, any issues

### Agent Prompt Template for Fixes

```
You are fixing engineering standards violations in `{FILE_PATH}`.

Read the audit report for this file from `.nbs/audit-report.md` (find the section for your file).

## Instructions

1. Read the source file and existing test file
2. For each violation in the audit report:
   a. Write an adversarial test that PROVES the violation exists (test should pass before fix if testing for silent failure, or demonstrate the missing assertion)
   b. Apply the fix from the audit report
   c. Verify the test now passes (for assertion additions) or correctly catches the new behaviour
3. Run the test file to verify all tests pass
4. Report what you did

Test requirements:
- Every precondition assertion must have a test proving it fires on invalid input
- Every postcondition assertion must have a test proving it fires on corrupted output (if possible to trigger)
- Every silent-failure fix must have a test proving the warning/error is now logged
- Every unfalsifiable claim must either get a verification test or have the claim removed
- Tests must use adversarial inputs: None, empty strings, boundary values, wrong types, subclass confusion

Use the project's test runner: `{TEST_COMMAND}`
Write tests in: `{TEST_FILE_PATH}`
```

### After All Agents Complete

1. Run the **full test suite** to verify no regressions — adding assertions to one file can change exception types that other files depend on
2. Produce a summary: violations fixed, tests added, any remaining issues
3. Commit with a message listing the violation categories addressed

---

## Scaling Notes

This tool is designed for maximum parallelism:

- **Audit phase**: N files → N parallel agents (all read-only, no dependencies)
- **Fix phase**: N files → N parallel agents (each writes to different files)
- **Typical run**: 20 source files → 20 audit agents + 20 fix agents = 40 agent invocations
- **Large codebase**: 50+ files → ask user before launching, consider batching

The audit agents are read-only. The fix agents each write to their own source and test files, so parallel execution is safe. However, adding assertions can change exception types across module boundaries — the full test suite run after all fixes is mandatory.

## When to Use

- After a major refactor (verify nothing was silently broken)
- Before a release (systematic hardening pass)
- When onboarding a new codebase (understand where the gaps are)
- Periodically (engineering standards drift over time)

## What This Produces

1. **Audit report** per file — violations with line numbers, severities, and specific fixes
2. **Consolidated summary** — bug count, security count, hardening count, file ranking
3. **Fix plan** — prioritised by severity with TDD approach
4. **Adversarial tests** — proving every assertion works and every silent failure is now observable
5. **Hardened codebase** — with executable specifications (assertions) at every boundary
