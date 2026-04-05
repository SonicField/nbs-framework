# GDB Debugging Reference

**These techniques work on ANY C binary.** The examples below happen to use the NBS sidecar as the target, but every technique — attaching, breakpoints, memory inspection, reverse debugging, interactive stepping — applies identically to any C program you are debugging. The sidecar is the worked example, not the subject.

If you are debugging a JIT compiler, a parser, a test harness, or any other C code — this document is for you. Substitute your binary, your function names, your struct types. The GDB commands are the same.

Every session below was captured against real NBS binaries built with `-g -O0`. No hypothetical or reconstructed output.

**When to use this:** Before adding a `printf` statement to any C code. GDB shows you the full state of a running process — stack, locals, globals, memory — without recompiling or restarting. For a single variable check, `printf` may be faster. For anything requiring more than one observation — use GDB. The session persists; `printf` does not.

**If you read nothing else, read Examples 11 and 12.**

---

## Prerequisites

### GDB Version

Use `/usr/bin/gdb` (GDB 16.3), not `/usr/local/bin/gdb` (GDB 9.1). The older version lacks reverse debugging support and has known bugs with ASAN binaries.

### Build Debug Binaries

Debug symbols are stripped from installed binaries. Build locally with debug targets:

```bash
cd ~/claude_docs/nbs-framework
make -C src/nbs-sidecar debug
make -C src/nbs-chat debug
make -C src/nbs-ts debug
```

The sidecar's `-O0` build may trigger `-Wformat-truncation` with GCC. Add `-Wno-format-truncation`:

```bash
make -C src/nbs-sidecar CFLAGS="-Wall -Wextra -Wshadow -Werror -std=c11 \
  -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O0 -g -Wno-format-truncation" clean all
```

Verify symbols exist:

```bash
$ file src/nbs-sidecar/nbs-sidecar
ELF 64-bit LSB executable, x86-64, ... with debug_info, not stripped
```

### Key Data Structures

Before debugging, know what you are looking at:

- **`sidecar_config_t`** (sidecar.h:49–74): Read-only config with timing constants, paths, handle.
- **`sidecar_state_t`** (sidecar.h:85–123): Mutable runtime state — idle counters, bus events, notification state, mention tracking.
- **`transport_t`** (transport.h): Function-pointer vtable for session capture/send.

Use `ptype` to inspect any struct definition:

```
(gdb) ptype sidecar_state_t
type = struct {
    int idle_seconds;
    int bus_check_counter;
    uint64_t last_content_hash;
    time_t sidecar_start_time;
    time_t last_notify_time;
    time_t last_flush_time;
    time_t last_poll_time;
    time_t last_fixup_check;
    time_t last_librarian_check;
    time_t last_pythia_check;
    time_t last_shepard_check;
    int notify_fail_count;
    int mention_detected;
    char mention_payload[256];
    int bus_event_count;
    char bus_max_priority[16];
    char bus_event_summary[256];
    int chat_unread_count;
    char chat_unread_summary[256];
    char notify_message[256];
    int control_inbox_line;
    int query_retry_count;
    int interrupt_retry_count;
    int mention_retry_count;
    int cooldown_suppressed;
    int startup_notify_sent;
    int was_paused;
}
```

---

## Category 1 — Attaching to a Running Process

### Worked Example 1 — Attach to a Running Sidecar and Inspect Its State

**Scenario:** A sidecar is running but notifications are not firing. You need to understand its internal state without restarting it.

**Setup:** Create a persistent GDB session with `nbs-local-session`, then use `nbs-ts send` / `nbs-ts read-new` to interact with GDB. This workflow persists across context compaction — your GDB breakpoints, watchpoints, and history survive.

```bash
# Create a test sidecar to debug
ALIVE_SESSION=$(nbs-ts list | grep alive | head -1 | awk '{print $1}')
mkdir -p /tmp/gdb-test-root/.nbs
~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar \
  --handle=gdb-test --root=/tmp/gdb-test-root --session=$ALIVE_SESSION \
  2>/tmp/gdb-sidecar.log &
SIDECAR_PID=$!

# Create a persistent local session and launch GDB inside it
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" \
  "/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar \
   -ex 'set sysroot /' -ex 'attach $SIDECAR_PID'"
sleep 3 && nbs-ts read-new "$GDB_HANDLE" --strip
```

**Session:** Each command is sent via `nbs-ts send`, output read via `nbs-ts read-new`:

```bash
$ nbs-ts send "$GDB_HANDLE" "bt"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
bt
#0  0x00007f81364d575a in clock_nanosleep@GLIBC_2.2.5 () from /lib64/libc.so.6
#1  0x00007f81364da247 in nanosleep () from /lib64/libc.so.6
#2  0x00007f81364da17e in sleep () from /lib64/libc.so.6
#3  0x0000000000405b0a in sidecar_run (cfg=0x7ffd2a28fe30, tp=0x7ffd2a28fe00)
    at sidecar.c:866
#4  0x000000000040365d in main (argc=4, argv=0x7ffd2a294448) at main.c:349
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "frame 3"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
frame 3
#3  0x0000000000405b0a in sidecar_run (cfg=0x7ffd2a28fe30, tp=0x7ffd2a28fe00)
    at sidecar.c:866
866         sleep(1);
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "print state.idle_seconds"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
print state.idle_seconds
$1 = 7
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "print state.startup_notify_sent"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
print state.startup_notify_sent
$2 = 0
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "print state.last_notify_time"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
print state.last_notify_time
$3 = 0
(gdb)
```

