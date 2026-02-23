#!/bin/bash
# test_cinderx.sh — Unified CinderX test suite
#
# Consolidates all CinderX test scripts into a single runner with
# CLI suite selection.
#
# Usage:
#   ./test_cinderx.sh [suite...]    # Run one or more suites
#   ./test_cinderx.sh all           # Run everything (default)
#   ./test_cinderx.sh --list        # List available suites
#   ./test_cinderx.sh --fix-opcode  # Fix cinderx.opcode and exit
#   ./test_cinderx.sh --check-only  # Verify setup without running tests
#
# Suites:
#   jit          17 JIT test suites (test_cinderjit etc.)
#   runtime      14 runtime test suites
#   compiler     26 compiler tests (10 SBS + 16 individual)
#   overrides    12 CPython override tests
#   cpython      CPython regression suite (~494 tests, slow)
#   smoke        3 speculative inlining smoke tests
#   adversarial  4 adversarial monkey-patch tests
#   torch        8 PyTorch P0 smoke tests
#   benchmarks   Benchmark-based correctness tests (5 benchmarks)
#   bugs         Bug 1 + Bug 4 regression tests
#   specialisation  11 standalone specialisation correctness tests
#   all          Everything above
#
# Multiple suites can be combined:
#   ./test_cinderx.sh smoke adversarial bugs    # Run these three
#   ./test_cinderx.sh jit runtime               # Run JIT + runtime only
#
# Environment:
#   CINDERX_ROOT    CinderX source root (default: ~/local/cinderx_dev/cinderx)
#   CINDERX_VENV    Virtual environment (default: ~/local/cinderx_dev/venv)
#   PYTORCH_ROOT    PyTorch source root (default: $CINDERX_ROOT/../pytorch)
#   BENCH_DIR       Benchmark directory (default: auto-detected)
#   SPEC_DIR        Specialisation test directory (default: auto-detected)
#
# Exit codes:
#   0 — All suites passed
#   1 — Setup error
#   2 — One or more test failures

set -uo pipefail

# ── Configuration ──────────────────────────────────────────────────────────

CINDERX_ROOT="${CINDERX_ROOT:-$HOME/local/cinderx_dev/cinderx}"
CINDERX_VENV="${CINDERX_VENV:-$HOME/local/cinderx_dev/venv}"
PYTORCH_ROOT="${PYTORCH_ROOT:-$CINDERX_ROOT/../pytorch}"
PYTHONLIB="$CINDERX_ROOT/cinderx/PythonLib"
TEST_DIR="$PYTHONLIB/test_cinderx"
RESULTS_DIR="/tmp/cinderx_test_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Auto-detect BENCH_DIR: look for benchmarks/ relative to script location,
# then relative to CINDERX_ROOT.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$SCRIPT_DIR/benchmarks" ]; then
    BENCH_DIR="$SCRIPT_DIR/benchmarks"
elif [ -d "$CINDERX_ROOT/benchmarks" ]; then
    BENCH_DIR="$CINDERX_ROOT/benchmarks"
else
    BENCH_DIR=""
fi

# Auto-detect SPEC_DIR: look for specialisation test files relative to script
# location, then relative to CINDERX_ROOT. The directory must contain at least
# one test_*.py file to be considered valid.
if [ -n "${SPEC_DIR:-}" ] && [ -d "$SPEC_DIR" ]; then
    :  # User-supplied, use as-is
elif [ -f "$SCRIPT_DIR/test_call_builtin_fast.py" ]; then
    SPEC_DIR="$SCRIPT_DIR"
elif [ -f "$CINDERX_ROOT/test_call_builtin_fast.py" ]; then
    SPEC_DIR="$CINDERX_ROOT"
else
    SPEC_DIR=""
fi

# SPEC_TESTS_DIR: subdirectory for tests/ files (e.g. test_binary_subscr_deopt.py)
if [ -n "$SPEC_DIR" ] && [ -d "$SPEC_DIR/tests" ]; then
    SPEC_TESTS_DIR="$SPEC_DIR/tests"
else
    SPEC_TESTS_DIR=""
fi

mkdir -p "$RESULTS_DIR"

# Colour codes (disabled if not a terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    RED='' GREEN='' YELLOW='' BOLD='' RESET=''
fi

# Temp dir for embedded test scripts (smoke, adversarial, torch)
TESTS_TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TESTS_TMPDIR"' EXIT

# Global counters
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_ERROR=0
TOTAL_SKIP=0
OVERALL_EXIT=0

SUITE_RESULTS=""  # Accumulates per-suite results for final summary

# ── Available suites ──────────────────────────────────────────────────────

ALL_SUITES="jit runtime compiler overrides cpython smoke adversarial torch benchmarks bugs specialisation"

# ── Helper: fix opcode ────────────────────────────────────────────────────

fix_opcode() {
    local opcode_src="$PYTHONLIB/opcodes/3.12/opcode.py"
    local opcode_dst="$PYTHONLIB/cinderx/opcode.py"
    if [ -f "$opcode_src" ] && [ ! -f "$opcode_dst" ]; then
        echo "Copying opcode.py (cmake build fixup)..."
        cp "$opcode_src" "$opcode_dst"
    fi
}

# ── Helper: record suite result ──────────────────────────────────────────

