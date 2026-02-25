# CinderX JIT Specialisation — Testing & Benchmarking Guide

Living document covering the full test and benchmark infrastructure for
CinderX adaptive specialisation work on ARM (devgpu004).

## Quick Reference

| What | Command | Where |
|------|---------|-------|
| Run specialisation tests | `for f in tests/test_*.py; do python3 "$f"; done` | devgpu004 |
| Run upstream CinderX tests | `python3 -m pytest cinderx/PythonLib/test_cinderx/` | devgpu004 |
| Run single specialisation test | `python3 tests/test_<opcode>.py` | devgpu004 |
| Run benchmark (JIT vs vanilla) | `bash cinderx_jit_benchmark.sh` | devgpu004 |
| Run specialisation benchmark | `bash benchmark_specialisation.sh` | devgpu004 |

---

## 1. Test Suites

### 1a. Specialisation Correctness Tests (tests/)

- **40 files**, **750 test cases** covering CPython 3.12 adaptive specialisation opcodes
- Each file targets one specialised opcode (e.g. `STORE_SUBSCR_LIST_INT`,
  `BINARY_OP_ADD_FLOAT`, `LOAD_ATTR_INSTANCE_VALUE`)
- Tests verify both the **fast path** (specialised) and **deopt path**
  (type mismatch triggers deoptimisation back to generic dispatch)

### Test Structure

Each test file is self-contained:

```python
#!/usr/bin/env python3
import cinderx; cinderx.init()
import cinderjit; cinderjit.auto()
cinderjit.enable_specialized_opcodes()

WARMUP = 15000  # Standard warmup — ensures JIT compilation + Tier 2

def some_function(x):
    return x + 1

# Warmup
for _ in range(WARMUP):
    some_function(42)

# Test
assert some_function(42) == 43
print("PASS  Test 1: basic addition")
passed += 1
```

**Key conventions:**
- `WARMUP = 15000` — standard across all tests. Exceeds Tier 2 threshold
  (~10k calls) to ensure full JIT compilation with specialised opcodes.
- Each test prints `PASS  Test N: <description>` or `FAIL  Test N: <description>`.
- Exit code 0 = all passed, non-zero = failures.
- Deopt tests warm up with one type, then call with a different type to
  trigger guard failure and fallback to generic dispatch.

### Running Tests

**On devgpu004:**

```bash
cd /data/users/alexturner/cinderx_dev/cinderx

# Single test
python3 tests/test_store_subscr_list_int.py

# All tests (create a runner script)
for f in tests/test_*.py; do
    echo "--- $f ---"
    python3 "$f" 2>&1
    if [ $? -ne 0 ]; then
        echo "FAILED: $f"
    fi
done
```

**Locally (without CinderX):**

Tests detect `ImportError` on `cinderx` and skip JIT-specific assertions.
Some tests may not be meaningful without CinderX.

### Test File Index

| Category | Files | Tests | Opcodes Covered |
|----------|-------|-------|-----------------|
| Binary arithmetic | 7 | 140 | BINARY_OP_ADD_INT, ADD_FLOAT, SUBTRACT_INT, SUBTRACT_FLOAT, MULTIPLY_INT, MULTIPLY_FLOAT, INPLACE_ADD_UNICODE |
| Subscript read | 5 | 92 | BINARY_SUBSCR_LIST_INT, TUPLE_INT, DICT, GETITEM, deopt |
| Subscript write | 1 | 20 | STORE_SUBSCR_LIST_INT |
| Comparison | 3 | 60 | COMPARE_OP_INT, FLOAT, STR |
| Contains | 1 | 20 | CONTAINS_OP_DICT |
| Call | 4 | 80 | CALL_PY_EXACT_ARGS, PY_WITH_DEFAULTS, BUILTIN_CLASS, BUILTIN_FAST, BUILTIN_O |
| Load attribute | 7 | 146 | LOAD_ATTR_INSTANCE_VALUE, MODULE, CLASS, PROPERTY, METHOD_WITH_VALUES, WITH_HINT, GETATTRIBUTE_OVERRIDDEN |
| Store attribute | 2 | 40 | STORE_ATTR_INSTANCE_VALUE, WITH_HINT |
| Load global | 2 | 40 | LOAD_GLOBAL, LOAD_GLOBAL_MODULE |
| For iter | 4 | 52 | FOR_ITER_LIST, GEN, mutation, polymorphic deopt |
| Unpack | 2 | 40 | UNPACK_SEQUENCE_LIST, TWO_TUPLE |
| Other | 1 | 20 | TO_BOOL |
| **Total** | **40** | **750** | |