**Finding:** `last_notify_time = 0` and `startup_notify_sent = 0` — the sidecar has never sent a notification. Combined with `idle_seconds = 7` (less than `startup_grace = 30`), the sidecar is still in its startup grace period. **This is why notifications are not firing** — the grace period suppresses all notifications for the first 30 seconds.

**Why GDB:** With `printf`, you would need to add logging to `should_inject_notify`, recompile, restart the sidecar, and wait for the condition. With `nbs-local-session` + GDB, you inspect the running process interactively — and the session persists across context compaction, so you can come back to it later.

---

### Worked Example 2 — Attach to `nbs-ts-helper` and Inspect File Descriptors

**Scenario:** Sessions are not spawning. You suspect the helper daemon has a socket or FD problem.

**Setup:** Attach to the running helper via `nbs-local-session` with the debug binary loaded for symbols:

```bash
HELPER_PID=$(pgrep -f 'nbs-ts-helper$' | head -1)

# Via nbs-local-session (persistent):
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" "/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-ts/nbs-ts-helper -ex 'attach $HELPER_PID'"
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip

# Or directly for a quick inspection:
/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-ts/nbs-ts-helper \
  -ex "attach $HELPER_PID"
```

**Session:**

```
(gdb) bt
#0  0x00007f127e105859 in select () from /lib64/libc.so.6
#1  0x000000000040202d in main () at helper.c:311
311         int ready = select(srv + 1, &rfds, NULL, NULL, &tv);

(gdb) print g_sock_path
$1 = "/foo/bar/alexie/.nbs-ts/helper.sock", '\000' <repeats 67 times>

(gdb) print g_quit
$2 = 0

(gdb) shell ls -la /proc/1258773/fd/
total 0
lr-x------ 1 alexie users 64 Apr  5 07:01 0 -> /dev/null
l-wx------ 1 alexie users 64 Apr  5 07:01 1 -> /dev/null
l-wx------ 1 alexie users 64 Apr  5 07:01 2 -> /dev/null
lrwx------ 1 alexie users 64 Apr  5 07:01 3 -> socket:[56038890]

(gdb) info proc status
Name:   nbs-ts-helper
State:  t (tracing stop)
Pid:    1258773
VmRSS:      1764 kB
FDSize:	64
```

**Finding:** The helper has exactly 4 file descriptors: stdin/stdout/stderr (all /dev/null after daemonisation) and one socket (fd 3) — the Unix domain socket for accepting connections. FDSize is 64, confirming no FD leak. **`g_quit = 0`** confirms the helper is not in shutdown state — it is healthy and waiting for connections.

**Why GDB:** Checking FDs from outside requires `ls /proc/PID/fd` plus guesswork about which FD is which. GDB lets you correlate FD numbers with the source code (`srv` variable) and inspect global state (`g_quit`) simultaneously.

---

## Category 2 — Post-Mortem Analysis

### Worked Example 3 — Debug an Assert Failure (Crash) Under GDB

**Scenario:** A chat function crashes with an assert failure. You need to inspect the state at the crash point.

**Setup:** A caller passes `NULL` as the handle to `chat_cursor_write`. Run the program under GDB:

```bash
gdb -q /tmp/gdb-segfault-test
(gdb) run
```

**Session:**

```
ASSERT FAILED chat_file.c:1578: chat_cursor_write: handle is NULL

Program received signal SIGABRT, Aborted.
0x00007ffff7c8d03c in __pthread_kill_implementation () from /lib64/libc.so.6

(gdb) bt
#0  0x00007ffff7c8d03c in __pthread_kill_implementation () from /lib64/libc.so.6
#1  0x00007ffff7c3fb86 in raise () from /lib64/libc.so.6
#2  0x00007ffff7c29873 in abort () from /lib64/libc.so.6
#3  0x0000000000406ee6 in chat_cursor_write (chat_path=0x40903e "/tmp/test.chat",
    handle=0x0, index=5) at chat_file.c:1578
#4  0x00000000004013bc in main () at /tmp/gdb-segfault-test.c:12

(gdb) frame 3
#3  ... in chat_cursor_write (...) at chat_file.c:1578
1578        ASSERT_MSG(handle != NULL, "chat_cursor_write: handle is NULL");

(gdb) info args
chat_path = 0x40903e "/tmp/test.chat"
handle = 0x0
index = 5

(gdb) list
1573        return result;
1574    }
1575
1576    int chat_cursor_write(const char *chat_path, const char *handle, int index) {
1577        ASSERT_MSG(chat_path != NULL, "chat_cursor_write: chat_path is NULL");
1578        ASSERT_MSG(handle != NULL, "chat_cursor_write: handle is NULL");
1579        ASSERT_MSG(handle[0] != '\0', "chat_cursor_write: handle is empty");
1580        ASSERT_MSG(index >= 0, "chat_cursor_write: index is negative: %d", index);
```

