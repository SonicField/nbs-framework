# nbs-ts Test Plan

**Author:** testkeeper
**Date:** 2026-03-23
**Status:** Draft — awaiting supervisor/human review

## 0. Scope

This plan covers all tests for `nbs-ts` (NBS Terminal Service), the C11 library and CLI that replaces `pty-session` (tmux wrapper) for local process management. Tests are organised by phase to match the implementation plan.

The plan does NOT cover:
- SSH completion signalling (out of scope per design doc)
- Remote-side behaviour (unchanged)
- Human TUI beyond basic `attach` smoke test

## 1. Test Architecture

### 1.1 Three Layers

| Layer | Language | Location | What it tests |
|-------|----------|----------|---------------|
| **C unit tests** | C | `src/nbs-ts/test_nbs_ts.c` | Library API directly — no fork+exec, no CLI parsing |
| **CLI integration tests** | Bash | `tests/automated/test_nbs_ts_*.sh` | CLI commands end-to-end via the `nbs-ts` binary |
| **System tests** | Bash | `tests/automated/test_nbs_ts_system_*.sh` | Multi-component: sidecar transport, worker spawn, agent lifecycle |

### 1.2 Conventions

- C tests compile with `$(COMMON_CFLAGS) -O2 -DTEST_BUILD -I../nbs-common` (same as pty-session)
- C tests linked with ASan+UBSan: `-fsanitize=address -fsanitize=undefined`
- Shell tests use `set -euo pipefail`, PID-qualified session names (`test-xxx-$$`), trap cleanup
- All shell tests are self-contained — no test depends on another test's state
- Exit codes follow existing convention: 0=pass, 1=fail, 2=session not found, 3=timeout, 4=invalid args
- `assert_check.c` verifies NDEBUG is not defined (same as all NBS C components)

### 1.3 Falsification Principle

Every test must be able to fail. For each test below, the "Falsification" column states what breaks it. If the falsification condition cannot occur, the test is decoration and must be removed or rewritten.

---

## 2. Phase 1: Core Library Tests

### 2.1 C Unit Tests (`src/nbs-ts/test_nbs_ts.c`)

These test the C library API directly, compiled into the same binary.

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| U1 | `nbs_ts_create("echo hello", NULL)` succeeds | Returns non-NULL session pointer | Fails if PTY allocation, fork, or log file creation breaks |
| U2 | `nbs_ts_create(NULL, NULL)` fails safely | Returns NULL, does not crash | Fails if NULL command is not checked |
| U3 | `nbs_ts_send` writes to PTY | Send "echo MARKER\n", read-new contains MARKER | Fails if write(master_fd) fails or output thread dies |
| U4 | `nbs_ts_send` with NULL session | Returns error code, no crash | Fails if NULL not guarded |
| U5 | `nbs_ts_send` with empty string | Returns success (zero-length write is valid) | Fails if empty string triggers error path |
| U6 | `nbs_ts_read_new` returns new output | After send+sleep, returns bytes containing MARKER | Fails if read offset tracking is broken |
| U7 | `nbs_ts_read_new` returns 0 when no new output | Call twice without send between | Fails if offset not advanced after first read |
| U8 | `nbs_ts_read` with offset | Read from byte 0, then from byte N, verify content | Fails if pread offset arithmetic is wrong |
| U9 | `nbs_ts_read_new_stripped` removes ANSI | Send command producing colour output, stripped result has no `\033[` | Fails if strip_ansi integration is broken |
| U10 | `nbs_ts_wait_complete` returns exit code | Create session with `exit 42`, wait-complete → exit_code == 42 | Fails if PROMPT_COMMAND injection or completion.log parsing breaks |
| U11 | `nbs_ts_wait_complete` timeout | Create bash session, wait-complete with 1s timeout, no command sent | Fails if inotify wait doesn't respect timeout |
| U12 | `nbs_ts_wait_pattern` finds pattern | Send `sleep 1; echo MARKER`, wait-pattern "MARKER" with 5s timeout | Fails if inotify on output.log doesn't wake or pattern scan misses |
| U13 | `nbs_ts_wait_pattern` timeout | Wait for pattern that never appears, 1s timeout | Fails if timeout not enforced |
| U14 | `nbs_ts_status` returns alive | Create bash session, check status == alive | Fails if waitpid check is wrong |
| U15 | `nbs_ts_status` returns dead after exit | Create `exit 0` session, sleep, check status == dead | Fails if child reaping is broken |
| U16 | `nbs_ts_destroy` cleans up | Destroy session, verify session directory removed | Fails if cleanup doesn't remove files |
| U17 | `nbs_ts_destroy` on already-dead session | Session exited naturally, then destroy — no crash, files cleaned | Fails if double-cleanup or dead-process handling is broken |
| U18 | Output log is append-only | Send 3 commands, read log file directly, verify all 3 outputs present in order | Fails if log is truncated or overwritten |
| U19 | Completion log has correct records | Send 3 commands (`true`, `false`, `exit 7`), parse completion.log — 3 records with exit codes 0, 1, 7 | Fails if PROMPT_COMMAND injection or record format is wrong |
| U20 | Heartbeat file mtime advances | Create session, stat heartbeat, sleep 2s, stat again — mtime increased | Fails if heartbeat thread is not running |
| U21 | PID file contains correct PID | Create session, read pid file, `kill -0 <pid>` succeeds | Fails if wrong PID written |
| U22 | Meta file contains command and cwd | Create session, read meta — command and cwd match | Fails if metadata write is broken |

