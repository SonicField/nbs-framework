# Valgrind Static Audit Report

**Date:** 2026-04-02
**Auditor:** generalist (automated)
**Scope:** nbs-sidecar, nbs-ts-helper — long-lived components only

---

## ResourceSite Table — nbs-sidecar

### sidecar.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| sidecar.c | 49 | FileDescriptor | `fopen` (g_sc_debug_fp) | *never closed* | N/A | once at init |
| sidecar.c | 157,740,876,1029 | HeapAlloc | `tp->capture()` → malloc | `free()` | YES | every tick |
| sidecar.c | 281 | HeapAlloc | `malloc` (scan_buf) | `free` (294) | YES | on query |
| sidecar.c | 334 | PipeOpen | `popen` (render_cmd) | `pclose` (348) | YES | on query |
| sidecar.c | 352 | HeapAlloc | `tp->capture()` fallback | `free` (362) | YES | on query (fallback) |
| sidecar.c | 269 | FileDescriptor | `open` (log_fd) | `close` (299) | YES | on query |
| sidecar.c | 966 | ProcessFork | `exec_fire_and_forget` | waitpid (sync) | YES | every 10s |
| sidecar.c | 1093 | ProcessFork | `exec_fire_and_forget` | waitpid (sync) | YES | on notification |

### transport_ts.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| transport_ts.c | 47 | FileDescriptor | `fopen` (g_ts_debug_fp) | *never closed* | N/A | once at init |
| transport_ts.c | 421 | HeapAlloc | `calloc` (ts_ctx_t) | `free` via transport_free | YES | once at init |
| transport_ts.c | 154 | HeapAlloc | `malloc` (capture buf) | returned to caller | YES | every tick |
| transport_ts.c | 143,167 | HeapAlloc | `calloc(1,1)` (empty string) | returned to caller | YES | on empty file |
| transport_ts.c | 227 | FileDescriptor | `open` (input_fifo) | `close` (241,246,261,271,285) | YES | on send_text |
| transport_ts.c | 317 | FileDescriptor | `open` (input_fifo) | `close` (328,334,339) | YES | on send_key |
| transport_ts.c | 98 | FileDescriptor | `open` (pid file) | `close` (103) | YES | on is_alive |

### exec_util.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| exec_util.c | 86 | FileDescriptor | `pipe2` (pipefd[0,1]) | `close` (92,99,104,113,148) | YES | on exec_capture |
| exec_util.c | 90 | ProcessFork | `fork` | `waitpid` (153-155) | YES | on exec_capture |
| exec_util.c | 67 | FileDescriptor | `open` (/dev/null) | `close` via dup2 (69-71) | YES | child only |
| exec_util.c | 187 | ProcessFork | `fork` | `waitpid` (214-216) | YES | on fire_and_forget |
| exec_util.c | 194 | FileDescriptor | `open` (/dev/null) | `close` via dup2 (196-200) | YES | child only |

### bus_client.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| bus_client.c | — | — | All via `exec_capture`/`exec_fire_and_forget` | — | YES | per check |

No direct resource acquisition. All subprocess execution delegated to exec_util.

### chat_client.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| chat_client.c | 88 | FileDescriptor | `fopen` (chat file) | `fclose` (114) | YES | per count_messages |
| chat_client.c | 91 | HeapAlloc | `getline` (line buf) | `free` (113) | YES | per count_messages |
| chat_client.c | 136 | FileDescriptor | `fopen` (cursor file) | `fclose` (182) | YES | per read_cursor |
| chat_client.c | 438 | FileDescriptor | `fopen` (chat file) | `fclose` (526,533,539) | YES | per sidecar_only_cb |
| chat_client.c | 444 | HeapAlloc | `getline` (line buf) | `free` (525,532,538) | YES | per sidecar_only_cb |
| chat_client.c | 492 | HeapAlloc | `malloc` (decoded msg) | `free` (501,516) | YES | per unread msg |

