#!/bin/bash
# Regression test suite for CinderX Bug 1: super().__init__() dispatch corruption
# Runs on devgpu004 against the CinderX debug or optimised build.
#
# Usage:
#   ./tests/regression_bug1.sh [python_path]
#
# If python_path is not provided, defaults to:
#   /data/users/alexturner/cinderx_dev/cinderx/python
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -euo pipefail

PYTHON="${1:-/data/users/alexturner/cinderx_dev/cinderx/python}"
BENCH_DIR="$(cd "$(dirname "$0")/../benchmarks" && pwd)"
PASS=0
FAIL=0
ERRORS=""

if [ ! -x "$PYTHON" ]; then
    echo "ERROR: Python binary not found or not executable: $PYTHON"
    exit 1
fi

run_test() {
    local name="$1"
    local script="$2"
    local timeout_secs="${3:-60}"

    printf "%-40s " "$name..."
    if timeout "$timeout_secs" "$PYTHON" -c "$script" > /dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo "FAIL (timeout after ${timeout_secs}s)"
        else
            echo "FAIL (exit code $exit_code)"
        fi
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  - $name"
    fi
}

run_benchmark() {
    local name="$1"
    local file="$2"
    local timeout_secs="${3:-120}"

    printf "%-40s " "$name..."
    if [ ! -f "$file" ]; then
        echo "SKIP (file not found: $file)"
        return
    fi
    if timeout "$timeout_secs" "$PYTHON" "$file" > /dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo "FAIL (timeout after ${timeout_secs}s)"
        else
            echo "FAIL (exit code $exit_code)"
        fi
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  - $name"
    fi
}

echo "============================================"
echo "CinderX Bug 1 Regression Test Suite"
echo "============================================"
echo "Python: $PYTHON"
echo "Benchmarks: $BENCH_DIR"
echo "Date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo ""

# --- Phase 1: Bug 1 Minimal Reproducer (smoke test) ---
echo "--- Phase 1: Bug 1 Smoke Test ---"

run_test "Bug1: 4-level super chain (N=100)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n):
        super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3):
        super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2):
        super().__init__(name, n, num=3); self.nb = nb

for i in range(200):
    Model("test")
print("OK")
'

run_test "Bug1: 4-level super chain (N=42)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(42)

class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n):
        super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3):
        super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2):
        super().__init__(name, n, num=3); self.nb = nb

for i in range(100):
    Model("test")
print("OK")
'

run_test "Bug1: 5-level super chain (N=100)" '
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

for i in range(200):
    E()
print("OK")
'

# Negative control: 3-level should pass even without fix
run_test "Control: 3-level super chain (should pass)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(100)

class Base:
    def __init__(self, name): self.name = name
class Mid(Base):
    def __init__(self, name):
        super().__init__(name); self.mid = True
class Top(Mid):
    def __init__(self, name):
        super().__init__(name); self.top = True

for i in range(200):
    Top("test")
print("OK")
'

# Post-fix dispatch path: 3-level past threshold (verifies fix doesn't break shallow chains)
run_test "Post-fix: 3-level past threshold (N=50)" '
import cinderx; cinderx.init()
import cinderjit; cinderjit.compile_after_n_calls(50)

class Base:
    def __init__(self, name): self.name = name
class Mid(Base):
    def __init__(self, name):
        super().__init__(name); self.mid = True
class Top(Mid):
    def __init__(self, name):
        super().__init__(name); self.top = True

for i in range(200):
    t = Top("test")
    assert t.name == "test" and t.mid and t.top, f"Attribute check failed at iter {i}"
print("OK")
'

# Negative control: without cinderx.init() should pass
run_test "Control: 4-level without cinderx (should pass)" '
class Base:
    def __init__(self, name): self.name = name
class Layer(Base):
    def __init__(self, name, n):
        super().__init__(name); self.n = n
class Block(Layer):
    def __init__(self, name, n, num=3):
        super().__init__(name, n); self.num = num
class Model(Block):
    def __init__(self, name, n=32, nb=2):
        super().__init__(name, n, num=3); self.nb = nb

for i in range(500):
    Model("test")
print("OK")
'

echo ""

# --- Phase 2: Blocked Benchmarks ---
echo "--- Phase 2: Previously Blocked Benchmarks ---"

run_benchmark "Benchmark: context_manager" "$BENCH_DIR/context_manager.py"
run_benchmark "Benchmark: decorator_chain" "$BENCH_DIR/decorator_chain.py"
run_benchmark "Benchmark: deep_class" "$BENCH_DIR/deep_class.py"
run_benchmark "Benchmark: kwargs_dispatch" "$BENCH_DIR/kwargs_dispatch.py"
run_benchmark "Benchmark: nn_module_forward" "$BENCH_DIR/nn_module_forward.py"

echo ""

# --- Phase 3: Regression Canary (should always pass) ---
echo "--- Phase 3: Regression Canary ---"

run_benchmark "Benchmark: dunder_protocol (canary)" "$BENCH_DIR/dunder_protocol.py"

echo ""

# --- Phase 4: Bug 4 Shared Root Cause Check ---
echo "--- Phase 4: Bug 4 (Shared Root Cause) ---"

run_test "Bug4: tight-loop type mutation (N=100)" '
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
' 30

echo ""

# --- Summary ---
echo "============================================"
echo "SUMMARY: $PASS passed, $FAIL failed"
echo "============================================"

if [ $FAIL -gt 0 ]; then
    echo -e "Failed tests:$ERRORS"
    exit 1
else
    echo "All tests passed."
    exit 0
fi