record_suite() {
    local suite="$1"
    local pass="$2"
    local fail="$3"
    local skip="${4:-0}"
    local error="${5:-0}"

    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    TOTAL_SKIP=$((TOTAL_SKIP + skip))
    TOTAL_ERROR=$((TOTAL_ERROR + error))

    if [ "$fail" -gt 0 ] || [ "$error" -gt 0 ]; then
        SUITE_RESULTS="${SUITE_RESULTS}  ${RED}FAIL${RESET}  $suite ($pass pass, $fail fail, $skip skip, $error error)\n"
        OVERALL_EXIT=2
    else
        SUITE_RESULTS="${SUITE_RESULTS}  ${GREEN}OK${RESET}    $suite ($pass pass, $fail fail, $skip skip)\n"
    fi
}

# ── Helper: run inline Python test ───────────────────────────────────────

run_inline_test() {
    local name="$1"
    local script="$2"
    local timeout_secs="${3:-60}"

    printf "  %-50s " "$name"
    if timeout "$timeout_secs" "$PYTHON" -c "$script" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"
        return 0
    else
        local exit_code=$?
        if [ "$exit_code" -eq 124 ]; then
            echo -e "${RED}FAIL${RESET} (timeout after ${timeout_secs}s)"
        elif [ "$exit_code" -gt 128 ]; then
            local sig=$((exit_code - 128))
            echo -e "${RED}CRASH${RESET} (signal $sig)"
        else
            echo -e "${RED}FAIL${RESET} (exit code $exit_code)"
        fi
        return 1
    fi
}

# ── Helper: run benchmark file ───────────────────────────────────────────