**Finding:** The backtrace immediately shows: `handle=0x0` — the caller at `main:12` passed NULL. The assert message confirms which precondition was violated (`handle is NULL`). Frame 4 shows exactly which line in the caller triggered the crash.

**Why GDB:** The assert message says *what* failed but not *who called it or why*. GDB's `bt` shows the complete call chain. With `printf` debugging, you would need to add logging at every call site to find the caller.

---

### Worked Example 4 — Use-After-Free with ASAN + GDB

**Scenario:** A buffer is freed and then accessed. ASAN catches it; GDB confirms the root cause.

**Setup:** Build with ASAN (`make -C src/nbs-chat asan`) and run:

```bash
$ /tmp/gdb-uaf-test
```

**Session:**

```
=================================================================
==1244599==ERROR: AddressSanitizer: heap-use-after-free on address
  0x7b6fcbde0080 at pc 0x00000042a131 bp 0x7fff785fd270

READ of size 2 at 0x7b6fcbde0080 thread T0
    #0 0x42a130 in printf_common asan_interceptors.cpp.o
    #1 0x42bb37 in printf (/tmp/gdb-uaf-test+0x42bb37)
    #2 0x4eb281 in main /tmp/gdb-uaf-test.c:17

0x7b6fcbde0080 is located 0 bytes inside of 64-byte region
freed by thread T0 here:
    #0 0x4a694a in free (/tmp/gdb-uaf-test+0x4a694a)
    #1 0x4eb26c in main /tmp/gdb-uaf-test.c:14

previously allocated by thread T0 here:
    #0 0x4a6be8 in malloc (/tmp/gdb-uaf-test+0x4a6be8)
    #1 0x4eb230 in main /tmp/gdb-uaf-test.c:9

Shadow bytes around the buggy address:
=>0x7b6fcbde0080:[fd]fd fd fd fd fd fd fd fa fa fa fa fa fa fa fa
```

**Finding:** ASAN pinpoints the exact line where the bug occurs:
- **Line 17:** the read-after-free (the crash)
- **Line 14:** where the buffer was freed
- **Line 9:** where the buffer was originally allocated

The shadow byte `fd` confirms the memory region was freed. The allocator tracked the full lifecycle of the 64-byte heap region.

**Why GDB:** ASAN gives you the line numbers without GDB. The value of combining them is when the ASAN report points to a complex function with multiple allocations — GDB lets you set a breakpoint just before the free, inspect the buffer contents, and understand *why* the free happened when it did.

**Note:** GDB 9.1 has a known bug with ASAN-instrumented binaries (`sect_index_data not initialized`). Use `/usr/bin/gdb` (16.3) or run ASAN standalone first, then use GDB on the non-ASAN debug binary to investigate the root cause identified by ASAN.

---

## Category 3 — Breakpoint-Driven Investigation

### Worked Example 5 — Conditional Breakpoint in the Sidecar Main Loop

**Scenario:** Notifications are intermittent. You want to inspect the sidecar state only when it has been idle for more than 10 seconds — not on every 1-second tick.

**Setup:** Attach to a running debug sidecar via `nbs-local-session`:

```bash
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" "/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar -ex 'attach $SIDECAR_PID'"
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip
```

**Session:**

```
(gdb) break sidecar.c:1326 if state.idle_seconds > 10
Breakpoint 1 at 0x406b5b: file sidecar.c, line 1326.

(gdb) continue
Continuing.

Breakpoint 1, sidecar_run (cfg=0x7ffca26cb780, tp=0x7ffca26cb750)
    at sidecar.c:1326
1326            state.bus_check_counter++;

(gdb) print state.idle_seconds
$1 = 183

(gdb) print state.bus_check_counter
$2 = 2

(gdb) print state.sidecar_start_time
$3 = 1775397510

(gdb) print state.last_notify_time
$4 = 0

(gdb) print state.startup_notify_sent
$5 = 0

(gdb) print state.cooldown_suppressed
$6 = 0
```

**Finding:** The sidecar has been idle for **183 seconds** (over 3 minutes) but `last_notify_time = 0` — it has never notified. `startup_notify_sent = 0` despite being well past `startup_grace`. This means the startup catch-up notification path at sidecar.c:459–465 never executed, which happens when `detect_prompt_idle(content)` never returns true for this session's output.

**Why GDB:** A conditional breakpoint lets you skip hundreds of uninteresting iterations and stop only when the condition you care about is met. With `printf`, you would flood the log with every tick and then grep for the interesting ones.

---

### Worked Example 6 — Breakpoint on `chat_cursor_write()` — Trace Cursor Advancement

**Scenario:** An agent's read cursor is advancing past messages she has not read. You want to see exactly who is calling `chat_cursor_write` and with what values.

**Setup:** Run `nbs-chat read --unread` under GDB:

```bash
gdb -q ~/claude_docs/nbs-framework/src/nbs-chat/nbs-chat
(gdb) break chat_cursor_write
(gdb) run read /path/to/.nbs/chat/gdb.chat --unread=gdb-cursor-test
```

**Session:**

