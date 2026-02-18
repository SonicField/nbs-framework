# NEW TERMINAL GOAL

## Current: Commit Option D Performance Fix

**Status:** Gate GREEN. Slot_read 0.80x → 0.96x (independently verified by testkeeper). Commit pending source recovery (generator.cpp changes lost from working tree, being re-applied).

**Action:** Commit the 5-file Option D change on build-host then move to next goal.

---

## Next Terminal Goal: All CinderX Tests Running

**Requirement from Alex (18 Feb 2026):**

> All CinderX tests must RUN. No excuses, no missing modules, no workarounds, no skips. EVERYTHING or it is not done.

### Definition of Done

1. A **single test runner script** exists on build-host that Alex can point her human team to
2. The script runs **every CinderX test file** — no exceptions
3. Every test **executes** — no import errors, no collection errors, no skips
4. Known failures (e.g., 3 async generator CANNOT_SPECIALIZE) are reported as **honest failures**, not hidden
5. The output is a clear report: N pass, M fail, 0 errors, 0 skips
6. Every CinderX test file appears in the report

### Known Blockers

- `cinderx.compiler.opcode` module not available on aarch64 build — blocks upstream test suites (`test_jit_attr_cache.py`, `test_cinderjit.py`, etc.)
- Root cause: `cinderx.opcode` is likely a C extension module not being compiled/installed for aarch64
- Fix: investigate cmake build, ensure the opcode module is built for aarch64

### Deliverables

1. Fix `cinderx.compiler.opcode` import failure
2. Write the unified test runner script
3. Run all tests, produce the report
4. Commit the test runner and any fixes to the repo

### Falsifier

If any CinderX test file fails to EXECUTE (import error, collection error, or skip), the goal is NOT met. Individual test case failures are acceptable as long as they run and report honestly.
