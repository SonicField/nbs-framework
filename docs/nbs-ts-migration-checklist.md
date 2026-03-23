# nbs-ts Migration Checklist

**Author:** testkeeper
**Date:** 2026-03-23
**Purpose:** Every file in the codebase that references tmux or pty-session, categorised by migration phase.

## Summary

| Category | Files | tmux refs | pty-session refs |
|----------|-------|-----------|------------------|
| src/ C code | 10 | 89 | 12 |
| bin/ scripts | 12 | 68 | 52 |
| tests/ | 39 | varies | varies |
| docs/ | 3 | varies | varies |
| **Total** | **64** | | |

---

## Phase 1: Core Library (no migration — new code)

No existing files need changes. `src/nbs-ts/` is built from scratch.

---

## Phase 2: Sidecar Transport

### Must change

| File | Lines | What to do |
|------|-------|------------|
| `src/nbs-sidecar/transport.h` | 2, 5, 64, 74 | Add `transport_ts_init()` declaration alongside existing tmux/pty |
| `src/nbs-sidecar/main.c` | 8, 56, 62-63, 178, 184, 283, 325, 366 | Add `--transport=ts` option, init transport_ts when selected |
| `src/nbs-sidecar/sidecar.c` | 13, 71, 243, 862 | Comments only — update to mention nbs-ts alongside tmux |
| `src/nbs-sidecar/sidecar.h` | 4 | Comment update |
| `src/nbs-sidecar/detect.c` | 191 | Comment about tmux trailing newlines — verify nbs-ts equivalent |
| `src/nbs-sidecar/strip_ansi.h` | 4 | Comment update — strip_ansi now used by nbs-ts --strip too |

### New file

| File | Purpose |
|------|---------|
| `src/nbs-sidecar/transport_ts.c` | nbs-ts transport vtable implementation |

### Tests affected

| Test file | Impact |
|-----------|--------|
| `tests/test_sidecar_transport_unit.c` | Add transport_ts unit tests |
| `tests/automated/test_sidecar_lifecycle.sh` | Add `--transport=ts` variant |

---

## Phase 3: Agent Launcher + Workers

### Must change

| File | Lines | What to do |
|------|-------|------------|
| `bin/nbs-claude` | 16-17, 72-87, 125, 239, 277, 312-424 | Add `NBS_TRANSPORT=ts` code path: nbs-ts create instead of tmux/pty-session, nbs-ts attach instead of tmux attach |
| `bin/nbs-spawn-worker` | 5, 105, 109, 122, 132, 140 | Replace tmux new-session/pipe-pane/has-session/kill-session with nbs-ts equivalents |
| `src/nbs-workers/worker.c` | 7, 169, 455-505, 995-1007, 1064, 1215-1302, 1318-1327, 1428-1429, 1438, 1478, 1513-1544, 1842-1851, 1919, 1966-2072 | Replace tmux_has_session/kill_session/send_keys/capture_pane/pipe_pane helpers with nbs-ts API calls. ~120 lines of tmux helper code to replace |
| `src/nbs-chat/terminal.c` | 181, 236, 256 | Session creation uses tmux — replace with nbs-ts |
| `src/nbs-chat/watchdog.h` | 4 | Update description from "tmux sessions" to "nbs-ts sessions" |
| `src/nbs-sidecar/triggers.c` | 323 | Comment about tmux new-session pattern |

### Tests affected

| Test file | Impact |
|-----------|--------|
| `tests/automated/test_nbs_worker_lifecycle.sh` | Update to verify nbs-ts sessions instead of tmux |
| `tests/automated/test_nbs_worker_search.sh` | Same |
| `tests/automated/test_worker_spawn_survival.sh` | Same |
| `tests/automated/test_worker_death_logging.sh` | Same |
| `tests/automated/test_ephemeral_worker_exit.sh` | Same |
| `tests/automated/test_supervisor_nbs_worker.sh` | Update criteria to expect nbs-ts commands |
| `tests/automated/test_supervisor_adv_no_old_pattern.sh` | Expand to also reject tmux patterns |
| `tests/automated/test_help_nbs_worker.sh` | Update expected recommendations |
| `tests/automated/test_install_worker.sh` | Update expected binary checks |
| `tests/automated/test_nbs_claude_audit_v17.sh` | References tmux transport |
| `tests/automated/test_nbs_claude_fixes.sh` | References pty-session mode |
| `tests/automated/test_nbs_claude_bus.sh` | References sidecar transport |
| `tests/automated/test_resume_semantics.sh` | References tmux_session in metadata |
| `tests/test_workers_adversarial.c` | C unit test for worker.c — tmux helpers |

---

## Phase 4: Remote Tools

### Must change

| File | Lines | What to do |
|------|-------|------------|
| `bin/nbs-remote-run` | 4, 29, 34, 77, 79, 97 | Replace pty-session create/send/wait/read/kill with nbs-ts |
| `bin/nbs-remote-session` | 4-5, 17-19, 28, 33, 67-70 | Replace pty-session with nbs-ts |
| `bin/nbs-remote-build` | 2, 5, 45, 48-50, 54, 153, 186 | Replace pty-session with nbs-ts |
| `bin/nbs-remote-diff` | 2, 4, 18, 42, 45-48, 135, 188 | Replace pty-session with nbs-ts |
| `bin/nbs-remote-status` | 2, 5, 36, 39-42, 108, 148 | Replace pty-session with nbs-ts |
| `bin/nbs-remote-edit` | 10-11, 30, 43, 65, 81-118, 129, 155, 178 | Replace pty-session with nbs-ts |
| `bin/nbs-remote-read` | 14-15, 26, 47, 49, 57-91, 103 | Replace pty-session with nbs-ts |