### 2.2 CLI Integration Tests (`tests/automated/test_nbs_ts_*.sh`)

#### test_nbs_ts_lifecycle.sh — Core create/send/read/kill cycle

Derived from: `test_pty_session_lifecycle.sh`, `test_pty_session_create.sh`

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| L1 | Create interactive session | `nbs-ts create 'bash'` returns handle, exit 0 | Fails if PTY creation fails |
| L2 | List shows session alive | `nbs-ts list` output contains handle with "alive" status | Fails if list doesn't scan session dirs |
| L3 | Send command | `nbs-ts send <handle> 'echo MARKER'` exit 0 | Fails if write to master fd fails |
| L4 | Read-new shows output | `nbs-ts read-new <handle>` contains MARKER | Fails if output capture thread died or offset tracking broken |
| L5 | Read-new is idempotent-empty | Second `nbs-ts read-new` returns empty (no new output) | Fails if read offset not advanced |
| L6 | Kill session | `nbs-ts kill <handle>` exit 0 | Fails if SIGTERM delivery or cleanup fails |
| L7 | List shows session gone | Handle absent from `nbs-ts list` after kill | Fails if session directory not removed |

#### test_nbs_ts_oneshot.sh — One-shot command lifecycle

Derived from: `test_pty_session_create.sh` tests 1, 4, 5

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| O1 | One-shot output captured | `nbs-ts create 'echo HELLO'`, read-new → contains HELLO | Fails if fast-exit command output lost (pipe-pane race equivalent) |
| O2 | One-shot session exits | After command completes, `nbs-ts status` reports dead | Fails if child exit not detected |
| O3 | Exit code captured | `nbs-ts create 'exit 42'`, wait-complete → exit code 42 | Fails if completion signalling broken for one-shot |
| O4 | Command-not-found exits cleanly | `nbs-ts create 'nonexistent_xyz'`, session status is dead, no zombie | Fails if error path leaves zombie |
| O5 | Failed command log non-empty | Log file for command-not-found session contains error text | Fails if stderr not captured to output.log |

#### test_nbs_ts_wait.sh — Wait operations

Derived from: `test_pty_session_wait.sh`, `test_pty_session_wait_fallback.sh`

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| W1 | wait-pattern finds existing pattern | Send marker, then wait-pattern — found immediately | Fails if pattern scan doesn't search existing output |
| W2 | wait-pattern finds delayed pattern | Send `sleep 2; echo MARKER`, wait-pattern with 5s timeout — found after ~2s | Fails if inotify doesn't wake on output.log append |
| W3 | wait-pattern timeout | Wait for pattern never produced, 3s timeout — exit code 3, elapsed ~3s | Fails if timeout not enforced or wrong exit code |
| W4 | wait-complete returns exit code | Send `exit 7`, wait-complete → completion record with code 7 | Fails if completion.log not written or parsed wrong |
| W5 | wait-complete timeout | Bash session, no command sent, wait-complete 1s timeout — exit code 3 | Fails if timeout broken for completion wait |
| W6 | wait-pattern on dead session — pattern in log | Session exited, pattern was in output — wait finds it in log | Fails if dead-session fallback to log scan is missing |
| W7 | wait-pattern on dead session — pattern NOT in log | Session exited, pattern was never produced — exit code 2 | Fails if dead session returns wrong code |
| W8 | wait-complete latency | Send `true`, measure time to wait-complete return — under 10ms | Fails if inotify latency exceeds design target (design doc: "sub-millisecond") |

#### test_nbs_ts_read.sh — Read operations