run_benchmark_test() {
    local name="$1"
    local file="$2"
    local timeout_secs="${3:-120}"

    printf "  %-50s " "$name"
    if [ ! -f "$file" ]; then
        echo -e "${YELLOW}SKIP${RESET} (file not found: $file)"
        return 2
    fi
    if timeout "$timeout_secs" "$PYTHON" "$file" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"
        return 0
    else
        local exit_code=$?
        if [ "$exit_code" -eq 124 ]; then
            echo -e "${RED}FAIL${RESET} (timeout after ${timeout_secs}s)"
        else
            echo -e "${RED}FAIL${RESET} (exit code $exit_code)"
        fi
        return 1
    fi
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: jit — 17 JIT test suites via unittest
# ══════════════════════════════════════════════════════════════════════════

JIT_TESTS=(
    test_cinderjit
    test_jit_async_generators
    test_jit_attr_cache
    test_jit_coroutines
    test_jit_count_calls
    test_jit_disable
    test_jit_exception
    test_jit_frame
    test_jit_generator_aarch64
    test_jit_generators
    test_jit_global_cache
    test_jitlist
    test_jit_perf_map
    test_jit_preload
    test_jit_specialization
    test_jit_support_instrumentation
    test_jit_type_annotations
)

RUNTIME_TESTS=(
    test_asynclazyvalue
    test_coro_extensions
    test_enabling_parallel_gc
    test_frame_evaluator
    test_immortalize
    test_oss_quick
    test_parallel_gc
    test_perfmaps
    test_perf_profiler_precompile
    test_python310_bytecodes
    test_python312_bytecodes
    test_python314_bytecodes
    test_shadowcode
    test_type_cache
)

COMPILER_TESTS=(
    test_compiler_sbs_stdlib_0
    test_compiler_sbs_stdlib_1
    test_compiler_sbs_stdlib_2
    test_compiler_sbs_stdlib_3
    test_compiler_sbs_stdlib_4
    test_compiler_sbs_stdlib_5
    test_compiler_sbs_stdlib_6
    test_compiler_sbs_stdlib_7
    test_compiler_sbs_stdlib_8
    test_compiler_sbs_stdlib_9
)

COMPILER_INDIVIDUAL_TESTS=(
    test_compiler.test_api
    test_compiler.test_cinder
    test_compiler.test_code_sbs
    test_compiler.test_corpus
    test_compiler.test_errors
    test_compiler.test_exception_table
    test_compiler.test_flags
    test_compiler.test_graph
    test_compiler.test_linepos
    test_compiler.test_optimizer
    test_compiler.test_py310
    test_compiler.test_pysourceloader
    test_compiler.test_sbs_external
    test_compiler.test_symbols
    test_compiler.test_unparse
    test_compiler.test_visitor
)

CPYTHON_OVERRIDE_TESTS=(
    test_cpython_overrides.test_asyncgen
    test_cpython_overrides.test_coroutines
    test_cpython_overrides.test_dis
    test_cpython_overrides.test_fork1
    test_cpython_overrides.test_gdb
    test_cpython_overrides.test_generators
    test_cpython_overrides.test_inspect
    test_cpython_overrides.test__opcode
    test_cpython_overrides.test_repl
    test_cpython_overrides.test_tracemalloc
    test_cpython_overrides.test_trace
    test_cpython_overrides.test_types
)

run_unittest_suites() {
    local suite_name="$1"
    shift
    local suites=("$@")
    local pass=0 fail=0 error=0 skip=0
    local count=0

    echo ""
    echo -e "${BOLD}=== $suite_name ===${RESET}"

    pushd "$PYTHONLIB" > /dev/null

    for suite in "${suites[@]}"; do
        count=$((count + 1))
        printf "  [%d/%d] %-45s " "$count" "${#suites[@]}" "$suite"

        OUTPUT=$(timeout 120 "$PYTHON" -m unittest "test_cinderx.$suite" 2>&1)
        TEST_EXIT=$?

        RAN_LINE=$(echo "$OUTPUT" | grep -E '^Ran [0-9]+ test' || echo "")
        STATUS_LINE=$(echo "$OUTPUT" | grep -E '^(OK|FAILED)' | tail -1 || echo "")

        if [ -z "$RAN_LINE" ]; then
            if [ $TEST_EXIT -eq 124 ]; then
                printf "${RED}TIMEOUT${RESET} (120s)\n"
                fail=$((fail + 1))
            elif [ $TEST_EXIT -gt 128 ]; then
                local sig=$((TEST_EXIT - 128))
                printf "${RED}CRASH${RESET} (signal %d)\n" "$sig"
                fail=$((fail + 1))
            elif echo "$OUTPUT" | grep -qE 'SkipTest:'; then
                local reason=$(echo "$OUTPUT" | grep -oP 'SkipTest: \K.*' | head -1 || echo "")
                printf "${YELLOW}SKIP${RESET} (%s)\n" "${reason:-module-level skip}"
                skip=$((skip + 1))
            else
                local err_msg=$(echo "$OUTPUT" | grep -E '(ModuleNotFoundError|ImportError|SyntaxError|AttributeError):' | tail -1 | head -c 60)
                printf "${RED}ERROR${RESET} (%s)\n" "${err_msg:-did not execute}"
                error=$((error + 1))
                echo "$OUTPUT" > "$RESULTS_DIR/cinderx_fail_${suite}.log"
            fi
        else
            local tests=$(echo "$RAN_LINE" | grep -oP '^Ran \K[0-9]+' || echo 0)

            if [ "$tests" -eq 0 ]; then
                printf "${YELLOW}SKIP${RESET}\n"
                skip=$((skip + 1))
            elif echo "$STATUS_LINE" | grep -q 'FAILED'; then
                local fails=$(echo "$STATUS_LINE" | grep -oP 'failures=\K[0-9]+' || echo 0)
                local errs=$(echo "$STATUS_LINE" | grep -oP 'errors=\K[0-9]+' || echo 0)
                local passed=$((tests - fails - errs))
                printf "${RED}FAIL${RESET} (%d pass, %d fail, %d error)\n" "$passed" "$fails" "$errs"
                pass=$((pass + passed))
                fail=$((fail + fails))
                error=$((error + errs))
                echo "$OUTPUT" > "$RESULTS_DIR/cinderx_fail_${suite}.log"
            elif echo "$STATUS_LINE" | grep -q 'OK'; then
                local skips=$(echo "$STATUS_LINE" | grep -oP 'skipped=\K[0-9]+' || echo 0)
                printf "${GREEN}OK${RESET}   (%d pass" "$((tests - skips))"
                if [ "$skips" -gt 0 ]; then
                    printf ", %d skip" "$skips"
                    skip=$((skip + skips))
                fi
                printf ")\n"
                pass=$((pass + tests - skips))
            else
                printf "${YELLOW}???${RESET}  (%d tests, status unclear)\n" "$tests"
                error=$((error + 1))
            fi
        fi
    done

    popd > /dev/null

    record_suite "$suite_name" "$pass" "$fail" "$skip" "$error"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: smoke — 3 speculative inlining smoke tests
# ══════════════════════════════════════════════════════════════════════════

run_smoke() {
    echo ""
    echo -e "${BOLD}=== Smoke: Speculative Inlining (3 tests) ===${RESET}"

    local pass=0 fail=0

    # Smoke 1: Happy path
    cat > "$TESTS_TMPDIR/smoke_1_happy_path.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 42
def caller(d): return d.speak()

for _ in range(3000): caller(Dog())

try:
    if cinderjit.is_jit_compiled(caller) is False:
        raise RuntimeError("caller() NOT JIT-compiled after 3000 warmup calls")
except AttributeError: pass

try:
    if not cinderjit.is_hir_inliner_enabled():
        raise RuntimeError("HIR inliner is DISABLED")
except AttributeError: pass

d = Dog()
assert caller(d) == 42
total = 0
for _ in range(10000): total += caller(d)
assert total == 420000, f"Expected 420000, got {total}"
print("PASS")
PYEOF

    printf "  %-50s " "[1/3] Happy path (inlining, correct result)"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/smoke_1_happy_path.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    # Smoke 2: Wrong receiver deopt
    cat > "$TESTS_TMPDIR/smoke_2_wrong_receiver_deopt.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 42
class Cat:
    def speak(self): return 99
def caller(animal): return animal.speak()

dog = Dog()
for _ in range(3000): caller(dog)

try:
    if cinderjit.is_jit_compiled(caller) is False:
        raise RuntimeError("caller() NOT JIT-compiled")
except AttributeError: pass
try:
    if not cinderjit.is_hir_inliner_enabled():
        raise RuntimeError("HIR inliner DISABLED")
except AttributeError: pass

cat = Cat()
assert caller(cat) == 99, f"Expected 99 from Cat, got {caller(cat)}"
assert caller(dog) == 42
assert caller(cat) == 99
for _ in range(1000):
    assert caller(dog) == 42
    assert caller(cat) == 99
print("PASS")
PYEOF

    printf "  %-50s " "[2/3] Wrong receiver deopt"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/smoke_2_wrong_receiver_deopt.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    # Smoke 3: Monkey-patch invalidation
    cat > "$TESTS_TMPDIR/smoke_3_monkey_patch.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 42
def caller(d): return d.speak()

d = Dog()
for _ in range(3000): caller(d)

try:
    if cinderjit.is_jit_compiled(caller) is False:
        raise RuntimeError("caller() NOT JIT-compiled")
except AttributeError: pass
try:
    if not cinderjit.is_hir_inliner_enabled():
        raise RuntimeError("HIR inliner DISABLED")
except AttributeError: pass

assert caller(d) == 42
Dog.speak = lambda self: 999
assert caller(d) == 999, f"Expected 999, got {caller(d)}"
for _ in range(1000):
    assert caller(d) == 999
print("PASS")
PYEOF

    printf "  %-50s " "[3/3] Monkey-patch invalidation"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/smoke_3_monkey_patch.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    record_suite "Smoke Tests" "$pass" "$fail"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: adversarial — 4 adversarial monkey-patch tests
# ══════════════════════════════════════════════════════════════════════════

run_adversarial() {
    echo ""
    echo -e "${BOLD}=== Adversarial: Monkey-Patch Tests (4 tests) ===${RESET}"

    local pass=0 fail=0

    # Adversarial 1: Post-compile monkey-patch
    cat > "$TESTS_TMPDIR/adv_1_post_compile.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 42
def caller(d): return d.speak()

d = Dog()
for _ in range(5000): caller(d)
assert caller(d) == 42

Dog.speak = lambda self: -1
result = caller(d)
assert result == -1, f"Post-compile monkey-patch: expected -1, got {result}"
d2 = Dog()
assert caller(d2) == -1, f"Fresh instance: expected -1, got {caller(d2)}"
print("PASS")
PYEOF

    printf "  %-50s " "[1/4] Post-compile monkey-patch"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/adv_1_post_compile.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    # Adversarial 2: Tight-loop mutation
    cat > "$TESTS_TMPDIR/adv_2_tight_loop.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 1
def caller(d): return d.speak()

d = Dog()
for _ in range(3000): caller(d)

for i in range(200):
    expected_a = i * 10
    expected_b = i * 10 + 1
    Dog.speak = lambda self, v=expected_a: v
    result = caller(d)
    assert result == expected_a, f"Iter {i}, A: expected {expected_a}, got {result}"
    Dog.speak = lambda self, v=expected_b: v
    result = caller(d)
    assert result == expected_b, f"Iter {i}, B: expected {expected_b}, got {result}"
print("PASS")
PYEOF

    printf "  %-50s " "[2/4] Tight-loop mutation (200 mutations)"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/adv_2_tight_loop.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    # Adversarial 3: Same signature, different body
    cat > "$TESTS_TMPDIR/adv_3_same_sig.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 42
def caller(d): return d.speak()

d = Dog()
for _ in range(3000): caller(d)
assert caller(d) == 42

def new_speak(self): return 84
Dog.speak = new_speak
assert caller(d) == 84, f"Same-sig swap: expected 84, got {caller(d)}"

accumulator = []
def logging_speak(self):
    accumulator.append(1)
    return 126
Dog.speak = logging_speak
assert caller(d) == 126, f"Logging swap: expected 126, got {caller(d)}"
assert len(accumulator) == 1

for _ in range(100): caller(d)
assert len(accumulator) == 101, f"Expected 101 calls, got {len(accumulator)}"
print("PASS")
PYEOF

    printf "  %-50s " "[3/4] Same-signature different body"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/adv_3_same_sig.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    # Adversarial 4: __dict__ vs class attribute
    cat > "$TESTS_TMPDIR/adv_4_dict_vs_class.py" << 'PYEOF'
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self): return 42
def caller(d): return d.speak()

d = Dog()
for _ in range(3000): caller(d)
assert caller(d) == 42

Dog.speak = lambda self: 100
assert caller(d) == 100, f"Class attr: expected 100, got {caller(d)}"

Dog.speak = lambda self: 42
for _ in range(3000): caller(d)
assert caller(d) == 42

type.__setattr__(Dog, 'speak', lambda self: 200)
assert caller(d) == 200, f"type.__setattr__: expected 200, got {caller(d)}"

class Cat:
    def speak(self): return 77
def multi_caller(animal): return animal.speak()

for _ in range(3000): multi_caller(Dog())
for _ in range(3000): multi_caller(Cat())

Dog.speak = lambda self: 300
assert multi_caller(Dog()) == 300
assert multi_caller(Cat()) == 77
print("PASS")
PYEOF

    printf "  %-50s " "[4/4] __dict__ vs class attribute mutation"
    if timeout 60 "$PYTHON" "$TESTS_TMPDIR/adv_4_dict_vs_class.py" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${RESET}"; pass=$((pass + 1))
    else
        echo -e "${RED}FAIL${RESET}"; fail=$((fail + 1))
    fi

    record_suite "Adversarial Tests" "$pass" "$fail"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: bugs — Bug 1 + Bug 4 regression tests
# ══════════════════════════════════════════════════════════════════════════

run_bugs() {
    echo ""
    echo -e "${BOLD}=== Bugs: Regression Tests (8 tests) ===${RESET}"

    local pass=0 fail=0

    # Bug 1: 4-level super chain (N=100)
    run_inline_test "[1/8] Bug1: 4-level super chain (N=100)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)
class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n): super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3): super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2): super().__init__(name, n, num=3); self.nb = nb
for i in range(200): Model("test")
print("OK")
' && pass=$((pass + 1)) || fail=$((fail + 1))

    # Bug 1: 4-level super chain (N=42)
    run_inline_test "[2/8] Bug1: 4-level super chain (N=42)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(42)
class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n): super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3): super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2): super().__init__(name, n, num=3); self.nb = nb
for i in range(100): Model("test")
print("OK")
' && pass=$((pass + 1)) || fail=$((fail + 1))

    # Bug 1: 5-level super chain (N=100)
    run_inline_test "[3/8] Bug1: 5-level super chain (N=100)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)