### Tests affected

| Test file | Impact |
|-----------|--------|
| `tests/automated/test_nbs_remote_build.sh` | Update to use nbs-ts session names |
| `tests/automated/test_nbs_remote_diff_status.sh` | Same |
| `tests/automated/test_nbs_remote_edit_static.sh` | Same |
| `tests/automated/test_nbs_remote_read.sh` | Same |
| `tests/automated/test_nbs_remote_session_run.sh` | Same |
| `tests/automated/test_claude_remote_audit_v2.sh` | References pty-session patterns |
| `tests/automated/test_claude_remote_fixes.sh` | Same |

---

## Phase 5: Restart, Watchdog, Cleanup

### Must change

| File | Lines | What to do |
|------|-------|------------|
| `bin/nbs-chat-terminal-restart.sh` | 59, 63, 115, 122-127 | Replace tmux kill-session/new-session/send-keys/display-message with nbs-ts kill/create/send |
| `bin/nbs-chat-init` | 5, 16-17, 75-88, 176, 243-244, 292, 324, 334, 734-976 | Replace tmux session management with nbs-ts. ~250 lines of tmux interaction |
| `bin/nbs-sidecar-restart` | 12, 122-168 | Replace tmux list-sessions/display-message with nbs-ts list/status |
| `src/nbs-common/trigger_defs.h` | 13, 41 | Update tmux references in trigger text |

### Tests affected

| Test file | Impact |
|-----------|--------|
| `tests/automated/test_chat_init_fixes.sh` | References tmux session checks |
| `tests/automated/test_nbs_chat_terminal.sh` | References terminal restart with tmux |
| `tests/automated/test_sidecar_restart_fixes.sh` | References tmux session scanning |
| `tests/automated/test_watchdog_chat_scope.sh` | References tmux session counting |

---

## Phase 6: Remove tmux

### Delete entirely

| File | Reason |
|------|--------|
| `src/nbs-pty-session/` (entire directory) | Replaced by src/nbs-ts/ |
| `src/nbs-sidecar/transport_tmux.c` | Replaced by transport_ts.c |
| `bin/pty-session` (binary) | Replaced by nbs-ts |
| `bin/pty-session-lock` | No shared sessions in nbs-ts |
| `docs/pty-session.md` | Replaced by nbs-ts docs |

### Delete test files

| Test file | Reason |
|-----------|--------|
| `tests/automated/test_pty_session_create.sh` | Replaced by test_nbs_ts_lifecycle.sh + test_nbs_ts_oneshot.sh |
| `tests/automated/test_pty_session_lifecycle.sh` | Replaced by test_nbs_ts_lifecycle.sh |
| `tests/automated/test_pty_session_wait.sh` | Replaced by test_nbs_ts_wait.sh |
| `tests/automated/test_pty_session_timeout.sh` | Merged into test_nbs_ts_wait.sh |
| `tests/automated/test_pty_session_fence.sh` | Eliminated — fence markers don't exist |
| `tests/automated/test_pty_session_last.sh` | Replaced by test_nbs_ts_read.sh |
| `tests/automated/test_pty_session_lock.sh` | Eliminated — no advisory locking |
| `tests/automated/test_pty_session_improvements.sh` | Replaced by test_nbs_ts_status.sh |
| `tests/automated/test_pty_session_adversarial.sh` | Replaced by test_nbs_ts_adversarial.sh |
| `tests/automated/test_pty_session_adv_invalid.sh` | Merged into test_nbs_ts_adversarial.sh |
| `tests/automated/test_pty_session_adv_no_collision.sh` | Eliminated — no tmux collision possible |
| `tests/automated/test_pty_session_wait_fallback.sh` | Merged into test_nbs_ts_wait.sh |

### Update references

| File | What to do |
|------|------------|
| `docs/tools.md` | Replace pty-session references with nbs-ts |
| `tests/README.md` | Update test descriptions |
| `tests/run_all.sh` | Update test list |

### Verification gate

```bash
# This command must return zero matches after Phase 6
grep -rn 'tmux\|pty-session\|capture-pane\|send-keys\|pipe-pane\|has-session\|kill-session\|new-session' \
    src/ bin/ tests/ docs/ \
    --include='*.c' --include='*.h' --include='*.sh' --include='*.md' --include='*.py' \
    | grep -v 'nbs-ts-migration-checklist.md' \
    | grep -v 'nbs-ts-test-plan.md' \
    | grep -v 'verdicts/'
```

If any matches remain, migration is incomplete.

---

## Other affected files (non-code)

| File | Type | Impact |
|------|------|--------|
| `tests/automated/test_investigation_ask.sh` | Test | References pty-session in test scenario |
| `tests/automated/test_investigation_adv_no_silent.sh` | Test | Same |
| `tests/automated/test_librarian.sh` | Test | References tmux in test prompts |
| `tests/automated/test_mention_query.sh` | Test | References tmux pane capture |
| `tests/automated/test_run_all_target.sh` | Test | References pty-session Makefile targets |
| `tests/test_sidecar_detect_unit.c` | Test | May reference tmux pane trailing newlines |
| `tests/test_sidecar_strip_ansi_unit.c` | Test | Comment about tmux pane captures |

---

## Risk: files that reference tmux only in comments

Several files reference tmux only in comments or documentation strings. These are not functional dependencies but must be updated for consistency:

- `src/nbs-sidecar/strip_ansi.c:90` — comment about tmux applications
- `src/nbs-sidecar/strip_ansi.h:4` — comment about tmux pane captures
- `src/nbs-common/trigger_defs.h:13` — comment about tmux Enter key
- `src/nbs-sidecar/detect.c:191` — comment about tmux trailing newlines

These can be updated as a batch during Phase 6.