Derived from: `test_pty_session_last.sh`

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| R1 | read-new tracks offset | Send 3 commands, read-new after each — each returns only new output | Fails if offset tracking broken |
| R2 | read with --offset | `nbs-ts read <handle> --offset=0` returns full output | Fails if offset parameter ignored |
| R3 | read-new --strip removes ANSI | Command producing colour output, `--strip` result has no escape sequences | Fails if strip flag not wired through |
| R4 | read-new --strip preserves content | Stripped output contains the text without escape codes | Fails if strip removes too aggressively |
| R5 | read after kill | Kill session, read output.log — output still available | Fails if kill deletes log before read |
| R6 | read --last=N returns tail viewport | Send 100 lines, `nbs-ts read <handle> --last=5` — returns exactly 5 lines, and they are the final 5 | Fails if tail-read not implemented or returns wrong lines (sidecar transport depends on this) |
| R7 | read --last=N stable on repeated calls | Call `read --last=5` twice without sending — same content both times | Fails if viewport semantics broken (sidecar hash-based idle detection requires stability) |

#### test_nbs_ts_completion.sh — Completion signalling

NEW — no pty-session equivalent (this is entirely new functionality)

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| C1 | PROMPT_COMMAND injected | Create bash session, check `env` output contains NBS_TS_CMD_SEQ | Fails if injection mechanism broken |
| C2 | Sequential commands get sequential IDs | Send 3 commands, parse completion.log — sequence numbers 1, 2, 3 | Fails if sequence counter broken |
| C3 | Exit codes recorded per command | Send `true`, `false`, `exit 7` — completion.log has codes 0, 1, 7 | Fails if exit code capture broken |
| C4 | wait-complete for Nth command | Send 3 commands, wait-complete returns data for most recent | Fails if wait targets wrong sequence |
| C5 | Completion signal vs output race (small) | Send `echo BEFORE; sleep 0; echo AFTER`, wait-complete — output contains both BEFORE and AFTER | Fails if completion fires before output flushed |
| C6 | Completion signal vs output race (large) | Send command producing 10KB of output then exit, wait-complete, read-new — all 10KB present | Fails if completion.log write races ahead of output.log flush. This is the critical invariant: when wait-complete returns, ALL output from that command MUST already be in the log |
| C7 | Non-bash shell: wait-complete fallback | Create session with `sh` (not bash — no PROMPT_COMMAND), send command, wait-complete with short timeout — times out with exit 3 (does not hang forever) | Fails if missing completion signal causes infinite wait instead of timeout |

#### test_nbs_ts_status.sh — Status and liveness

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| S1 | Alive session | `nbs-ts status` reports alive for running bash | Fails if waitpid check wrong |
| S2 | Dead session | Session exited, status reports dead with exit code | Fails if child reaping broken |
| S3 | Killed session | After `nbs-ts kill`, status reports killed or session gone | Fails if SIGTERM not delivered |
| S4 | exit-code command | `nbs-ts exit-code <handle>` returns numeric code | Fails if exit code not persisted |
| S5 | Heartbeat mtime | Heartbeat file mtime < 5 seconds old for alive session | Fails if heartbeat thread dead |

#### test_nbs_ts_adversarial.sh — Adversarial inputs

Derived from: `test_pty_session_adversarial.sh`, `test_pty_session_adv_invalid.sh`

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| A1 | Path traversal in handle | `nbs-ts create '../../etc/passwd' 'bash'` — rejected or sanitised, no file created outside session dir | Fails if handle not validated |
| A2 | Slash in handle | `nbs-ts create 'test/slash' 'bash'` — rejected | Fails if slash passes validation |
| A3 | Empty handle | `nbs-ts create '' 'bash'` — exit code 4 | Fails if empty string not caught |
| A4 | Very long handle | 256-char handle — rejected with clear error | Fails if buffer overflow or silent truncation |
| A5 | Send to nonexistent session | Exit code 2, no crash | Fails if missing session not checked |
| A6 | Read nonexistent session | Exit code 2 | Same |
| A7 | Kill nonexistent session | Exit code 2 | Same |
| A8 | Wait on nonexistent session | Exit code 2 | Same |
| A9 | Unknown command | `nbs-ts bogus` — exit code 4 | Fails if command dispatch falls through |
| A10 | Create with missing args | `nbs-ts create` — exit code 4, usage message | Fails if arg count not checked |
| A11 | Unknown option | `nbs-ts read <h> --bogus=5` — exit code 4 | Fails if unknown options silently ignored |
| A12 | Large output (1MB+) | Create `head -c 1048576 /dev/urandom | base64`, kill, read — output size > 0 | Fails if output.log truncated or thread dies on large write |
| A13 | 50 concurrent sessions | Create 50, verify all alive, kill all, verify all cleaned up | Fails if resource exhaustion or zombie leak |
| A14 | Session handle collision | Create "test-x", kill, create "test-x" again — works, no stale state | Fails if old session dir not cleaned |