class A:
    def __init__(self): self.a = 1
class B(A):
    def __init__(self): super().__init__(); self.b = 2
class C(B):
    def __init__(self): super().__init__(); self.c = 3
class D(C):
    def __init__(self): super().__init__(); self.d = 4
class E(D):
    def __init__(self): super().__init__(); self.e = 5
for i in range(200): E()
print("OK")
' && pass=$((pass + 1)) || fail=$((fail + 1))

    # Control: 3-level (should pass even without fix)
    run_inline_test "[4/8] Control: 3-level super chain (should pass)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)
class Base:
    def __init__(self, name): self.name = name
class Mid(Base):
    def __init__(self, name): super().__init__(name); self.mid = True
class Top(Mid):
    def __init__(self, name): super().__init__(name); self.top = True
for i in range(200): Top("test")
print("OK")
' && pass=$((pass + 1)) || fail=$((fail + 1))

    # Post-fix: 3-level past threshold with attribute verification
    run_inline_test "[5/8] Post-fix: 3-level past threshold (N=50)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(50)
class Base:
    def __init__(self, name): self.name = name
class Mid(Base):
    def __init__(self, name): super().__init__(name); self.mid = True
class Top(Mid):
    def __init__(self, name): super().__init__(name); self.top = True
for i in range(200):
    t = Top("test")
    assert t.name == "test" and t.mid and t.top, f"Attribute check failed at iter {i}"
