# AGENTS-README.md — CinderX Development Environment

> Deploy to: `/home/alexturner/local/cinderx_dev/AGENTS-README.md`
> This document prevents build archaeology. Read it before touching the build.

## Directory Layout

```
~/local/cinderx_dev/
├── python-3.12/          # CPython 3.12.9 source tree (vanilla)
├── python-install/       # CPython 3.12.9 installed (--enable-shared)
│   ├── bin/python3.12    # The Python binary to use for everything
│   └── lib/
│       ├── libpython3.12.so.1.0  # Shared library (REQUIRED by CinderX)
│       └── libpython3.12.so      # Symlink
├── cinderx/              # CinderX source tree (fork)
│   ├── build_cinderx.sh  # CinderX build script (use this, never ad-hoc)
│   ├── cinderx/          # C++ source
│   │   └── PythonLib/    # Python package + _cinderx.so output
│   └── setup.py          # Build system entry point
├── venv/                 # Python venv (uses python-install Python)
└── AGENTS-README.md      # This file
```

## Step 1: Build CPython (one-time, ~10 min)

CinderX requires a shared-library CPython build with all symbols exported.

```bash
cd ~/local/cinderx_dev/python-3.12
LDFLAGS='-Wl,--export-dynamic' ./configure \
    --enable-shared \
    --prefix=$HOME/local/cinderx_dev/python-install \
    --without-ensurepip
make CFLAGS_NODIST='-fvisibility=default' -j8
make install
```

**Or use the script:**
```bash
bash ~/local/cinderx_dev/build_cpython.sh
```

### Verify CPython build

```bash
# Shared library must exist
ls -la ~/local/cinderx_dev/python-install/lib/libpython3.12.so*

# Internal symbols must be exported (CinderX needs them)
nm -D ~/local/cinderx_dev/python-install/lib/libpython3.12.so.1.0 | grep _PyUnion_Type
# Must show a 'D' or 'B' entry, NOT empty
```

### Why these flags

| Flag | Purpose |
|------|---------|
| `--enable-shared` | Creates `libpython3.12.so` (CinderX .so links against it) |
| `-fvisibility=default` | Passed via `CFLAGS_NODIST` at make time. Overrides CPython's `-fvisibility=hidden` (which is appended after user CFLAGS at configure time). Exports `_Py*` symbols from `libpython3.12.so` |
| `-Wl,--export-dynamic` | Exports symbols from the executable's dynamic table |
| `--without-ensurepip` | Saves ~2 min build time (pip not needed) |

## Step 2: Build CinderX (~2 min)

```bash
cd ~/local/cinderx_dev/cinderx
bash build_cinderx.sh
```

**NEVER use ad-hoc cmake, pip install, or setup.py commands.** If the build script needs changes, change the script.

### Verify CinderX build

```bash
ls -la ~/local/cinderx_dev/cinderx/cinderx/PythonLib/_cinderx.so
# Must exist, ~50MB
```

## Step 3: Run CinderX

```bash
export PYTHONPATH=~/local/cinderx_dev/cinderx/cinderx/PythonLib
export LD_LIBRARY_PATH=~/local/cinderx_dev/python-install/lib

~/local/cinderx_dev/python-install/bin/python3.12 -c '
import cinderx
cinderx.init()
import cinderjit
print("CinderX OK:", cinderjit)
cinderjit.auto()
print("JIT enabled")
'
```

**Expected output:** No errors, cinderjit module prints, JIT enabled.

## Patches Applied (Vanilla CPython Compatibility)

CinderX was originally designed for Meta's Cinder CPython fork. These patches make it work with vanilla CPython 3.12.9:

1. **`cinderx/UpstreamBorrow/borrowed-3.12.gen_cached.c:654`** — Added `#ifdef ENABLE_GENERATOR_AWAITER` guard around `Py_CLEAR(gen->gi_ci_awaiter)`. Without this, compilation fails against vanilla CPython headers.

2. **`cinderx/Jit/codegen/gen_asm.cpp:2625` and `cinderx/Jit/generators_mm.cpp:22`** — Made `sizeof(GenDataFooter)` static_asserts conditional on `ENABLE_LIGHTWEIGHT_FRAMES` and `__aarch64__`.

3. **`cinderx/PythonLib/cinderx/__init__.py`** — Changed `is_supported_runtime()` to return `True` for vanilla CPython 3.12 (was checking for `+meta` in `sys.version`).

## Common Errors and Fixes

| Error | Cause | Fix |
|-------|-------|-----|
| `no member 'gi_ci_awaiter' in PyGenObject` | Missing ifdef guard | Apply patch 1 above |
| `sizeof(GenDataFooter) == 80` static_assert | Size mismatch without ENABLE_LIGHTWEIGHT_FRAMES | Apply patch 2 above |
| `cinderx.init()` does nothing, `import cinderjit` fails | `is_supported_runtime()` returns False | Apply patch 3 above |
| `undefined symbol: _PyUnion_Type` | CPython built without `--export-dynamic` | Rebuild CPython with `LDFLAGS='-Wl,--export-dynamic'` |
| `libpython3.12.so: cannot open shared object file` | Missing LD_LIBRARY_PATH | `export LD_LIBRARY_PATH=~/local/cinderx_dev/python-install/lib` |

## Benchmarking

**`benchmark_cinderx.py` is the ONLY benchmark script.** No ad-hoc benchmark scripts are to be created or used. All performance measurements must go through this script.

```bash
export PYTHONPATH=~/local/cinderx_dev/cinderx/cinderx/PythonLib
export LD_LIBRARY_PATH=~/local/cinderx_dev/python-install/lib

# Run the CinderX benchmark suite
~/local/cinderx_dev/python-install/bin/python3.12 \
    ~/local/cinderx_dev/cinderx/benchmark_cinderx.py
```

## Testing

All CinderX tests run via `run_cinderx_tests.sh`:

```bash
cd ~/local/cinderx_dev/cinderx
bash run_cinderx_tests.sh
```

### Phase 1 Subtype Guard Tests

```bash
export PYTHONPATH=~/local/cinderx_dev/cinderx/cinderx/PythonLib
export LD_LIBRARY_PATH=~/local/cinderx_dev/python-install/lib

~/local/cinderx_dev/python-install/bin/python3.12 \
    tests/test_phase1_subtype_guard.py
```

Expected: 8 tests pass, no segfault.

## Architecture Notes

- **CinderX JIT** compiles Python bytecodes to machine code via HIR → LIR → native.
- **Tier 1**: Initial compilation. Uses `tier1Vectorcall` trampoline.
- **Tier 2**: After 1000 invocations (`kTier2ThresholdDefault`), recompiles with inlining.
- **Deopt backoff**: After 1000 guard failures (`kDeoptBackoffThreshold`), detaches JIT from function.
- **Option E** (Phase 1): Skips exact-type guard for types with subclasses to prevent deopt cascades.