---

### 1b. Upstream CinderX Tests (cinderx/PythonLib/test_cinderx/)

- **79 files**, **3037+ test functions** covering the full CinderX runtime
- Standard `unittest.TestCase` format — run via `pytest` or `python -m unittest`
- Covers: JIT compiler (`test_cinderjit.py`), static compiler, async lazy
  values, compiler API, strict modules, type system, and more

**Location on devgpu004:**
```
/data/users/alexturner/cinderx_dev/cinderx/cinderx/PythonLib/test_cinderx/
```

**Running:**
```bash
cd /data/users/alexturner/cinderx_dev/cinderx
python3 -m pytest cinderx/PythonLib/test_cinderx/ -x --tb=short

# Or individual file:
python3 -m pytest cinderx/PythonLib/test_cinderx/test_cinderjit.py -v
```

**Combined total: ~3787+ test cases across both suites.**

### 1c. Standalone Test Files (repo root)

Additional standalone tests restored from git history:

| File | Tests | Purpose |
|------|-------|---------|
| `test_loadattr_inline_fastpath.py` | 42 | LOAD_ATTR codegen optimisation correctness (A-lite, inline cache, GuardType+LoadField) |
| `test_super_fix.py` | ~10 | super().__init__() JIT bug fix verification |

**Running:**
```bash
# On devgpu004:
CINDERJIT_ENABLE=1 python3 test_loadattr_inline_fastpath.py -v
python3 test_super_fix.py
```

### 1d. Test Runner: `run_cinderx_tests.sh`

The canonical test runner for the full upstream CinderX test suite on aarch64.
Runs all 41 CinderX test modules and produces a summary report.

```bash
# On devgpu004:
export CINDERX_ROOT=/data/users/alexturner/cinderx_dev/cinderx

./run_cinderx_tests.sh              # Run all tests
./run_cinderx_tests.sh jit          # Run only JIT tests
./run_cinderx_tests.sh runtime      # Run only runtime tests
./run_cinderx_tests.sh compiler     # Run only compiler tests
./run_cinderx_tests.sh TESTNAME     # Run a specific test module
```

**Note:** The runner sets `PYTHONPATH` to include `cinderx/PythonLib` and
gates on CinderX JIT availability. It will not silently run on stock Python.

---

### CRITICAL: Script Selection

There are multiple benchmark scripts on devgpu004. **Not all are equivalent.**
Using the wrong script produces misleading results.

| Script | Purpose | Specialisation? | Warmup | Baseline |
|--------|---------|-----------------|--------|----------|
| `cinderx_jit_benchmark.sh` | **PRIMARY** — CinderX JIT vs vanilla CPython | Yes (`enable_specialized_opcodes(True)`) | 3-phase: specialise → force_compile → thermal | Vanilla fbcode Python 3.12 (`-I`) |
| `benchmark_specialisation.sh` | Spec ON vs spec OFF (both CinderX) | Compared | `WARMUP=15000` + `cinderjit.auto()` | CinderX without specialisation |
| `benchmark_warmup_50k.sh` | Warmup-fixed version of specialisation benchmark | Compared | `WARMUP=50000` | CinderX without specialisation |
| `benchmark_abba.py` | Python ABBA harness for individual benchmarks | Depends on caller | Configurable | Configurable |
| `benchmark_cinderx_full.sh` | **DO NOT USE** — missing `enable_specialized_opcodes()` | **No** | `compile_after_n_calls(100)` | Broken (system python imports cinderx) |

### The Correct Benchmark: `cinderx_jit_benchmark.sh`

This is the script that produced the validated results (e.g. fibonacci 1.23x,
richards 2.29x spec ON vs OFF). Restored from commit `b723fa2`.

**How it works:**

1. **Phase 1 — Specialisation warmup** (20 iterations):
   Each benchmark function is called 20 times with representative inputs.
   This lets CPython's adaptive interpreter specialise bytecodes
   (e.g. `BINARY_OP` → `BINARY_OP_ADD_FLOAT`).

2. **Phase 2 — JIT compilation** (`cinderjit.force_compile()`):
   After specialisation, each function is explicitly compiled by the JIT.
   The JIT sees the specialised bytecodes and generates type-guarded machine code.