### registry.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| registry.c | 51 | FileDescriptor | `fopen` (registry) | `fclose` (53,63,68) | YES | per contains check |
| registry.c | 83 | FileDescriptor | `fopen` (registry) | `fclose` (89,91) | YES | per append |
| registry.c | 107 | FileDescriptor | `fopen` (registry) | `fclose` (119,149) | YES | per remove |
| registry.c | 117 | FileDescriptor | `mkstemp` (tmp) | `fclose` (via fdopen) | YES | per remove |
| registry.c | 355 | FileDescriptor | `opendir` | `closedir` (390,395) | YES | per seed |
| registry.c | 434 | FileDescriptor | `fopen` (inbox) | `fclose` (439,441,448,451,465) | YES | per inbox process |
| registry.c | 458 | HeapAlloc | `malloc` (inbox buf) | `free` (471,498,512) | YES | per inbox process |
| registry.c | 525 | FileDescriptor | `fopen` (registry) | `fclose` (550,554,556) | YES | per find_first |
| registry.c | 573 | FileDescriptor | `fopen` (registry) | `fclose` (629,635) | YES | per for_each |

### triggers.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| triggers.c | 146 | FileDescriptor | `fopen` (timestamp) | `fclose` (157) | YES | per trigger check |
| triggers.c | 179 | FileDescriptor | `mkstemp` (tmp) | `fclose` (via fdopen 181) | YES | per timestamp write |
| triggers.c | 265 | FileDescriptor | `open` (lock file) | `close` (281,296,324) | YES | per spawn |
| triggers.c | 311 | ProcessFork | `exec_fire_and_forget` | waitpid (sync) | YES | per trigger spawn |

### detect.c, hash.c, strip_ansi.c, mention_escape.c

No resource acquisition. Pure computation/string manipulation.

`escape_mentions` (mention_escape.c:86) allocates and returns a heap buffer, but this function is NOT called from the sidecar main loop (only `sanitise_at_signs` is used, which operates in-place).

### main.c (sidecar)

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| main.c | 316 | FileDescriptor | `freopen` (stderr → log) | process lifetime | YES | once at init |
| main.c | 341 | HeapAlloc | `transport_ts_init` → calloc | `transport_free` (355) | YES | once at init/shutdown |

---

## ResourceSite Table — nbs-ts-helper

### helper.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| helper.c | 254 | FileDescriptor | `socket` (server) | `close` (325) | YES | once at init |
| helper.c | 314 | SocketAccept | `accept` (client_fd) | `close` (125,132,144,152,167,176,219) | YES | per connection |
| helper.c | 163 | FileDescriptor | `openpty` (master_fd, slave_fd) | `close` (175-176 err, 183 child, 204+218 parent) | YES | per connection |
| helper.c | 171 | ProcessFork | `fork` | `reap_children`→waitpid (101) | YES | per connection |

### session.c (library — used by nbs-ts CLI, not helper directly)

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| session.c | 206 | HeapAlloc | `calloc` (session) | `free` (215,222,229,271,305,316,414) | YES | per create |
| session.c | 96 | FileDescriptor | `open` (/dev/urandom) | `close` (100) | YES | per create |
| session.c | 237 | FileDescriptor | `open` (output.log) | `close` (241) | YES | per create |
| session.c | 243 | FileDescriptor | `open` (completion.log) | `close` (247) | YES | per create |
| session.c | 172 | FileDescriptor | `open` (output.log capture) | `close` (196) | YES | capture thread |
| session.c | 364 | ProcessFork | `pthread_create` | `pthread_join` (399) | YES | per create/destroy |
| session.c | 309 | ProcessFork | `fork` (fallback) | `waitpid` in destroy (382-389) | YES | per create/destroy |

### io.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| io.c | 54 | FileDescriptor | `open` (output.log) | `close` (62) | YES | per read_new |
| io.c | 76 | FileDescriptor | `open` (output.log) | `close` (83) | YES | per read |
| io.c | 103 | FileDescriptor | `open` (output.log) | `close` (113,121,137) | YES | per read_tail |
| io.c | 128 | HeapAlloc | `malloc` (tmp buf) | `free` (141,162) | YES | per read_tail |

