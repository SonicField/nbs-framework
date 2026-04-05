---
description: Falsification-focused GDB debugging via nbs-ts for persistent, hypothesis-driven debugging
allowed-tools: Read, Glob, Grep, Bash(nbs-ts:*), Bash(file:*), Bash(stat:*), Bash(md5sum:*), Bash(ls:*)
---

# NBS GDB

You are conducting a **GDB debugging session** using the NBS falsification framework. GDB is run via `nbs-ts` for persistence across tool calls and context compaction.

---

## Core Principle

**Hypothesis before commands.** Never type a GDB command without first stating what you expect to see and what result would falsify your hypothesis. GDB is an instrument for measurement, not exploration.

---

## Step 0: Build Verification (MANDATORY)

Before attaching GDB to anything, verify the binary matches the source.

```bash
# Check binary timestamp vs source modification time
stat --format='%Y %n' /path/to/binary
stat --format='%Y %n' /path/to/source/*.cpp | sort -rn | head -5

# If binary is OLDER than any source file: STOP. Rebuild first.
# Record the md5 hash for later verification
md5sum /path/to/binary
```

**Falsifier**: If the binary timestamp is older than the most recent source modification, any GDB results are against the WRONG code. Do not proceed.

This step exists because stale builds have caused incorrect root cause analyses in this project. Build verification is not optional.

---

## Step 1: State the Hypothesis

Before starting GDB, write down:

1. **What you believe is happening** (one sentence)
2. **What GDB observation would FALSIFY this belief** (specific register value, memory content, or control flow)
3. **What GDB observation would SUPPORT this belief** (specific register value, memory content, or control flow)

Example:
> **Hypothesis**: GenDataFooter's resumeEntry is NULL because Py_SIZE(gen) is 0, causing genDataFooter() to compute the wrong address.
> **Falsifier**: If Py_SIZE(gen) > 0 at crash time, ob_size is not the cause.
> **Support**: If Py_SIZE(gen) = 0 AND tp_itemsize > 0, the allocator failed to set ob_size.

---

## Step 2: Create GDB Session

Use `nbs-local-session` (or `nbs-remote-session` for remote machines) for persistence. The GDB session survives across tool calls, context compaction, and agent restarts. These wrappers provide the user's login environment (SSH keys, proxy credentials).

```bash
# Local debugging
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" '/usr/bin/gdb -q -ex "set sysroot /" --args /path/to/binary'
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip

# Remote debugging
GDB_HANDLE=$(nbs-remote-session <remote-host>)
nbs-ts send "$GDB_HANDLE" '/usr/bin/gdb -q -ex "set sysroot /" --args ./binary'
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip

# Attach to a running process
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" '/usr/bin/gdb -q -ex "set sysroot /" -p $PID'
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip
```

Use `/usr/bin/gdb` (GDB 16.3), not `/usr/local/bin/gdb` (GDB 9.1) — the newer version has better reverse debugging and fewer ASAN conflicts.

`set sysroot /` is required — GDB 16.3 cannot find shared libraries without it.

For worked examples with real GDB output, see `terminal-weathering/concepts/gdb-debugging.md` (`nbs-help debug`).

---

## Step 3: Set Up Experiment

Design the minimal GDB commands to test your hypothesis. Prefer:

- **Breakpoints** over stepping (faster, more precise)
- **Watchpoints** for corruption bugs (catch the writer, not the reader)
- **Conditional breakpoints** to filter noise

```bash
# Set breakpoint
nbs-ts send $HANDLE 'break generators_rt.cpp:107'
sleep 1 && nbs-ts read-new $HANDLE --strip

# Set watchpoint (for corruption bugs)
nbs-ts send $HANDLE 'watch *(int64_t*)0xADDRESS'
sleep 1 && nbs-ts read-new $HANDLE --strip

# Conditional breakpoint
nbs-ts send $HANDLE 'break jit_rt.cpp:745 if gen->ob_size == 0'
sleep 1 && nbs-ts read-new $HANDLE --strip

# Run to trigger
nbs-ts send $HANDLE 'run'
sleep 5 && nbs-ts read-new $HANDLE --strip
```

---

## Step 4: Observe and Record

Read GDB output after each command. Record EXACT values, not summaries.

```bash
# Read current output
nbs-ts read-new $HANDLE --strip

# Examine registers (aarch64)
nbs-ts send $HANDLE 'info registers x0 x1 x29 x30 pc'
sleep 1 && nbs-ts read-new $HANDLE --strip

# Examine memory
nbs-ts send $HANDLE 'x/8gx $address'
sleep 1 && nbs-ts read-new $HANDLE --strip

# Print expression
nbs-ts send $HANDLE 'print ((PyVarObject*)gen)->ob_size'
sleep 1 && nbs-ts read-new $HANDLE --strip
```