3. **Phase 3 — Thermal warmup** (3 iterations):
   The compiled code is run 3 times to warm CPU caches and thermal state.

4. **Phase 4 — Measurement** (ABBA × N_REPS):
   JIT_ON and JIT_OFF conditions are interleaved in ABBA pattern to
   control for temporal drift.

**Running:**

```bash
ssh devgpu004
cd /data/users/alexturner/cinderx_dev/cinderx

# Update paths if needed (script defaults to $HOME/local/cinderx_dev/)
export CINDERX_ROOT=/data/users/alexturner/cinderx_dev/cinderx
export CINDERX_VENV=/data/users/alexturner/cinderx_dev/venv

bash cinderx_jit_benchmark.sh [N_REPS]
# Default: 2 reps = 8 runs (4 JIT_ON + 4 JIT_OFF)
```

**Expected results (aarch64, 46de56af + specialisation patches):**

| Benchmark | JIT/Vanilla | Verdict |
|-----------|-------------|---------|
| fibonacci | ~1.2x | JIT wins |
| method_calls | ~1.3x | JIT wins (inliner) |
| richards | ~1.2x | JIT wins (with specialisation) |
| nqueens | ~1.2x | JIT wins (with specialisation) |
| generator_simple | ~1.08x | JIT wins |

### Warmup Order Matters

**The single most important thing to get right.**

```
WRONG:  JIT compile → run specialised opcodes
         (JIT compiles generic bytecodes, no specialisation benefit)

RIGHT:  Run N times → CPython specialises bytecodes → JIT compile
         (JIT sees BINARY_OP_ADD_FLOAT, emits type-guarded fast path)
```

If results show CinderX slower than vanilla CPython on compute-bound
benchmarks, check warmup order first. The symptom is identical to
not calling `enable_specialized_opcodes()`.

### Known Limitations

1. **JIT coverage gaps**: The JIT cannot compile functions using:
   - `*args` / `**kwargs` forwarding
   - Custom `__getattr__` / `__setattr__` / `__delattr__`
   - `__enter__` / `__exit__` context protocol
   - Complex `__init__` with `object.__setattr__`

   Functions with these patterns run in CinderX's interpreter, which is
   ~2x slower than vanilla CPython's interpreter due to JIT infrastructure
   overhead (shadow frames, type tracking, profiling counters).

2. **Richards benchmark variants**: The pyperformance Richards
   (8 task classes, polymorphic dispatch) behaves differently from
   simplified Richards with lambda closures. Always use the pyperformance
   version for fair comparison.

3. **nqueens on aarch64**: Known LICM GuardType hoisting crash. Skip in
   benchmarks unless specifically testing nqueens.

4. **Segfault with `import time` + `enable_specialized_opcodes()` + 55k+ calls**:
   Pre-existing JIT bug. `time` module C extension interaction with
   specialised opcodes causes segfault when a function hits JIT
   compilation threshold. Does not affect benchmark scripts (they don't
   trigger this pattern). Tracked separately.

### Benchmark Results Interpretation

| Ratio | Meaning |
|-------|---------|
| C/A > 1.0 | CinderX JIT is faster than vanilla CPython |
| C/A < 1.0 | CinderX is slower (JIT overhead exceeds benefit) |
| A/B > 1.0 | Specialisation (or inliner) is helping |
| A/B ≈ 1.0 | Specialisation has no effect on this workload |

Where:
- **A** = CinderX with specialisation / inliner ON
- **B** = CinderX without specialisation / inliner OFF
- **C** = Vanilla CPython (no CinderX)

---

## 3. devgpu004 Environment

### Paths

```
/data/users/alexturner/cinderx_dev/
├── cinderx/                    # CinderX source (git repo)
│   ├── cinderx/Jit/hir/        # JIT compiler HIR passes
│   │   ├── builder.cpp          # Opcode → HIR translation
│   │   ├── simplify.cpp         # HIR simplification (UseType + CallStatic)
│   │   └── guard_removal.cpp    # GuardType elimination pass
│   ├── cinderx/Jit/bytecode.cpp # Specialised opcode routing
│   ├── cinderx/Jit/compiler.cpp # Pass pipeline ordering
│   └── tests/                   # Correctness test suite
└── venv/                       # CinderX Python venv
    └── bin/python3              # Python 3.12.12+meta with CinderX
```

