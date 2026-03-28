# Making Code Shareable

A guide for AI teams preparing a codebase for external review. Based on the [NBS engineering standards](../engineering-standards.md) and [falsifiability principles](../falsifiability.md), extracted from the experience of preparing a parallel garbage collector for CPython PR submission.

## The Falsification Criterion

The work is done when this is true: **a competent reviewer can clone the repo, build, run tests, run benchmarks, and understand the design — without asking any questions.**

If they have to ask a question, something is missing. The question identifies what.

## Sequence

Work these phases in order. Each depends on the previous one being complete. Do not parallelise across phases — the team must agree that phase N is done before starting phase N+1. Within a phase, agents may work in parallel on independent tasks.

Why the ordering is strict:
- Assessment before cleanup — you cannot clean what you do not understand. Deleting a "junk" file that is actually the only working test wastes days.
- Cleanup before audit — auditing code mixed with junk wastes time on files that will be deleted.
- Audit before testing — fixing bugs found during audit changes the binary. Testing a pre-audit binary proves nothing about the final code.
- Testing before documentation — documentation must describe verified behaviour, not hoped-for behaviour. If tests fail, the documentation is speculative.
- Documentation before upstream sync — the merge will change APIs and constants. But having documentation first means you know what to re-verify.
- Upstream sync before final review — the merge is the last destructive operation. Final review must be on the exact code that will be shared.

### Phase 0: Assessment

Understand what exists before changing anything. This phase is read-only.

An AI team arriving at an unfamiliar project does not know what is working code, what is a dead end, and what is junk. Proceeding to cleanup without this understanding destroys valuable artefacts and preserves worthless ones.

**Step 1: Catalogue.** List everything in the repository. Source files, tests, documentation, scripts, build system, generated files, configuration. Use `find`, `ls -R`, `git log --stat`. Do not read file contents yet — just build the map.

**Step 2: Classify.** For each artefact, determine its status:

| Classification | Meaning | Examples |
|---------------|---------|----------|
| Source | Code that implements the project | `.c`, `.py`, `.rs` files in `src/` |
| Test | Code that verifies the project | Test files, test fixtures, test scripts |
| Documentation | Text that explains the project | README, design docs, API reference |
| Build | Infrastructure that builds the project | Makefile, configure, CMakeLists, CI config |
| Generated | Output of a build or tool, not hand-written | `.o`, `.pyc`, `build/`, `dist/` |
| Junk | No clear purpose, likely stale | `temp.py`, `old_version/`, `test2.py.bak` |
| Unknown | Cannot determine without reading or asking | Anything ambiguous |

Do not guess. If a file's purpose is unclear, classify it as Unknown. Do not delete Unknown files during cleanup — investigate them first.

**Step 3: Check what works.** Before cleaning anything:

```bash
# Does it build?
make 2>&1 | tail -5     # or the project's build command

# Do tests exist? Do they pass?
find . -name '*test*' -o -name '*_test.*' | head -20
make test 2>&1 | tail -10   # or however tests run

# Is there a .gitignore? Is it correct?
git status --short | head -20   # untracked files that should be ignored?
```

Record the answers. A project that builds and has passing tests is in a different state from one that does not compile.

**Step 4: Identify gaps.** What is missing for the terminal goal?

| Gap | Severity | Implication |
|-----|----------|-------------|
| No tests | Critical | Phase 3 requires writing tests from scratch, not just running them |
| No documentation | Critical | Phase 4 requires writing docs from scratch |
| Build is broken | High | Must be fixed before any other phase can verify its work |
| No .gitignore | Medium | Generated files are committed, inflating the repo |
| No CI/linting | Low | Style compliance in Phase 2 must be manual |

**Step 5: Post the assessment.** Write a short report to chat:

```
ASSESSMENT: [project name]

Files: N source, M tests, K docs, J generated, L junk, P unknown
Build: [passes / fails / no build system]
Tests: [N pass, M fail, K skip / no tests]
Docs: [exist / partial / none]
Gaps: [list the critical and high gaps]

Recommended focus for Phase 1: [what needs the most cleanup work]
```

This report drives the rest of the process. A project with no tests needs a different time allocation than one with a full test suite. A project with no documentation needs Phase 4 treated as a major effort, not a polish pass.

### Phase 1: Cleanup

Remove everything that is not the project.

