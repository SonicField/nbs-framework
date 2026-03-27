# Feature Request: Dedicated Test Runner

## Problem

The current test infrastructure is a collection of shell scripts and C binaries invoked sequentially by `make`. This has several problems:

1. **No partial runs.** `make test` runs everything or nothing. There is no way to run a single test file, a single test case, or a named suite.
2. **First-failure abort.** Make stops on the first non-zero exit. A failure in `test_nbs_chat_watchdog.sh` prevents `test_nbs_team_check.sh` from ever executing. You cannot see the full picture.
3. **No structured output.** Results are free-form text. Grepping for `PASS` and `FAIL` is fragile. There is no machine-readable report for CI or tooling.
4. **No colour.** All output is monochrome. In a terminal with hundreds of lines of test output, finding failures requires scrolling.
5. **No timing.** No per-test or per-suite duration. Slow tests are invisible.
6. **Duplicated targets.** `test`, `test-debug`, `test-asan` repeat the same list of scripts. Adding a new test requires editing three places.

## Proposal

A standalone test runner — `bin/nbs-test-runner` (bash or C) — that owns test discovery, execution, reporting, and exit codes. Make delegates to it.

### CLI

```bash
# Run everything
nbs-test-runner

# Run one suite
nbs-test-runner --suite unit
nbs-test-runner --suite integration

# Run one test file
nbs-test-runner --file test_nbs_team_check.sh

# Run one test case (within a C unit test binary)
nbs-test-runner --test "child_pipe_compact removes closed entries"

# Output format
nbs-test-runner --format terminal    # default: colour output
nbs-test-runner --format json        # machine-readable
nbs-test-runner --format plain       # no ANSI, for piping/logs
```

### Suites

Tests are grouped into suites. The runner discovers them by convention:

| Suite | Pattern | Description |
|-------|---------|-------------|
| `unit` | `tests/test_*_unit` (compiled binaries) | Fast, no I/O, no processes |
| `integration` | `tests/automated/test_*.sh` | Shell scripts, spawn processes |
| `asan` | Same as unit + integration, built with ASan | Memory safety |

### Terminal Output (Colour)

```
=== nbs-chat test suite ===

  unit tests
    ✓ test_chat_file_unit          32 passed   0.04s
    ✓ test_base64_unit             28 passed   0.02s
    ✓ test_terminal_unit           41 passed   0.01s
    ✗ test_watchdog_unit           29 passed, 1 failed   0.01s
      FAIL: cooldown blocks restart within 120s (watchdog.c:89)

  integration tests
    ✓ test_nbs_chat_lifecycle      26 passed   1.2s
    ✓ test_nbs_chat_terminal       40 passed   8.3s
    ✗ test_nbs_chat_watchdog       23 passed, 8 failed   45.1s
    ✓ test_nbs_chat_bus            12 passed   2.1s
    ✓ test_nbs_team_check          15 passed   0.5s

  summary
    195 passed, 9 failed, 2 files with failures
    total time: 57.3s
```

Colours:
- Green `✓` for all-pass files
- Red `✗` for files with failures
- Dim for timing
- Bold for suite headers
- Red for `FAIL:` lines (indented under the failing file)

### JSON Output

```json
{
  "timestamp": "2026-03-27T14:30:00Z",
  "suites": [
    {
      "name": "unit",
      "files": [
        {
          "file": "test_chat_file_unit",
          "passed": 32,
          "failed": 0,
          "duration_ms": 40,
          "failures": []
        }
      ]
    }
  ],
  "summary": {
    "total_passed": 195,
    "total_failed": 9,
    "total_duration_ms": 57300
  }
}
```

### Test Protocol

Each test file (C binary or shell script) must conform to a simple protocol:

- **Exit code 0** = all tests passed
- **Exit code non-zero** = at least one failure
- **stdout** contains lines matching `PASS:` and `FAIL:` (the runner counts these)
- Optionally: a final line `=== Results: N passed, M failed ===` (the runner parses this if present, otherwise counts PASS/FAIL lines)

This is already the convention in every existing test file. No test changes needed.

### Make Integration

```makefile
test: all install
	bin/nbs-test-runner --suite unit --suite integration

test-debug: debug install
	bin/nbs-test-runner --suite unit --suite integration

test-asan: asan install
	bin/nbs-test-runner --suite unit --suite integration --build-mode asan

test-unit: all
	bin/nbs-test-runner --suite unit

test-file:
	bin/nbs-test-runner --file $(FILE)
```

Make sees a single exit code: 0 if all suites pass, 1 if any test failed. The runner always runs all tests regardless of individual failures (no first-failure abort).

### Discovery

The runner finds tests automatically:

- **Unit tests**: glob `tests/test_*_unit` for compiled binaries. The Makefile must build them first (the runner does not compile).
- **Integration tests**: glob `tests/automated/test_*.sh` for shell scripts.
- **Exclusions**: a `.nbs-test-skip` file can list patterns to exclude (e.g. tests that need special hardware).

### Nice-to-Have (Not Required for V1)

- `--parallel N` to run N test files concurrently (unit tests are independent)
- `--filter <regex>` to match test names within files
- `--retry-failed` to re-run only tests that failed on the last run
- `--diff` to run only tests for files changed since last commit
- TAP output format for CI integration
- JUnit XML for Jenkins/GitHub Actions

## What Does NOT Change

- Individual test files — they already follow the protocol
- Test naming conventions — already consistent
- The C unit test macro (`TEST_ASSERT`, `TEST_PASS`) — unchanged
- The shell test `check()` function — unchanged

## Implementation Notes

Bash is probably sufficient for V1. The runner is a thin orchestrator: discover files, fork each one, capture output, count results, format report. No need for C unless performance matters (it won't — the tests themselves dominate runtime).

The hardest part is parsing the existing output format reliably. The `=== Results: N passed, M failed ===` line is the most reliable signal. Falling back to counting `PASS:` and `FAIL:` lines handles files that don't emit the summary.