```
Breakpoint 1, chat_cursor_write (
    chat_path=0x7fffffffacf0 "/foo/bar/alexie/nbs-debug/.nbs/chat/gdb.chat",
    handle=0x7fffffffc58c "gdb-cursor-test3",
    index=14) at chat_file.c:1577
1577        ASSERT_MSG(chat_path != NULL, "chat_cursor_write: chat_path is NULL");

(gdb) bt
#0  chat_cursor_write (...) at chat_file.c:1577
#1  0x0000000000403906 in cmd_read (argc=4, argv=0x7fffffffbeb8) at main.c:420
#2  0x0000000000406af9 in main (argc=4, argv=0x7fffffffbeb8) at main.c:1352

(gdb) info args
chat_path = 0x7fffffffacf0 "/foo/bar/alexie/nbs-debug/.nbs/chat/gdb.chat"
handle = 0x7fffffffc58c "gdb-cursor-test3"
index = 14
```

**Finding:** `cmd_read` at main.c:420 calls `chat_cursor_write` with `index=14`, meaning it advances the cursor to message 14 (the end of the chat). The call originates from the `--unread` flag handler — every `nbs-chat read --unread=X` call writes the cursor forward after displaying messages. **This is the cursor advancement path** — if you see unexpected cursor jumps, break here and check who is calling.

**Why GDB:** The cursor write happens inside a locked region with atomic file replacement. Adding `printf` inside `chat_cursor_write` requires rebuilding the library and would fire on every cursor operation across all processes. GDB's breakpoint fires only in the process you are debugging.

---

## Category 4 — Memory and State Inspection

### Worked Example 7 — Print `sidecar_state_t` to Diagnose Notification Failures

**Scenario:** A sidecar is running but not delivering notifications. You need the complete picture of its internal state.

**Setup:** Attach to the running sidecar (same as Example 1).

**Session:**

```
(gdb) frame 3
(gdb) print *cfg
$1 = {
  handle = "gdb-test",
  nbs_root = "/tmp/gdb-test-root",
  session_name = "0a99c258",
  initial_prompt = "Your NBS handle is 'gdb-test'. ...",
  bus_check_interval = 3,
  notify_cooldown = 15,
  startup_grace = 30,
  notify_fail_threshold = 5,
  flush_interval = 60,
  transport_mode = TRANSPORT_TS
}

(gdb) print state.idle_seconds
$2 = 12

(gdb) print state.bus_check_counter
$3 = 0

(gdb) print state.notify_fail_count
$4 = 0

(gdb) print state.startup_notify_sent
$5 = 0

(gdb) print state.cooldown_suppressed
$6 = 0

(gdb) print state.mention_detected
$7 = 0

(gdb) print state.sidecar_start_time
$8 = 1775397184

(gdb) print state.last_notify_time
$9 = 0
```

**Finding:** A systematic reading of the state reveals:
1. `idle_seconds = 12` but `startup_grace = 30` → still in grace period, so `should_inject_notify` returns 1 (suppress) at line 449–451.
2. `bus_check_counter = 0` → just finished a bus check cycle (counter resets at `bus_check_interval`).
3. `notify_fail_count = 0` → no failed delivery attempts.
4. `last_notify_time = 0` → zero (not epoch 0) because `memset` initialised the state — no notification has ever been sent.

**Root cause:** The sidecar was recently started and is still in the 30-second startup grace period. Wait for grace to expire before investigating further.

**Why GDB:** Printing every field of a 1.5 KB struct takes one GDB command. With `printf`, you would need to add ~20 format strings, recompile, and restart.

---

### Worked Example 8 — Walk the Cursor Participants Array

**Scenario:** You suspect a cursor tracking bug — a handle's read position is wrong. You want to see all participants and their cursor positions.

**Setup:** Break on `chat_cursor_write` after the cursor file has been read into the `handles[]` and `indices[]` arrays (line 1626):

```bash
gdb -q ~/claude_docs/nbs-framework/src/nbs-chat/nbs-chat
(gdb) break chat_file.c:1626
(gdb) run read /path/to/.nbs/chat/gdb.chat --unread=gdb-walk-test
```

**Session:**

```
Breakpoint 1, chat_cursor_write (
    chat_path=0x7fffffffacf0 ".../.nbs/chat/gdb.chat",
    handle=0x7fffffffc58e "gdb-walk-test",
    index=14) at chat_file.c:1627

(gdb) print count
$1 = 9

(gdb) print found
$2 = 0

(gdb) print handles[0]
$3 = "nbs-chat-init", '\000' <repeats 50 times>
(gdb) print indices[0]
$4 = 0

(gdb) print handles[1]
$5 = "alexie", '\000' <repeats 57 times>
(gdb) print indices[1]
$6 = 12

(gdb) print handles[2]
$7 = "scribe", '\000' <repeats 57 times>
(gdb) print indices[2]
$8 = 14

(gdb) print handles[3]
$9 = "medic", '\000' <repeats 58 times>
(gdb) print indices[3]
$10 = 14

(gdb) print handles[4]
$11 = "supervisor", '\000' <repeats 53 times>
(gdb) print indices[4]
$12 = 14

(gdb) print handles[5]
$13 = "gatekeeper", '\000' <repeats 53 times>
(gdb) print indices[5]
$14 = 14

(gdb) print handles[6]
$15 = "theologian", '\000' <repeats 53 times>
(gdb) print indices[6]
$16 = 14
```