| Task | Falsification |
|------|---------------|
| Delete generated files, build artefacts, editor configs, IDE projects | `find . -name '*.pyc' -o -name '*.o' -o -name '.DS_Store'` returns nothing |
| Delete stray scripts, notebooks, one-off experiments | Every file in the repo has a clear purpose. Ask: "if a reviewer sees this, will they understand why it exists?" |
| Scrub internal references — hostnames, paths, internal tools, employee names, private URLs | `grep -rn` for the organisation's internal domain, tool names, and infrastructure. Zero hits. |
| Rewrite README for the external reader | A stranger reads the README and can build the project on the first try. If they cannot, the README is wrong. |
| Fix .gitignore — no committed build artefacts, no tracked generated files | `git status` after a clean build shows nothing untracked that should be ignored |

**Do not fix bugs or add features in this phase.** Cleanup only. The temptation to "quickly fix this while I'm here" leads to untested changes mixed with cleanup commits.

**Commit at the end of this phase.** The reviewer should be able to see that cleanup was done deliberately, not accidentally mixed with implementation work.

### Phase 2: Code Audit

Review the implementation against the target community's standards.

| Task | Falsification |
|------|---------------|
| Style compliance — indentation, naming, brace style, comment style | Automated linter passes with zero warnings, or manual review against the project's style guide |
| Assertion discipline — preconditions on public functions, postconditions on complex operations, invariants at state transitions | Read every public function. Does it validate its inputs? Does it document what it promises? |
| TODO/FIXME/HACK audit — resolve, document, or remove every one | `grep -rn 'TODO\|FIXME\|HACK\|XXX\|TEMP\|WORKAROUND'` returns zero, or each remaining instance has a documented reason |
| Dead code removal — unused functions, commented-out blocks, vestigial #ifdefs | No function that is never called. No block that is always commented out. |
| Thread safety review (if applicable) — shared data access, atomic ordering, lock discipline | Every shared variable is either atomic with documented ordering, mutex-protected, or read-only after init. Flag concerns — do not try to formally verify. |

**Honesty over tidiness.** If you find a real bug during audit, fix it and test it properly. Do not paper over it. Do not mark it as "known issue" unless it genuinely cannot be fixed before sharing. A known issue with a documented workaround is honest. A known issue with no workaround is a warning to the reviewer that the code is not ready.

### Phase 3: Build and Test Verification

The build must work. The tests must pass. No skips, no workarounds, no "it passes if you run it twice".

| Task | Falsification |
|------|---------------|
| Build from clean state | `make clean && ./configure && make` succeeds with zero warnings. On a fresh clone, not your development tree. |
| Build in all supported configurations | Every configuration documented in the README builds and passes tests. If the project supports debug and release, test both. If it supports multiple platforms, test all documented ones. |
| Run the full test suite | Every test passes. If a test is flaky, fix the test or fix the code. Do not skip it. |
| Run sanitisers — ASAN, TSAN, UBSAN, Valgrind — whatever is appropriate | Zero errors from the project's code. Upstream/third-party errors are documented and suppressed explicitly, not silently ignored. |
| Run benchmarks (if applicable) | Benchmarks produce results consistent with documented claims. If the docs say "3x speedup", the benchmarks show approximately 3x. |

**No skips.** A skipped test is a test that the reviewer will run. If it fails for them, you have lost trust. Fix it or remove it — do not leave it broken and skipped.

**No workarounds.** "Run `export THING=1` before building" is a workaround. If the build requires it, put it in the configure script. If the test requires it, put it in the test harness. The reviewer should not need secret knowledge.

**Test both modes, both builds, both platforms.** If your project supports two configurations (e.g. GIL and free-threaded), test both completely. Do not assume that testing one implies the other works. They share code but they are not the same binary.

**Document the test matrix.** Write down exactly which configurations were tested, which test suites were run, and what the results were. A table is better than prose:

| Configuration | Test suite | Result |
|--------------|-----------|--------|
| Debug + GIL | stdlib (492 tests) | 472 pass, 20 skip (upstream) |
| Debug + FTP | stdlib (507 tests) | 496 pass, 11 skip (upstream) |
| ASAN + GIL | parallel GC tests | 92/92 pass, 0 errors |
| ASAN + FTP | parallel GC tests | 152/152 pass, 0 errors |
| TSAN + FTP | benchmarks | 0 warnings |