#### test_nbs_ts_cleanup.sh — Process death and cleanup

NEW — pty-session tests lack this coverage

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| CL1 | Kill cleans session directory | After kill, `~/.nbs-ts/sessions/<handle>/` does not exist | Fails if cleanup skips directory removal |
| CL2 | Output log survives kill for read | Read output after kill — data available | Fails if log deleted before consumer reads (NOTE: design says cleanup removes dir, so this tests the read-after-kill grace period or caching) |
| CL3 | No zombie after supervisor process killed | Kill the per-session supervisor process (not the CLI caller — the caller exits immediately after create). Session's shell is dead, no zombie in `ps`, session directory cleaned up | Fails if supervisor-exit cleanup not implemented |
| CL4 | No zombie after SIGKILL to supervisor | Same as CL3 but supervisor is SIGKILL'd — session cleaned up eventually | Fails if cleanup relies only on graceful SIGTERM handling |
| CL5 | Concurrent kill is safe | Two processes call kill on same session — both return without crash | Fails if cleanup has TOCTOU race |
| CL6 | CLI caller exit does not kill session | `nbs-ts create` in a subshell, subshell exits, session still alive (supervisor persists) | Fails if session is owned by caller instead of supervisor |

---

## 3. Phase 2: Sidecar Transport Tests

#### test_nbs_ts_transport.sh

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| T1 | Sidecar starts with `--transport=ts` | Sidecar process alive, no errors in log | Fails if transport_ts.c vtable registration broken |
| T2 | `read_content()` returns agent output | Sidecar reads agent pane via nbs-ts — returns text | Fails if `nbs_ts_read_new()` integration broken |
| T3 | `send_text()` delivers to agent | Sidecar sends skill injection, agent receives | Fails if `nbs_ts_send()` path broken |
| T4 | `is_alive()` detects live agent | Sidecar reports agent alive | Fails if `nbs_ts_status()` mapping wrong |
| T5 | `is_alive()` detects dead agent | Kill agent, sidecar detects death | Same |
| T6 | No tmux commands in sidecar log | `grep -c tmux sidecar.log` == 0 when using ts transport | Fails if tmux fallback leaks through |

---

## 4. Phase 3: Agent Launcher + Worker Tests

#### test_nbs_ts_worker.sh

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| WK1 | Worker spawns via nbs-ts | `nbs-spawn-worker` with NBS_TRANSPORT=ts creates nbs-ts session | Fails if spawn script not updated |
| WK2 | Worker output logged automatically | Worker produces output, readable via `nbs-ts read-new` | Fails if no pipe-pane equivalent needed (output capture is built-in) |
| WK3 | Worker completion detected | Worker finishes, `nbs-ts wait-complete` returns | Fails if completion signalling doesn't work through spawn wrapper |
| WK4 | Worker kill via nbs-ts | `nbs-ts kill <worker-handle>` terminates worker | Same as core kill |

---

## 5. Phase 4-5: System Tests (deferred until implementation reaches these phases)

These test multi-component integration and are specified here as placeholders.

- `test_nbs_ts_system_remote.sh` — nbs-remote-run via nbs-ts (SSH through local PTY)
- `test_nbs_ts_system_restart.sh` — /restart kills and relaunches agents via nbs-ts
- `test_nbs_ts_system_watchdog.sh` — watchdog detects team death via nbs-ts

---

## 6. Phase 6: Removal Verification

#### test_nbs_ts_no_tmux.sh

| # | Test | Assertion | Falsification |
|---|------|-----------|---------------|
| NT1 | No tmux references in source | `grep -r 'tmux\|pty-session\|capture-pane\|send-keys' src/ bin/` — zero matches | Fails if removal incomplete |
| NT2 | Full test suite passes without tmux | Unset PATH to tmux, run suite — all pass | Fails if hidden tmux dependency remains |
| NT3 | Team operates without tmux | 6 agents run for 10 minutes, all respond to chat | Fails if tmux required for any code path |

---

## 7. Benchmarks