**Finding:** 9 participants total. `found = 0` means `gdb-walk-test` is not yet in the cursor file (first read). All team agents (`scribe`, `medic`, `supervisor`, `gatekeeper`, `theologian`) have cursor position 14 (fully caught up). `alexie` (the supervisor who posted) is at position 12 — she has 2 unread messages. `nbs-chat-init` is at 0 because it is a system handle that does not read.

**Why GDB:** The cursor file is a simple key=value format, but during the `chat_cursor_write` read–modify–write cycle, the data exists only in stack-allocated arrays. You cannot observe this in-memory state with `cat` on the file — GDB catches it mid-operation.

---

## Category 5 — Reverse Debugging and Watchpoints

### Worked Example 9 — Hardware Watchpoint — Catch Who Modifies a Variable

**Scenario:** The sidecar's `idle_seconds` is unexpectedly resetting. You want to catch the exact line that writes to it, without adding logging.

**Setup:** Attach GDB 16.3 to a running debug sidecar via `nbs-local-session`:

```bash
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" "/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar \
  -ex 'set sysroot /' -ex 'attach $SIDECAR_PID'"
sleep 3 && nbs-ts read-new "$GDB_HANDLE" --strip
```

**Session:** Each command sent via `nbs-ts send`, output read via `nbs-ts read-new`:

```bash
$ nbs-ts send "$GDB_HANDLE" "watch state.idle_seconds"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
watch state.idle_seconds
Hardware watchpoint 1: state.idle_seconds
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "continue"
$ sleep 3 && nbs-ts read-new "$GDB_HANDLE" --strip
```
```
continue
Continuing.

Hardware watchpoint 1: state.idle_seconds

Old value = 7
New value = 8
sidecar_run (cfg=0x7ffd2a28fe30, tp=0x7ffd2a28fe00) at sidecar.c:1325
1325        if (state.bus_check_counter < INT_MAX)
(gdb)
```

**Finding:** The hardware watchpoint fires every time `idle_seconds` is modified, showing the **exact line** (sidecar.c:1325) and both old and new values. The increment is 7→8, confirming a healthy 1-per-tick increment. If the value suddenly reset to 0, the watchpoint would fire at the reset line — **catching the writer without knowing the writer in advance**.

**Why GDB:** With `printf`, you would need to add logging at every assignment to `idle_seconds` across the codebase. A hardware watchpoint catches all writes regardless of source — including writes from macros, inlined functions, or memory corruption from a different variable.

---

### Worked Example 10 — Reverse Stepping with `record btrace pt`

**Scenario:** You want to step backwards through execution to find when a config field was set.

**Setup:** GDB 16.3 supports `record btrace pt` (Intel Processor Trace) which has much lower overhead than `record full`. Note: `record full` fails on this platform because glibc uses AVX-512 EVEX instructions (opcode `0x62`) that `record full` does not support.

```bash
/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar
(gdb) break main
(gdb) run --handle=gdb-rev --root=/tmp/gdb-test-root --session=fakesession
```

**Session:**

```
Breakpoint 1, main (argc=4, argv=0x7fffffffbec8) at main.c:134
134     memset(&cfg, 0, sizeof(cfg));

(gdb) record btrace pt

(gdb) next
137     env_str("NBS_HANDLE", cfg.handle, sizeof(cfg.handle));
(gdb) next
138     env_str("NBS_ROOT", cfg.nbs_root, sizeof(cfg.nbs_root));
...
(gdb) next
147     cfg.flush_interval = env_int("NBS_FLUSH_INTERVAL", 60);

(gdb) info record
Active record target: record-btrace
Recording format: Intel Processor Trace.
Buffer size: 16kB.
Recorded 12078 instructions in 187 functions (0 gaps) for thread 1

(gdb) print cfg.handle
$1 = '\000' <repeats 63 times>

(gdb) reverse-next
146     cfg.notify_fail_threshold = env_int("NBS_NOTIFY_FAIL_THRESHOLD", 5);
(gdb) reverse-next
145     cfg.startup_grace = env_int("NBS_STARTUP_GRACE", 30);

(gdb) list
140     env_str("NBS_REMOTE_HOST", cfg.remote_host, sizeof(cfg.remote_host));
141     env_str("NBS_REMOTE_SSH_OPTS", cfg.remote_ssh_opts, sizeof(cfg.remote_ssh_opts));
142
143     cfg.bus_check_interval = env_int("NBS_BUS_CHECK_INTERVAL", 3);
144     cfg.notify_cooldown = env_int("NBS_NOTIFY_COOLDOWN", 15);
145     cfg.startup_grace = env_int("NBS_STARTUP_GRACE", 30);
146     cfg.notify_fail_threshold = env_int("NBS_NOTIFY_FAIL_THRESHOLD", 5);
147     cfg.flush_interval = env_int("NBS_FLUSH_INTERVAL", 60);
```