### Vanilla Python Baseline

```
/usr/local/fbcode/platform010-aarch64/bin/python3.12
```

Verified clean — cannot import cinderx. Use this for baseline benchmarks.

**Do NOT use** the system `python3.12` from PATH — it may resolve to the
venv Python (with CinderX), giving a contaminated baseline.

### Building CinderX

CinderX is installed via pip (development mode):

```bash
cd /data/users/alexturner/cinderx_dev/cinderx
pip install -e .  # Development install — changes to .cpp files require rebuild
```

After modifying C++ files (builder.cpp, simplify.cpp, etc.):

```bash
pip install -e .  # Rebuilds the C extension
```

### HIR Dump (Debug Builds Only)

```bash
PYTHONJITDUMPHIRPASSES=1 python3 -c "..."
```

Shows HIR before/after each pass. Useful for verifying GuardType survival
through GuardTypeRemoval pass. Requires debug build (`--with-pydebug`).

---

## 4. Common Pitfalls

### Pitfall 1: Missing `enable_specialized_opcodes()`

**Symptom**: CinderX slower than vanilla CPython on compute-bound benchmarks.
Richards at ~1400ms instead of ~625ms.

**Cause**: Without `cinderjit.enable_specialized_opcodes()`, CPython's
adaptive interpreter does not route specialised opcodes to CinderX's
JIT. The JIT compiles generic opcodes with no type information.

**Fix**: Call `cinderjit.enable_specialized_opcodes()` before warmup.

### Pitfall 2: Wrong Warmup Order

**Symptom**: `cinderjit.is_jit_compiled(func)` returns True but
performance matches non-specialised baseline.

**Cause**: JIT compiled the function BEFORE CPython specialised the
bytecodes. The JIT sees generic `BINARY_OP` instead of
`BINARY_OP_ADD_FLOAT`.

**Fix**: Run the function N times FIRST (≥20 for CPython specialisation),
THEN call `cinderjit.force_compile()`.

### Pitfall 3: Contaminated Baseline

**Symptom**: Vanilla Python baseline shows same performance as CinderX.

**Cause**: System `python3.12` resolves to the venv Python (with CinderX).

**Fix**: Use the explicit path:
`/usr/local/fbcode/platform010-aarch64/bin/python3.12`

### Pitfall 4: Benchmark Script Version Mismatch

**Symptom**: Unexplained regression compared to previous results.

**Cause**: Different benchmark scripts measure different things:
- `cinderx_jit_benchmark.sh` → CinderX vs vanilla CPython
- `benchmark_specialisation.sh` → spec ON vs spec OFF (both CinderX)
- `benchmark_cinderx_full.sh` → BROKEN (missing specialisation)

**Fix**: Always use `cinderx_jit_benchmark.sh` for the primary
CinderX-vs-vanilla comparison. Record which script produced each result.

### Pitfall 5: Insufficient Warmup for Test Suite

**Symptom**: Correctness test deopt failures on devgpu004 but not locally.

**Cause**: `WARMUP = 15000` may not be enough for Tier 2 JIT compilation
under certain system load conditions.

**Fix**: Increase to `WARMUP = 50000` if tests are flaky.

---

## 5. Adding New Tests

Follow the established pattern:

1. **One file per opcode**: `tests/test_<opcode_name>.py`
2. **20 test cases**: Standard count. Include:
   - Basic fast-path operation (3–5 tests)
   - Edge cases (boundary values, special values like NaN/inf/None)
   - Deopt triggers (type mismatch after warmup, 3–5 tests)
   - Loop patterns (accumulator, rapid alternation)
   - Equivalence checks (JIT result == interpreter result)
3. **Warmup**: `WARMUP = 15000` (standard)
4. **Self-contained**: No external dependencies beyond cinderx/cinderjit
5. **Summary line**: Print `{passed}/{passed+failed} passed` at the end

---

## 6. Adding New Benchmarks

1. Add the benchmark function to `cinderx_jit_benchmark.sh`
2. Ensure `force_compile()` is called on all hot functions
3. Verify `cinderjit.is_jit_compiled()` returns True
4. Run ABBA comparison and verify the JIT condition is actually faster
5. Document the benchmark and expected results in this file

---

*Last updated: 2026-02-24. 40 specialisation files (750 tests) + 79 upstream files (3037+ tests) = ~3787+ total. 4 benchmark scripts.*
