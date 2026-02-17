# NBS GDB: Remote Debugging with pty-session

Techniques for AI agents to debug native crashes (segfaults, SIGABRT, etc.) on remote machines using GDB via `pty-session`.

## When to Use

- **Segmentation faults** in JIT-compiled code, native extensions, or C/C++ libraries
- **Signal crashes** (SIGSEGV, SIGABRT, SIGBUS) during test runs
- **Wrong values** passed to C functions (visible as TypeError with garbage type names)
- Any crash where you need the **exact instruction, register state, and memory contents**

## Setup

### 1. Create a Minimal Reproducer Script

Always create a standalone Python script that triggers the crash:

```python
#!/usr/bin/env python3
"""Minimal reproducer for <description>."""
# Import and initialise the extension
import myextension
myextension.init()

# Trigger the crash
result = crash_function()
```

Copy it to the remote machine:
```bash
scp reproducer.py remote-host:~/workdir/reproducer.py
```

### 2. Launch GDB via pty-session

```bash
~/.nbs/bin/pty-session create gdb-debug \
    "ssh -t remote-host 'cd ~/workdir && source venv/bin/activate && \
     gdb -q --args python3 reproducer.py'"
```

Wait for the `(gdb)` prompt:
```bash
~/.nbs/bin/pty-session wait gdb-debug '(gdb)' --timeout=30
```

### 3. Configure and Run

```bash
# Stop on segfaults (don't pass to program)
~/.nbs/bin/pty-session send gdb-debug 'handle SIGSEGV stop nopass'
sleep 1

# Run the program
~/.nbs/bin/pty-session send gdb-debug 'run'

# Wait for crash
~/.nbs/bin/pty-session wait gdb-debug 'SIGSEGV\|Program received\|exited' --timeout=60
```

## Debugging Techniques

### Get the Backtrace

```bash
~/.nbs/bin/pty-session send gdb-debug 'bt 20'
sleep 2
~/.nbs/bin/pty-session read gdb-debug --scrollback=80
```

**What to look for:**
- Frames with `?? ()` — these are JIT-generated code (no symbols)
- The transition from JIT code to C code tells you what the JIT called with wrong args
- The arguments in the C frame show what values the JIT passed

### Get Register Dump

```bash
~/.nbs/bin/pty-session send gdb-debug 'info registers'
sleep 2
~/.nbs/bin/pty-session read gdb-debug --scrollback=50
```

GDB may paginate with `--Type <RET> for more--`. Send `c` to continue:
```bash
~/.nbs/bin/pty-session send gdb-debug 'c'
```

### Disassemble JIT Code

Find the JIT frame address from the backtrace (e.g., `#7 0x0000ffffe8b20338 in ?? ()`), then:

```bash
# Select the JIT frame
~/.nbs/bin/pty-session send gdb-debug 'frame 7'

# Disassemble a range around the crash point
~/.nbs/bin/pty-session send gdb-debug 'disas 0x0000ffffe8b20240,0x0000ffffe8b20380'
sleep 2
~/.nbs/bin/pty-session read gdb-debug --scrollback=80
```

**Reading JIT disassembly on aarch64:**
- `blr x16` — indirect function call (x16 loaded with target address)
- `mov x0, xN` / `mov x1, xN` — setting up call arguments (AAPCS64: x0=arg1, x1=arg2, ...)
- `ldr xN, [xM, #offset]` — load from memory (field access)
- `stp`/`ldp` — push/pop register pairs
- `str`/`ldr` with `[sp, #-16]!` — stack push/pop
- `stur`/`ldur` — unscaled offset store/load (for offsets in -256..255)
- `=>` arrow marks the current instruction

### Inspect Objects

Check if an address is a valid Python object:
```bash
~/.nbs/bin/pty-session send gdb-debug 'p *(PyObject *)0x7e3ef8'
```

Check if it's a specific type:
```bash
~/.nbs/bin/pty-session send gdb-debug 'p ((PyLongObject *)0x7e3ef8)->long_value.ob_digit[0]'
```

Check struct layout for offset analysis:
```bash
~/.nbs/bin/pty-session send gdb-debug 'ptype /o PyThreadState'
```

Check what's at a specific offset:
```bash
~/.nbs/bin/pty-session send gdb-debug 'p/x *(uint64_t *)((uint8_t *)0x8533e0 + 56)'
```

### Examine Memory Around an Address

```bash
~/.nbs/bin/pty-session send gdb-debug 'x/16gx 0xfffffffff050'
```

### Look Up Symbol Addresses

```bash
~/.nbs/bin/pty-session send gdb-debug 'p/x (void*)PyNumber_Add'
```

## Common Patterns

### Pattern: Wrong Arguments to C Function

**Symptom:** Crash in a C function like `PyNumber_Add`, `PyObject_Call`, etc. with garbage arguments.

**Diagnosis:**
1. Get backtrace — find the JIT frame
2. Select the C function frame — check argument values
3. Are arguments `_PyRuntime+offset` or stack addresses? → Register clobber in JIT prologue
4. Is one argument NULL? → Missing null check / guard in JIT codegen

### Pattern: JIT Function Prologue Clobber

**Symptom:** One operand is correct, the other is a frame/thread pointer.

**Diagnosis:**
1. Disassemble the JIT function from the start
2. Find where the argument registers (x0, x1) are first used
3. Check if anything writes to those registers before they're saved
4. The `mov wN, immediate` + `strb wN, [mem]` pattern on aarch64 may clobber argument registers — x86 doesn't have this issue because it can do `mov [mem], immediate` directly

### Pattern: Offset Error in Field Access

**Symptom:** Crash on `ldr xN, [xM, #offset]` where xM is valid but offset is wrong.

**Diagnosis:**
1. Check what struct xM points to: `p *(StructType *)$xM`
2. Check the struct layout: `ptype /o StructType`
3. Compare the `#offset` in the instruction with the actual field offset

## Cleanup

Always kill the GDB session when done:
```bash
~/.nbs/bin/pty-session kill gdb-debug
```

## Tips

1. **Avoid PYTHONJITDUMPASM=1 with GDB** — the dump output is huge and may cause the session to hang. Use GDB's own `disas` command to view generated code at the crash point.

2. **Handle pagination** — GDB paginates long output. Send `c` to continue, `q` to quit paging. Or disable paging before running: `set pagination off`.

3. **SSH -t flag** — Always use `ssh -t` to allocate a pseudo-terminal. GDB needs it for interactive features.

4. **Copy scripts, don't inline** — Multi-line Python in SSH commands gets mangled by shell escaping. Always `scp` a script file to the remote machine and run that.

5. **One GDB session at a time** — Don't run multiple GDB sessions on the same core/process. Kill old sessions before creating new ones.

6. **Check for core dumps** — If a crash already happened, you may find core dumps at `/var/tmp/cores/` or similar. Load them directly: `gdb python3 /var/tmp/cores/core.12345`.