**Finding:** `reverse-next` steps backwards through execution history. After stepping forward past `cfg.handle` assignment, we stepped backwards to see the sequence of config initialisation. **`cfg.handle` is empty** (`'\000' <repeats 63 times>`) because `env_str("NBS_HANDLE", ...)` found no environment variable and the `--handle=` argument is parsed later (lines 186+). The recording captured 12,078 instructions across 187 function calls with **zero gaps** — Intel PT provides clean, complete traces.

**Why GDB:** Reverse stepping lets you answer "what was the state *before* this line executed?" without re-running the program. This is invaluable for corruption bugs where you see the symptom but need to find the cause earlier in the execution.

### Reverse Debugging Limitations

- **`record full`** does not work on this platform — glibc's AVX-512 `memset`/`memcpy` use EVEX instructions that `record full` cannot log. Error: `Process record does not support instruction 0x62`.
- **`record btrace pt`** works but requires Intel Processor Trace hardware support. Variable values may show as `<unavailable>` during reverse execution because btrace records control flow, not data flow.
- **Hardware watchpoints** are the most reliable alternative for "who wrote this value?" investigations and work on all platforms without recording overhead.

### Platform Notes

The examples in this document were captured on x86-64 Linux with GDB 16.3 and an Intel CPU with Processor Trace support. On other platforms:

- **ARM (aarch64):** `record btrace pt` is unavailable (Intel PT is Intel-only). `record full` may work if glibc does not use unsupported instructions. Hardware watchpoints work. All non-reverse-debugging examples (1–8, 11–13) work unchanged.
- **AMD:** Intel PT is unavailable. `record btrace` will fail. All other examples work.
- **VMs without PT passthrough:** `record btrace pt` requires the hypervisor to expose Intel PT to the guest. Most cloud VMs do not. Hardware watchpoints still work.
- **Older GDB (< 14):** `record full` has more instruction gaps. Use hardware watchpoints instead of reverse debugging. The `call`, `x/`, `display`, and stepping examples work on any GDB version with debug symbols.

---

## Category 6 — Advanced Interactive Techniques

### Worked Example 11 — Interactive Stepping Through `should_inject_notify()`

**Scenario:** Notifications are not being delivered. You want to follow the exact decision path inside `should_inject_notify()` — watching each branch decision as it happens.

**Setup:** Attach to a running debug sidecar via `nbs-local-session`, navigate to the `sidecar_run` frame, set a breakpoint on `should_inject_notify`, then invoke it with `call`:

```bash
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" "/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar \
  -ex 'set sysroot /' -ex 'set debuginfod enabled off' -ex 'attach $SIDECAR_PID'"
sleep 3 && nbs-ts read-new "$GDB_HANDLE" --strip
```

**Session:**

```bash
$ nbs-ts send "$GDB_HANDLE" "frame 3"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
frame 3
#3  0x0000000000405b0a in sidecar_run (cfg=0x7ffff27f3250, tp=0x7ffff27f3220)
    at sidecar.c:866
866         sleep(1);
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "break should_inject_notify"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
break should_inject_notify
Breakpoint 1 at 0x40494d: file sidecar.c, line 440.
(gdb)
```

Use `call` to invoke the function — GDB will stop at the breakpoint:

```bash
$ nbs-ts send "$GDB_HANDLE" "call should_inject_notify(cfg, &state, registry_path)"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
Breakpoint 1, should_inject_notify (cfg=0x7ffff27f3250, state=0x7ffff27efbd0,
    registry_path=0x7ffff27f10c0 "/tmp/gdb-test-root/.nbs/control-registry-gdb-ex11")
    at sidecar.c:440
440     ASSERT_MSG(cfg != NULL, "should_inject_notify: cfg is NULL");
The program being debugged stopped while in a function called from GDB.
(gdb)
```

Now step through the decision tree, inspecting each branch:

```bash
$ nbs-ts send "$GDB_HANDLE" "next 3"    # skip past asserts
```
```
448     if (state->sidecar_start_time > 0) {
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "next"
```
```
449         if ((now - state->sidecar_start_time) < cfg->startup_grace) {
(gdb)
```

**Key decision point — is startup grace still active?**

```bash
$ nbs-ts send "$GDB_HANDLE" "print now - state->sidecar_start_time"
```
```
$7 = 63
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "print cfg->startup_grace"
```
```
$8 = 30
(gdb)
```

63 > 30 — grace period expired. Step past:

```bash
$ nbs-ts send "$GDB_HANDLE" "next"
```
```
459     if (!state->startup_notify_sent) {
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "print state->startup_notify_sent"
```
```
$9 = 0
(gdb)
```

**`startup_notify_sent = 0`** — the catch-up notification has never fired. Step into the catch-up block:

```bash
$ nbs-ts send "$GDB_HANDLE" "next"    # sets startup_notify_sent = 1
$ nbs-ts send "$GDB_HANDLE" "next"    # snprintf the message
$ nbs-ts send "$GDB_HANDLE" "next"    # sets last_notify_time
```
```
464         return 0;  /* Force notification delivery */
(gdb)
```

```bash
$ nbs-ts send "$GDB_HANDLE" "print state->notify_message"
```
```
$10 = "sidecar startup catch-up", '\000' <repeats 231 times>
(gdb)
```