### Phase 4: Documentation

Write for the reviewer, not for yourself.

| Document | Purpose | Falsification |
|----------|---------|---------------|
| README | Clone → build → run → understand | A stranger follows it start to finish with no errors |
| Architecture / Design | How it works, why it works this way | Every factual claim (struct names, function signatures, constants) verified against source code |
| Build and Test | How to build, how to test, what the results mean | Every command in the doc is copy-pasteable and produces the documented output |
| Getting Started | Quickest path from "I have the repo" to "I see it working" | Tested on a clean machine or clean checkout |
| Benchmarks (if applicable) | How to reproduce performance claims | Scripts exist, run, and produce results within the documented range |

**Verify every factual claim against source.** AI agents fabricate documentation. This is the single most common failure mode. An agent will write "this function returns a dictionary with keys `a`, `b`, `c`" without running the function. The dictionary actually has keys `x`, `y`. The documentation looks professional. It is wrong.

Cross-reference every struct name, function signature, constant value, file path, command, and expected output against the actual source code or actual execution. Use the medic or an independent agent for this — the agent that wrote the documentation cannot reliably verify it because it may reproduce the same hallucination.

**Do not document aspirations.** Document what the code does, not what you plan for it to do. "Future work" sections are fine. Describing unimplemented features as if they exist is not.

### Phase 5: Upstream Sync (if applicable)

If the project is a fork or branch, sync with upstream before sharing.

| Task | Falsification |
|------|---------------|
| Merge or rebase against current upstream | The merge completes. Conflicts are resolved. The result compiles. |
| Fix post-merge bugs | Every test that passed before the merge still passes after |
| Re-run the full verification matrix | The same matrix from Phase 3, on the merged code |
| Verify documentation is still accurate | Constants, function signatures, and APIs may have changed in upstream. Re-verify. |

**The merge will break things.** Expect it. Budget time for it. The post-merge bugs are not failures — they are expected consequences of integration. Fix them properly, do not work around them.

**Re-run everything.** The merge produces a different binary. A different binary has never been tested. "It passed before the merge" is not evidence that it passes after.

### Phase 6: Final Review

The last pass before sharing.

| Task | Falsification |
|------|---------------|
| Read the README as a stranger | Can you build the project following only the README? |
| Run `git log --oneline` | Does the commit history tell a coherent story? |
| Run `git diff upstream..HEAD --stat` | Is the diff reasonable? No committed binaries, no 10,000-line generated files? |
| Check for private information | `grep -rn` for internal hostnames, employee names, private URLs, API keys. Zero hits. |
| Check for licensing issues | Every file has appropriate headers. Third-party code is attributed. |

## Practices

These are not phases — they apply throughout.

### Root Cause, Not Workaround

When a test fails, fix the code or fix the test. Do not:
- Skip the test
- Add a retry loop
- Mark it as "known flaky"
- Add an environment variable that makes it pass

A workaround is a lie. It tells the reviewer "this works" when it does not. The reviewer will find the lie. Trust is expensive to rebuild.

### Single Builder

Only one agent modifies the build directory at a time. Concurrent builds, concurrent configures, concurrent test runs in the same tree produce corrupt state that is extremely difficult to diagnose. Coordinate via chat. Wait your turn.

### Patience

Some phases take hours. ASAN stdlib runs are slow. Upstream merges produce surprises. Documentation verification is tedious.

Do not cut corners to go faster. The reviewer will find every corner you cut. The time saved is borrowed against the reviewer's trust, at punitive interest.

### Honest Reporting

Report what happened, not what you wanted to happen.

- "472/492 tests pass, 20 skipped (upstream issues, not ours)" is honest
- "All tests pass" when 20 were skipped is a lie
- "ASAN clean — zero errors from our code, 1 upstream leak in readline" is honest
- "ASAN clean" when you suppressed warnings is a lie

If something is not right, say so. A reviewer who discovers an undisclosed problem loses trust in everything else you reported. A reviewer who reads an honest disclosure of a minor issue gains trust in the rest.

## References

- [Engineering Standards](../engineering-standards.md) — the verification cycle, assertion protocol
- [Falsifiability](../falsifiability.md) — the epistemic foundation
- [Goals](../goals.md) — terminal vs instrumental, the pathos question
- [Verification Cycle](../verification-cycle.md) — design → plan → test → code → document