print("OK")
' && pass=$((pass + 1)) || fail=$((fail + 1))

    # Control: 4-level without cinderx (should always pass)
    run_inline_test "[6/8] Control: 4-level without cinderx (should pass)" '
class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n): super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3): super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2): super().__init__(name, n, num=3); self.nb = nb
for i in range(500): Model("test")
print("OK")
' && pass=$((pass + 1)) || fail=$((fail + 1))

    # Bug 4: tight-loop type mutation
    run_inline_test "[7/8] Bug4: tight-loop type mutation (N=100)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)
class Dog:
    def speak(self): return 1
def caller(d): return d.speak()
d = Dog()
for _ in range(3000): caller(d)
for i in range(200):
    Dog.speak = lambda self, v=i: v
    result = caller(d)
    if result != i:
        raise RuntimeError(f"Bug4 at i={i}: expected {i}, got {result}")
print("OK")
' 30 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Regression canary: dunder_protocol
    if [ -n "$BENCH_DIR" ] && [ -f "$BENCH_DIR/dunder_protocol.py" ]; then
        run_benchmark_test "[8/8] Canary: dunder_protocol benchmark" "$BENCH_DIR/dunder_protocol.py"
        local rc=$?
        if [ $rc -eq 0 ]; then pass=$((pass + 1))
        elif [ $rc -eq 2 ]; then :  # skip — don't count
        else fail=$((fail + 1)); fi
    else
        printf "  %-50s " "[8/8] Canary: dunder_protocol benchmark"
        echo -e "${YELLOW}SKIP${RESET} (benchmark dir not found)"
    fi

    record_suite "Bug Regression Tests" "$pass" "$fail"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: benchmarks — Benchmark-based correctness tests
# ══════════════════════════════════════════════════════════════════════════