**Finding:** By stepping through `should_inject_notify()`, we watched the function take the startup catch-up path (lines 459–464): `startup_notify_sent` was 0, so it set the message to "sidecar startup catch-up" and returned 0 (inject). **The function never reached the bus/chat check logic at lines 467+** because the catch-up notification takes priority. On the next call, `startup_notify_sent` will be 1 and the function will proceed to check bus events and chat unreads.

**Why GDB:** Stepping lets you follow the exact execution path — you see which branches are taken and which are skipped. With `printf`, you would need to add logging at every `if` statement. With interactive stepping, you inspect variables *at each decision point* and understand the causal chain.

---

### Worked Example 12 — Function Injection with `call`

**Scenario:** You want to test individual functions on a running sidecar without modifying code or waiting for the right conditions to occur naturally.

**Setup:** Same as Example 11 — attach to sidecar, navigate to `sidecar_run` frame.

**Session:**

```bash
$ nbs-ts send "$GDB_HANDLE" "call cooldown_is_active(cfg, &state)"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
call cooldown_is_active(cfg, &state)
$1 = 0
(gdb)
```

Cooldown is not active (returns 0). **Note:** `call` has real side effects — Example 11's `call should_inject_notify(...)` set `startup_notify_sent=1`, which is still in effect because we are in the same GDB session.

Now test prompt idle detection:

```bash
$ nbs-ts send "$GDB_HANDLE" "call detect_prompt_idle(content)"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
call detect_prompt_idle(content)
$2 = 0
(gdb)
```

Prompt is not idle (returns 0) — this explains why `should_inject_notify` is never called naturally. Now invoke it manually:

```bash
$ nbs-ts send "$GDB_HANDLE" "call should_inject_notify(cfg, &state, registry_path)"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
call should_inject_notify(cfg, &state, registry_path)
$3 = 1
(gdb)
```

Returns 1 (suppress) — no notification should fire. Check sidecar uptime:

```bash
$ nbs-ts send "$GDB_HANDLE" "print (int)((long)time(0) - state.sidecar_start_time)"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
$5 = 40
(gdb)
```

**Finding:** `call` turns GDB into a C REPL — you can invoke any function in the binary with arbitrary arguments, on a live process. Here we tested three functions without modifying code: `cooldown_is_active` (cooldown not active), `detect_prompt_idle` (prompt not idle — the session being monitored is not a Claude Code session), and `should_inject_notify` (suppress notification). We even called `time(0)` to compute the sidecar's uptime.

**Why GDB:** `printf` debugging requires you to predict which function calls to log *before* the interesting event. `call` lets you test hypotheses on demand — "what would `cooldown_is_active` return right now?" — without recompiling.

---

### Worked Example 13 — Memory Examination with `x/` and `display`

**Scenario:** You suspect a buffer contains stale or corrupted data beyond the NUL terminator. You want to see the raw bytes.

**Setup:** Same session — attached to running sidecar in the `sidecar_run` frame.

**Session:**

Examine `notify_message` as ASCII characters — see the string and what lies beyond the NUL:

```bash
$ nbs-ts send "$GDB_HANDLE" "x/32c state.notify_message"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
x/32c state.notify_message
0x7ffff27eff40: 115 's' 105 'i' 100 'd' 101 'e' 99 'c'  97 'a' 114 'r' 32 ' '
0x7ffff27eff48: 115 's' 116 't'  97 'a' 114 'r' 116 't' 117 'u' 112 'p' 32 ' '
0x7ffff27eff50:  99 'c'  97 'a' 116 't'  99 'c' 104 'h'  45 '-' 117 'u' 112 'p'
0x7ffff27eff58:   0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000'
(gdb)
```

Clean NUL termination at byte 24, zeroes beyond. Now examine `mention_payload` — should be empty:

```bash
$ nbs-ts send "$GDB_HANDLE" "x/32c state.mention_payload"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
x/32c state.mention_payload
0x7ffff27efc28: 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000'
0x7ffff27efc30: 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000'
0x7ffff27efc38: 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000'
0x7ffff27efc40: 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000' 0 '\000'
(gdb)
```

All zeroes — no mention data. Hex dump the first 64 bytes of the state struct:

```bash
$ nbs-ts send "$GDB_HANDLE" "x/8gx &state"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
x/8gx &state
0x7ffff27efbd0: 0x0000000200000002  0xf4aad92a29477f5c
0x7ffff27efbe0: 0x0000000069d26fd3  0x0000000069d27012
0x7ffff27efbf0: 0x0000000069d26fd3  0x0000000069d26fd3
0x7ffff27efc00: 0x0000000069d26fd3  0x0000000069d26fd3
(gdb)
```

First 8 bytes: `idle_seconds=2` and `bus_check_counter=2` (two 32-bit ints packed into one 64-bit word). Next 8 bytes: `last_content_hash=0xf4aad92a29477f5c`. Then four identical timestamps (`0x69d26fd3` = 1775398867 epoch).

**Persistent watches with `display`:**

```bash
$ nbs-ts send "$GDB_HANDLE" "display state.idle_seconds"
$ nbs-ts read-new "$GDB_HANDLE" --strip
```
```
1: state.idle_seconds = 2
(gdb)
```

Now every time GDB stops (breakpoint, watchpoint, step), it automatically prints `state.idle_seconds`. This accumulates — you can add multiple `display` expressions and they all print at every stop.