**Record format** (paste into chat or document):
```
OBSERVATION: Py_SIZE(gen) = 0 at crash point
GDB: print ((PyVarObject*)0xffffe8cdfe28)->ob_size = 0
VERDICT: Consistent with hypothesis (ob_size not set)
```

---

## Step 5: Discriminating Experiments

When two hypotheses explain the same crash, design an experiment that distinguishes them.

| Hypothesis A | Hypothesis B | Discriminating observation |
|-------------|-------------|--------------------------|
| ob_size never set | ob_size zeroed after allocation | Watchpoint on ob_size: if write count = 0, never set. If write count > 1, zeroed after. |
| Footer at wrong address | Footer never written | Examine OLD footer address: if resumeEntry != 0 there, address mismatch. If 0 everywhere, never written. |
| CPython frame init corrupts | JIT codegen corrupts | Breakpoint BEFORE and AFTER frame init: check value. If changed, CPython. If unchanged, JIT. |

---

## Step 6: Reverse Debugging (when available)

GDB 15+ supports reverse execution with `record full`, but availability is platform-dependent. On x86-64, `record full` may fail if glibc uses AVX-512 instructions — use `record btrace pt` (Intel PT) or hardware watchpoints as alternatives. On aarch64, `record full` may work but is not guaranteed. Always test before relying on it.

```bash
# Enable recording (BEFORE the crash)
nbs-ts send $HANDLE 'record full'
sleep 1 && nbs-ts read-new $HANDLE --strip

# Run until crash
nbs-ts send $HANDLE 'continue'
sleep 5 && nbs-ts read-new $HANDLE --strip

# Now reverse to find who wrote the bad value
nbs-ts send $HANDLE 'reverse-continue'
sleep 5 && nbs-ts read-new $HANDLE --strip
```

**Caveat**: `record-full` is slow (10-100x). Only enable it close to the crash point. Set a breakpoint just before the suspected corruption, enable recording there, then continue.

---

## Common Patterns

### Pattern: Pointer Corruption

```bash
# 1. Find the corrupted pointer address
nbs-ts send $HANDLE 'print &footer->resumeEntry'

# 2. Set a hardware watchpoint
nbs-ts send $HANDLE 'watch *(uint64_t*)ADDRESS'

# 3. Run - GDB will stop at the WRITER
nbs-ts send $HANDLE 'run'

# 4. Check who wrote: bt, info registers
nbs-ts send $HANDLE 'bt 5'
```

### Pattern: Address Mismatch (two computations should agree)

```bash
# Compare two address computations
nbs-ts send $HANDLE 'print (char*)gen + old_formula'
nbs-ts send $HANDLE 'print (char*)gen + new_formula'
# If they differ: the read and write paths disagree
```

### Pattern: Type Metadata Check

```bash
# Check if generator type supports ob_size
nbs-ts send $HANDLE 'print Py_TYPE(gen)->tp_itemsize'
# If 0: ob_size is meaningless for this type
# If > 0: ob_size should be set by allocator
```

### Pattern: Build Verification In-Session

```bash
# Verify the loaded .so matches expected source
nbs-ts send $HANDLE 'info sharedlibrary _project'
# Check the path and verify it matches your build output
```

---

## Session Management

```bash
# List active GDB sessions
nbs-ts list --name=gdb

# Resume after compaction (session persists!)
# Find handle by name, then read
HANDLE=$(nbs-ts find gdb-debug)
nbs-ts read-new $HANDLE --strip

# Clean up when done — MUST detach before killing the session,
# otherwise the debugged process is left frozen in ptrace-stop.
nbs-ts send "$HANDLE" "detach"
sleep 1
nbs-ts send "$HANDLE" "quit"
sleep 1
nbs-ts kill "$HANDLE"
```

---

## Anti-Patterns

1. **Exploring without a hypothesis**: Don't `step` through code looking for something wrong. State what you expect, set a breakpoint, verify.

2. **Ignoring stale builds**: Always Step 0. A stale binary produces correct-looking but meaningless GDB output.

3. **Summarising instead of recording**: Write `ob_size = 0` not `ob_size was small`. Exact values enable others to verify your work.

4. **Single-hypothesis debugging**: Always have at least two hypotheses and a discriminating experiment. Otherwise you'll only find evidence FOR your belief, never against it.

5. **Running GDB via Bash instead of nbs-ts**: Bash tool calls are one-shot. GDB state (breakpoints, watchpoints, history) is lost between calls. Always use nbs-ts for GDB.

---

## The Contract

GDB is a measurement instrument. Your hypothesis is what you're measuring. The falsifier is your calibration. Without all three, you're just pressing buttons.