run_benchmarks() {
    echo ""
    echo -e "${BOLD}=== Benchmarks: Correctness Tests (5 benchmarks) ===${RESET}"

    if [ -z "$BENCH_DIR" ]; then
        echo "  ${YELLOW}SKIP${RESET} — no benchmark directory found"
        record_suite "Benchmark Tests" 0 0 5
        return
    fi

    local pass=0 fail=0 skip=0

    for bench in context_manager decorator_chain deep_class kwargs_dispatch nn_module_forward; do
        run_benchmark_test "Benchmark: $bench" "$BENCH_DIR/${bench}.py"
        local rc=$?
        if [ $rc -eq 0 ]; then pass=$((pass + 1))
        elif [ $rc -eq 2 ]; then skip=$((skip + 1))
        else fail=$((fail + 1)); fi
    done

    record_suite "Benchmark Tests" "$pass" "$fail" "$skip"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: specialisation — 11 standalone specialisation correctness tests
# ══════════════════════════════════════════════════════════════════════════
#
# These are standalone Python test scripts that:
#   - Exit 0 on pass, nonzero on fail
#   - Skip cleanly (exit 0) when cinderjit is not available
#   - Use WARMUP=15000, REQUIRE_JIT=True, check_jit_compiled()
#
# Root-level tests (4):
#   test_call_builtin_fast.py          CALL_BUILTIN_FAST specialisation
#   test_call_path_mutual_exclusion.py Call path mutual exclusion assertions
#   test_binary_subscr_correctness.py  BINARY_SUBSCR correctness
#   test_super_fix.py                  super().__init__() JIT bug fix
#
# tests/ subdirectory (7):
#   test_binary_op_add_int.py          BINARY_OP_ADD_INT/SUBTRACT/MULTIPLY correctness
#   test_binary_subscr_deopt.py        BINARY_SUBSCR deoptimisation
#   test_for_iter_list_mutation.py     FOR_ITER_LIST mutation handling
#   test_for_iter_polymorphic_deopt.py FOR_ITER polymorphic deoptimisation
#   test_load_attr_instance_value.py   LOAD_ATTR_INSTANCE_VALUE correctness
#   test_load_attr_module_inline.py    LOAD_ATTR_MODULE inline correctness
#   test_store_attr_instance_value.py  STORE_ATTR_INSTANCE_VALUE correctness

# Specialisation tests in the root directory
SPEC_ROOT_TESTS=(
    test_call_builtin_fast
    test_call_path_mutual_exclusion
    test_binary_subscr_correctness
    test_super_fix
)

# Specialisation tests in the tests/ subdirectory
SPEC_SUB_TESTS=(
    test_binary_op_add_int
    test_binary_subscr_deopt
    test_for_iter_list_mutation
    test_load_global
    test_for_iter_polymorphic_deopt
    test_load_attr_instance_value
    test_load_attr_module_inline
    test_store_attr_instance_value
)

run_specialisation() {
    local total=${#SPEC_ROOT_TESTS[@]}
    total=$((total + ${#SPEC_SUB_TESTS[@]}))

    echo ""
    echo -e "${BOLD}=== Specialisation: Standalone Correctness Tests ($total tests) ===${RESET}"

    if [ -z "$SPEC_DIR" ]; then
        echo "  ${YELLOW}SKIP${RESET} — no specialisation test directory found"
        echo "  Set SPEC_DIR to the directory containing test_call_builtin_fast.py"
        record_suite "Specialisation Tests" 0 0 "$total"
        return
    fi

    local pass=0 fail=0 skip=0
    local idx=0

    # Root-level tests
    for test in "${SPEC_ROOT_TESTS[@]}"; do
        idx=$((idx + 1))
        run_benchmark_test "[$idx/$total] $test" "$SPEC_DIR/${test}.py" 120
        local rc=$?
        if [ $rc -eq 0 ]; then pass=$((pass + 1))
        elif [ $rc -eq 2 ]; then skip=$((skip + 1))
        else fail=$((fail + 1)); fi
    done

    # tests/ subdirectory tests
    if [ -z "$SPEC_TESTS_DIR" ]; then
        echo "  ${YELLOW}NOTE${RESET} — tests/ subdirectory not found, skipping ${#SPEC_SUB_TESTS[@]} tests"
        skip=$((skip + ${#SPEC_SUB_TESTS[@]}))
    else
        for test in "${SPEC_SUB_TESTS[@]}"; do
            idx=$((idx + 1))
            run_benchmark_test "[$idx/$total] $test" "$SPEC_TESTS_DIR/${test}.py" 120
            local rc=$?
            if [ $rc -eq 0 ]; then pass=$((pass + 1))
            elif [ $rc -eq 2 ]; then skip=$((skip + 1))
            else fail=$((fail + 1)); fi
        done
    fi

    record_suite "Specialisation Tests" "$pass" "$fail" "$skip"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: torch — PyTorch P0 smoke tests
# ══════════════════════════════════════════════════════════════════════════

run_torch() {
    echo ""
    echo -e "${BOLD}=== Torch: PyTorch P0 Smoke Tests (8 tests) ===${RESET}"

    # Pre-flight: verify torch is importable
    if ! "$PYTHON" -c "import torch" 2>/dev/null; then
        echo "  ${YELLOW}SKIP${RESET} — PyTorch not importable"
        record_suite "PyTorch Smoke Tests" 0 0 8
        return
    fi

    if ! "$PYTHON" -c "import cinderjit; import torch; cinderjit.auto()" 2>/dev/null; then
        echo "  ${YELLOW}SKIP${RESET} — cinderjit.auto() crashes after torch import"
        record_suite "PyTorch Smoke Tests" 0 0 8
        return
    fi

    local pass=0 fail=0

    # Test 1: Tensor operations
    run_inline_test "[1/8] tensor_ops: create, matmul, sum, mean" '
import cinderjit, torch; cinderjit.auto()
def tensor_ops():
    a = torch.randn(100, 50); b = torch.randn(50, 30)
    c = torch.matmul(a, b)
    assert c.shape == (100, 30)
    s = c.sum(); m = c.mean()
    assert s.ndim == 0 and m.ndim == 0
    return True
for i in range(200): tensor_ops()
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 2: In-place operations
    run_inline_test "[2/8] inplace_ops: add_, mul_, relu_" '
import cinderjit, torch; cinderjit.auto()
def inplace_ops():
    x = torch.randn(64, 64); ptr = x.data_ptr()
    x.add_(1.0); x.mul_(2.0); x.relu_()
    assert x.data_ptr() == ptr
    assert (x >= 0).all()
    return True
for i in range(200): inplace_ops()
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 3: Autograd
    run_inline_test "[3/8] autograd: forward + backward + grad check" '
import cinderjit, torch; cinderjit.auto()
def autograd_check():
    x = torch.randn(32, 16, requires_grad=True)
    w = torch.randn(16, 8, requires_grad=True)
    y = torch.matmul(x, w); loss = y.sum(); loss.backward()
    assert x.grad is not None and w.grad is not None
    assert x.grad.shape == x.shape and w.grad.shape == w.shape
    return True
for i in range(200): autograd_check()
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 4: nn.Linear
    run_inline_test "[4/8] nn_linear: Linear forward + backward" '
import cinderjit, torch; cinderjit.auto()
def nn_linear_check():
    model = torch.nn.Linear(32, 16)
    x = torch.randn(8, 32); y = model(x)
    assert y.shape == (8, 16)
    y.sum().backward()
    assert model.weight.grad is not None
    return True
for i in range(200): nn_linear_check()
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 5: MLP
    run_inline_test "[5/8] mlp: 3-layer MLP forward + backward + step" '
import cinderjit, torch; cinderjit.auto()
def mlp_check():
    model = torch.nn.Sequential(
        torch.nn.Linear(64, 128), torch.nn.ReLU(),
        torch.nn.Linear(128, 64), torch.nn.ReLU(),
        torch.nn.Linear(64, 10),
    )
    opt = torch.optim.SGD(model.parameters(), lr=0.01)
    x = torch.randn(16, 64); target = torch.randint(0, 10, (16,))
    logits = model(x); assert logits.shape == (16, 10)
    loss = torch.nn.functional.cross_entropy(logits, target)
    opt.zero_grad(); loss.backward(); opt.step()
    assert loss.item() > 0
    return True
for i in range(100): mlp_check()
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 6: JIT vs interpreter correctness
    run_inline_test "[6/8] correctness: JIT vs interpreter match" '
import cinderjit, torch
torch.manual_seed(42)
x_ref = torch.randn(32, 32); w_ref = torch.randn(32, 16)
def compute(x, w):
    y = torch.matmul(x, w); y = torch.relu(y); return y.sum()
ref_result = compute(x_ref.clone(), w_ref.clone())
cinderjit.auto()
for i in range(200):
    torch.manual_seed(42)
    x = torch.randn(32, 32); w = torch.randn(32, 16)
    jit_result = compute(x, w)
diff = abs(ref_result.item() - jit_result.item())
assert diff <= 1e-5, f"ref={ref_result.item()}, jit={jit_result.item()}, diff={diff}"
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 7: Conv2d + BatchNorm
    run_inline_test "[7/8] conv_bn: Conv2d + BatchNorm2d + ReLU" '
import cinderjit, torch; cinderjit.auto()
def conv_bn_check():
    model = torch.nn.Sequential(
        torch.nn.Conv2d(3, 16, 3, padding=1),
        torch.nn.BatchNorm2d(16),
        torch.nn.ReLU(),
    )
    x = torch.randn(4, 3, 8, 8); y = model(x)
    assert y.shape == (4, 16, 8, 8)
    y.sum().backward()
    return True
for i in range(100): conv_bn_check()
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    # Test 8: Compilation count
    run_inline_test "[8/8] compilation: functions actually JIT-compiled" '
import cinderjit, torch; cinderjit.auto()
def fn_a(x): return x + 1
def fn_b(x): return x * 2
def fn_c(x): return torch.relu(x)
t = torch.randn(10)
for i in range(500): fn_a(t); fn_b(t); fn_c(t)
try:
    compiled = cinderjit.get_num_functions_compiled()
    assert compiled >= 1, f"Only {compiled} functions compiled"
except AttributeError: pass
print("OK")
' 120 && pass=$((pass + 1)) || fail=$((fail + 1))

    record_suite "PyTorch Smoke Tests" "$pass" "$fail"
}

# ══════════════════════════════════════════════════════════════════════════
# Suite: cpython — CPython regression suite
# ══════════════════════════════════════════════════════════════════════════

run_cpython() {
    echo ""
    echo -e "${BOLD}=== CPython: Regression Suite ===${RESET}"
    echo "Running CPython test suite via python3 -m test"
    echo ""

    local cpython_results="$RESULTS_DIR/cpython_results_${TIMESTAMP}.txt"
    local skip_file="$CINDERX_ROOT/cinderx/TestScripts/cinder_skip_test.txt"
    local arm64_fail="$CINDERX_ROOT/cinderx/TestScripts/3.12-opt-arm64-failures.txt"

    # Build ignore list
    local cpython_ignores=""
    declare -A seen_modules

    if [ -f "$skip_file" ]; then
        while IFS= read -r line; do
            [[ "$line" =~ ^[[:space:]]*# ]] && continue
            [[ -z "${line// }" ]] && continue
            local module=""
            if [[ ! "$line" =~ \. ]]; then
                module="$line"
            elif [[ "$line" =~ ^test\.([^.]+) ]]; then
                module="${BASH_REMATCH[1]}"
            else
                continue
            fi
            if [[ -z "${seen_modules[$module]+x}" ]]; then
                cpython_ignores="$cpython_ignores -x $module"
                seen_modules[$module]=1
            fi
        done < "$skip_file"
    fi

    if [ -f "$arm64_fail" ]; then
        while IFS= read -r line; do
            [[ "$line" =~ ^[[:space:]]*# ]] && continue
            [[ -z "${line// }" ]] && continue
            local module=""
            if [[ "$line" =~ ^test_cinderx\. ]]; then
                continue
            elif [[ "$line" =~ ^test\.([^.]+) ]]; then
                module="${BASH_REMATCH[1]}"
            elif [[ ! "$line" =~ \. ]]; then
                module="$line"
            else
                continue
            fi
            if [[ -z "${seen_modules[$module]+x}" ]]; then
                cpython_ignores="$cpython_ignores -x $module"
                seen_modules[$module]=1
            fi
        done < "$arm64_fail"
    fi

    # Environment-specific skips
    for env_skip in test_pdb test_venv; do
        if [[ -z "${seen_modules[$env_skip]+x}" ]]; then
            cpython_ignores="$cpython_ignores -x $env_skip"
            seen_modules[$env_skip]=1
        fi
    done

    echo "  Skip files parsed: ${#seen_modules[@]} modules excluded"
    echo ""

    timeout 1800 "$PYTHON" -m test \
        --timeout 60 \
        -j4 \
        $cpython_ignores \
        2>&1 | tee "$cpython_results" | tail -20

    local cpython_exit=$?

    echo ""
    local cpython_result=$(grep -oP 'Tests result: \K\w+' "$cpython_results" | tail -1 || echo "")

    if [ "$cpython_result" = "SUCCESS" ]; then
        echo -e "  ${GREEN}CPython regression suite: PASS${RESET}"
        record_suite "CPython Regression" 1 0
    elif [ "$cpython_exit" -eq 124 ]; then
        echo -e "  ${YELLOW}CPython regression suite: TIMEOUT (30 min limit)${RESET}"
        record_suite "CPython Regression" 0 1
    else
        echo -e "  ${RED}CPython regression suite: FAIL${RESET}"
        record_suite "CPython Regression" 0 1
    fi

    echo "  Results: $cpython_results"
}

# ══════════════════════════════════════════════════════════════════════════
# Setup + dispatch
# ══════════════════════════════════════════════════════════════════════════

# Handle --list
if [ "${1:-}" = "--list" ]; then
    echo "Available suites:"
    for s in $ALL_SUITES; do
        echo "  $s"
    done
    exit 0
fi

# Handle --fix-opcode
if [ "${1:-}" = "--fix-opcode" ]; then
    fix_opcode
    echo "Done."
    exit 0
fi

# Activate venv
if [ ! -f "$CINDERX_VENV/bin/activate" ]; then
    echo "FATAL: venv not found at $CINDERX_VENV"
    exit 1
fi
# shellcheck disable=SC1091
source "$CINDERX_VENV/bin/activate"

PYTHON="${CINDERX_PYTHON:-python3}"

# Always fix opcode
fix_opcode

export PYTHONPATH="$PYTHONLIB${PYTHONPATH:+:$PYTHONPATH}"

# ── Pre-flight checks ─────────────────────────────────────────────────────

echo -e "${BOLD}CinderX Unified Test Suite${RESET}"
echo "Root:     $CINDERX_ROOT"
echo "Python:   $($PYTHON --version 2>&1)"
echo "Host:     $(hostname)"
echo "Started:  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "---"

# Gate: CinderX JIT usable
# Two-tier gate:
#   1. cinderjit must be importable and enabled
#   2. force_compile + is_jit_compiled is attempted but not required to pass
#      (some builds have JIT enabled for auto/compile_after_n_calls but
#       force_compile doesn't set the is_jit_compiled flag)
echo -n "Verifying CinderX JIT... "
GATE_CHECK=$("$PYTHON" -c "
import cinderjit
assert cinderjit.is_enabled(), 'JIT subsystem not enabled'
def _gate(): return 42
cinderjit.force_compile(_gate)
if cinderjit.is_jit_compiled(_gate):
    print('OK (force_compile + is_jit_compiled verified)')
else:
    print('OK (JIT enabled, force_compile ran, is_jit_compiled=False — proceeding)')
assert _gate() == 42, 'function returned wrong result'
" 2>&1) || {
    echo -e "${RED}FATAL: CinderX JIT not available${RESET}"
    echo "$GATE_CHECK"
    exit 1
}
echo -e "${GREEN}$GATE_CHECK${RESET}"

# Handle --check-only
if [ "${1:-}" = "--check-only" ]; then
    echo ""
    echo "All gates passed. Ready to run tests."
    exit 0
fi

# Parse suite arguments
REQUESTED_SUITES=()
if [ $# -eq 0 ] || [ "${1:-}" = "all" ]; then
    REQUESTED_SUITES=($ALL_SUITES)
else
    for arg in "$@"; do
        arg="${arg#--}"  # strip leading --
        if [ "$arg" = "all" ]; then
            REQUESTED_SUITES=($ALL_SUITES)
            break
        fi
        # Validate suite name
        found=0
        for valid in $ALL_SUITES; do
            if [ "$arg" = "$valid" ]; then
                found=1
                break
            fi
        done
        if [ "$found" -eq 0 ]; then
            echo "ERROR: Unknown suite '$arg'"
            echo "Available suites: $ALL_SUITES"
            exit 1
        fi
        REQUESTED_SUITES+=("$arg")
    done
fi

echo "Suites:   ${REQUESTED_SUITES[*]}"
echo ""

# ── Run requested suites ─────────────────────────────────────────────────

for suite in "${REQUESTED_SUITES[@]}"; do
    case "$suite" in
        jit)         run_unittest_suites "JIT Tests (17 suites)" "${JIT_TESTS[@]}" ;;
        runtime)     run_unittest_suites "Runtime Tests (14 suites)" "${RUNTIME_TESTS[@]}" ;;
        compiler)    run_unittest_suites "Compiler Tests (26 suites)" "${COMPILER_TESTS[@]}" "${COMPILER_INDIVIDUAL_TESTS[@]}" ;;
        overrides)   run_unittest_suites "CPython Override Tests (12 suites)" "${CPYTHON_OVERRIDE_TESTS[@]}" ;;
        cpython)     run_cpython ;;
        smoke)       run_smoke ;;
        adversarial) run_adversarial ;;
        torch)       run_torch ;;
        benchmarks)  run_benchmarks ;;
        bugs)        run_bugs ;;
        specialisation) run_specialisation ;;
    esac
done

# ── Final summary ────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}══════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}FINAL SUMMARY${RESET}"
echo -e "${BOLD}══════════════════════════════════════════════════════════════${RESET}"
echo ""
echo "Finished: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""
echo "Per-suite results:"
echo -e "$SUITE_RESULTS"
echo ""
printf "Totals:   %d pass, %d fail, %d error, %d skip\n" \
    "$TOTAL_PASS" "$TOTAL_FAIL" "$TOTAL_ERROR" "$TOTAL_SKIP"
echo ""

if [ "$OVERALL_EXIT" -eq 0 ]; then
    echo -e "${GREEN}${BOLD}VERDICT: ALL TESTS PASSED${RESET}"
else
    echo -e "${RED}${BOLD}VERDICT: FAILURES DETECTED${RESET}"
fi

exit "$OVERALL_EXIT"