**Finding:** `x/` shows raw memory including bytes beyond NUL terminators — essential for detecting buffer overflows, stale data, or corruption that `print` would hide. The hex dump reveals the struct's binary layout: how fields pack, what padding looks like, and whether timestamps are consistent. `display` provides persistent monitoring without resending print commands.

**Why GDB:** `printf` shows you what the code *chooses* to print. `x/` shows you what is actually *in memory* — including data the code doesn't know about.

---

## Quick Reference

### Session Setup with `nbs-local-session` and `nbs-remote-session`

Use `nbs-local-session` to create a persistent local shell for GDB, and `nbs-remote-session` for debugging on remote machines. Both return an `nbs-ts` session handle.

```bash
# Local: create a GDB session via nbs-local-session
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" '/usr/bin/gdb -q ~/claude_docs/nbs-framework/src/nbs-sidecar/nbs-sidecar \
  -ex "set sysroot /" -ex "attach $SIDECAR_PID"'
sleep 3 && nbs-ts read-new "$GDB_HANDLE" --strip

# Send GDB commands one at a time
nbs-ts send "$GDB_HANDLE" 'bt'
sleep 1 && nbs-ts read-new "$GDB_HANDLE" --strip

nbs-ts send "$GDB_HANDLE" 'frame 3'
sleep 1 && nbs-ts read-new "$GDB_HANDLE" --strip

nbs-ts send "$GDB_HANDLE" 'print state.idle_seconds'
sleep 1 && nbs-ts read-new "$GDB_HANDLE" --strip

# Remote: debug on a remote machine via nbs-remote-session
GDB_HANDLE=$(nbs-remote-session <remote-host>)
nbs-ts send "$GDB_HANDLE" '/usr/bin/gdb -q ./nbs-sidecar'
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip
```

The session survives context compaction and agent restarts — GDB state (breakpoints, watchpoints, history) is preserved across tool calls.

```bash
# Resume after compaction — find your session by listing alive sessions
nbs-ts list | grep alive
# Then read new output from the GDB session
nbs-ts read-new "$GDB_HANDLE" --strip

# Clean up when done
nbs-ts send "$GDB_HANDLE" 'detach'
nbs-ts send "$GDB_HANDLE" 'quit'
nbs-ts kill "$GDB_HANDLE"
```

### Common Commands

| What | Command |
|------|---------|
| Attach to process | `/usr/bin/gdb -q <debug-binary> -ex "attach PID"` |
| Backtrace | `bt` or `bt 5` (limit depth) |
| Navigate frames | `frame N` |
| Print struct | `print state` or `print *cfg` |
| Print field | `print state.idle_seconds` |
| Show struct definition | `ptype sidecar_state_t` |
| Conditional break | `break file.c:LINE if expr` |
| Watch variable | `watch var` or `watch *(int *)&var` |
| Record (Intel PT) | `record btrace pt` |
| Reverse step | `reverse-next` or `reverse-continue` |
| Show locals | `info locals` |
| Show arguments | `info args` |
| Show FDs | `shell ls -la /proc/PID/fd/` |
| Process info | `info proc status` |
| Source context | `list` |

### Important Notes

1. **Symbol file must match the binary.** If the running process was built with `-O2` and you load symbols from a `-O0` build, line numbers and variable locations will be wrong. Either rebuild and restart, or accept that some variables may show as `<optimized out>`.

2. **Do not attach to production phoenix sessions.** Create isolated test sessions with `nbs-local-session` (local) or `nbs-remote-session` (devservers) for debugging.

3. **ASAN + GDB compatibility.** GDB 9.1 crashes with ASAN binaries (`sect_index_data not initialized`). Use `/usr/bin/gdb` (16.3) instead, or run ASAN standalone and use GDB on the non-ASAN debug binary to investigate further.

4. **Set sysroot when attaching.** GDB 16.3 cannot find shared libraries without `set sysroot /`. Always include `-ex 'set sysroot /'` when attaching, or add `set sysroot /` to your `~/.gdbinit`.

5. **Always detach and clean up sessions.** GDB leaves the process in a stopped state if you quit without `detach`. An orphaned GDB attachment holds the target in ptrace-stopped state — a sidecar held by a forgotten ptrace will silently stop processing bus events. After every debugging session:
   ```bash
   nbs-ts send "$GDB_HANDLE" "detach"
   nbs-ts send "$GDB_HANDLE" "quit"
   nbs-ts kill "$GDB_HANDLE"
   ```

6. **Handle slow GDB responses.** The `sleep N && nbs-ts read-new` pattern assumes GDB responds within N seconds. On loaded hosts or during ASAN runs, GDB may need longer. If `read-new` returns empty, retry rather than assuming failure:
   ```bash
   # Preferred: use wait-pattern to wait for the (gdb) prompt
   nbs-ts wait-pattern "$GDB_HANDLE" '(gdb)' --timeout=30

   # Fallback: retry read-new with increasing delays
   nbs-ts read-new "$GDB_HANDLE" --strip  # try immediately
   sleep 3 && nbs-ts read-new "$GDB_HANDLE" --strip  # retry after 3s
   ```