All benchmarks use **ABBA interleaving** — not sequential sweeps. Each measurement alternates baseline (pty-session/tmux) and candidate (nbs-ts) to cancel thermal drift and system load variation.

| # | Benchmark | Metric | Threshold | Method |
|---|-----------|--------|-----------|--------|
| B1 | Session create latency | Time from CLI invocation to first read-new returning | nbs-ts <= tmux | 100 iterations, ABBA, measure wall clock |
| B2 | Send-to-output latency | Time from send to read-new containing marker | nbs-ts < 10ms | 100 iterations, ABBA |
| B3 | wait-complete latency | Time from command completion to wait-complete return | nbs-ts < 10ms (design target) | 100 iterations, ABBA |
| B4 | wait-pattern latency | Time from pattern appearing in output to wait-pattern return | nbs-ts < 10ms | 100 iterations, ABBA |
| B5 | Large output throughput | Time to capture 10MB of output | nbs-ts >= tmux (no data loss) | 10 iterations, ABBA, verify byte count |
| B6 | 50-session concurrent create/kill | Total time to create and destroy 50 sessions | nbs-ts <= tmux | 5 iterations, ABBA |

Benchmark reports include: machine, OS, kernel, CPU governor, per-iteration timings (not just aggregates), and baseline drift analysis.

---

## 8. Dynamic Analysis

Required for all C code in `src/nbs-ts/`. Non-negotiable per engineering standards.

| Tool | What it catches | Command |
|------|----------------|---------|
| ASan | Heap overflow, use-after-free, double-free, stack overflow, leaks | `CFLAGS="-fsanitize=address" make test` |
| UBSan | Signed overflow, null deref, alignment, shift | `CFLAGS="-fsanitize=undefined" make test` |
| Valgrind | Memory leaks (complementary to ASan leak detection) | `valgrind --leak-check=full --error-exitcode=1 ./test_nbs_ts` |

The Makefile must have a `test-sanitize` target that compiles and runs with ASan+UBSan. CI runs this target.

---

## 9. Coverage Gap Tracking

After each implementation phase, testkeeper runs the test suite and reports gaps:

```
TESTKEEPER REPORT — Phase N

**C unit tests:** PASS N/N | FAIL — details
**CLI integration tests:** PASS N/N | FAIL — details
**Benchmarks:** baseline +/- margin | REGRESSION — details

**Coverage gaps:** list of untested paths with file:line
**Methodology notes:** any concerns about test validity
```

---

## 10. Relationship to Existing pty-session Tests

| Existing test | nbs-ts equivalent | Notes |
|--------------|-------------------|-------|
| test_pty_session_create.sh | test_nbs_ts_lifecycle.sh + test_nbs_ts_oneshot.sh | Split into lifecycle and one-shot |
| test_pty_session_lifecycle.sh | test_nbs_ts_lifecycle.sh | Direct mapping |
| test_pty_session_wait.sh | test_nbs_ts_wait.sh | Expanded with wait-complete |
| test_pty_session_timeout.sh | test_nbs_ts_wait.sh W3, W5 | Merged into wait tests |
| test_pty_session_fence.sh | **ELIMINATED** | Fence markers replaced by completion signalling |
| test_pty_session_last.sh | test_nbs_ts_read.sh R6, R7 | --last viewport semantics preserved for sidecar |
| test_pty_session_lock.sh | **ELIMINATED** | No shared sessions, no advisory locking |
| test_pty_session_improvements.sh | test_nbs_ts_status.sh + test_nbs_ts_read.sh R5 | Status display + read-after-kill |
| test_pty_session_adversarial.sh | test_nbs_ts_adversarial.sh | Direct mapping, expanded |
| test_pty_session_adv_invalid.sh | test_nbs_ts_adversarial.sh A5-A11 | Merged into adversarial |
| test_pty_session_adv_no_collision.sh | **ELIMINATED** | No tmux, no collision possible |
| test_pty_session_wait_fallback.sh | test_nbs_ts_wait.sh W6, W7 | Log fallback on dead session |

**Existing tests eliminated:** 3 (fence, lock, collision) — the mechanisms they test do not exist in nbs-ts.
**New test areas:** 3 (completion signalling, cleanup/process death, ANSI stripping) — functionality that pty-session lacks.

---

## 11. Falsification of This Plan

This test plan is wrong if:
- A test exists that cannot fail (decoration)
- A code path in nbs-ts has no test exercising it
- A benchmark uses sequential sweeps instead of ABBA
- Dynamic analysis is not run on every build
- An nbs-ts failure mode matches one of the 15 documented tmux incidents and no test covers it