### wait.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| wait.c | 50 | FileDescriptor | `inotify_init1` | `close` (116) | YES | per wait_complete |
| wait.c | 57 | FileDescriptor | `inotify_add_watch` | `inotify_rm_watch` (115) | YES | per wait_complete |
| wait.c | 76 | FileDescriptor | `fopen` (completion) | `fclose` (91) | YES | per poll cycle |
| wait.c | 130 | FileDescriptor | `inotify_init1` | `close` (207) | YES | per wait_pattern |
| wait.c | 137 | FileDescriptor | `inotify_add_watch` | `inotify_rm_watch` (206) | YES | per wait_pattern |
| wait.c | 158 | FileDescriptor | `open` (output.log) | `close` (182) | YES | per poll cycle |
| wait.c | 165 | HeapAlloc | `malloc` (search buf) | `free` (179) | YES | per poll cycle |

### helper_client.c

| File | Line | Kind | Acquire | Release | All Paths | Frequency |
|------|------|------|---------|---------|-----------|-----------|
| helper_client.c | 34 | FileDescriptor | `socket` | `close` (46,53,89,117) | YES | per PTY request |

---

## LeakCandidate Table

| # | File | Line | Kind | Description | Severity |
|---|------|------|------|-------------|----------|
| L1 | sidecar.c | 49 | FileDescriptor | `g_sc_debug_fp`: debug log FILE opened on first `sc_dbg()` call, never closed. One FILE struct + one fd leak at process exit. Not per-tick — allocated once. | low |
| L2 | transport_ts.c | 47 | FileDescriptor | `g_ts_debug_fp`: debug log FILE opened on first `ts_dbg()` call, never closed. Same pattern as L1. One FILE struct + one fd leak at process exit. | low |

---

## Analysis Summary

### nbs-sidecar

The sidecar main loop (`sidecar_run` in sidecar.c) is clean. Every `tp->capture()` allocation is freed on all code paths — I traced 9 distinct exit points from the main loop body and verified each one frees `content`. The TOCTOU re-capture (`fresh`) at line 1029 is separately freed on both its branches.

All `exec_capture` and `exec_fire_and_forget` calls properly manage pipe fds, child processes, and waitpid. The H1 fix (pipe drain before close) prevents deadlock when child output exceeds the capture buffer.

All file I/O in registry.c, chat_client.c, and bus_client.c follows open-use-close patterns with cleanup on all error paths. `getline()` buffers are freed after loop exit. Base64-decoded message buffers in `sidecar_only_cb` are freed on all paths including early returns.

The `popen`/`pclose` pair in `handle_query` (line 334/348) is clean — the child process is reaped by `pclose`.

**No per-tick heap, fd, or process leaks found.**

### nbs-ts-helper

The helper's main loop is clean. Each `accept()`ed client_fd is closed on all paths through `handle_client()`. `openpty()`-created master_fd and slave_fd are properly distributed (child gets slave, parent gets master, both closed). Fork'd children are reaped by `reap_children()` via `waitpid(WNOHANG)` on each loop iteration — zombies exist only transiently between iterations.

The server socket and its cleanup path (close + unlink) are correct.

**No per-tick heap, fd, or process leaks found.**

### Leak Candidates Detail

**L1 & L2 — Debug log FILEs (low severity)**

Both `g_sc_debug_fp` (sidecar.c:49) and `g_ts_debug_fp` (transport_ts.c:47) are FILE pointers opened once on first use and never closed. At process exit, valgrind will report these as "still reachable" (not "definitely lost"). Each contributes one FILE struct (~200 bytes on glibc) and one file descriptor.

These are functionally harmless — the OS reclaims both on exit — but they will pollute valgrind output. The fix is trivial: close them before `sidecar_run` returns.

**Fix location:**
- L1: Close `g_sc_debug_fp` at end of `sidecar_run()` (sidecar.c, before line 1114)
- L2: Close `g_ts_debug_fp` in `transport_free()` (transport_ts.c, around line 481)

### False Positive Expectations

Valgrind will likely report the following as "still reachable" which are NOT leaks:
- `pthread_once` control blocks in bus_client.c and chat_client.c (glibc internal state)
- Thread-local storage from `localtime_r` (glibc allocates TLS lazily)
- Any glibc/libc startup allocations

These should be suppressed in the valgrind suppressions file.
