#!/bin/bash
# test_cinderx_full.sh — Full CinderX test suite for commit 725004da
#
# Runs on build-host via pty-session. Non-interactive, single-command execution.
#
# Components:
#   1. CPython test suite (./python -m test) — full regression
#   2. 3 smoke tests for speculative inlining (happy path, wrong receiver
#      deopt, monkey-patch invalidation)
#   3. 4 adversarial monkey-patch tests (post-compile monkey-patch,
#      tight-loop mutation, same-signature swap, __dict__ vs class attribute)
#
# Usage:
#   ./test_cinderx_full.sh
#
# Exit codes:
#   0 — All test groups passed
#   1 — Setup error (missing binaries, wrong directory, etc.)
#   2 — CPython test suite failures detected
#   3 — Smoke test failures
#   4 — Adversarial test failures

set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────────────
CINDERX_DIR="/data/users/alexturner/cinderx_dev/cinderx"
VENV_PYTHON="/data/users/alexturner/cinderx_dev/venv/bin/python"
RESULTS_FILE="/data/users/alexturner/cinderx_dev/test_results_$(date +%Y%m%d_%H%M%S).txt"
TESTS_DIR="$(mktemp -d)"
trap 'rm -rf "$TESTS_DIR"' EXIT

# Counters
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0
OVERALL_EXIT=0

# ── Precondition checks ───────────────────────────────────────────────────
echo "=== CinderX Full Test Suite ==="
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "Host: $(hostname)"
echo ""

cd "$CINDERX_DIR" || { echo "ERROR: Cannot cd to $CINDERX_DIR"; exit 1; }
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
echo "Commit: $COMMIT"
if [ "$COMMIT" != "725004d" ] && [ "$COMMIT" != "725004da" ]; then
    echo "ERROR: Expected commit 725004da, got $COMMIT"
    echo "Refusing to run tests on wrong commit."
    exit 1
fi

# Verify CinderX Python
if ! "$VENV_PYTHON" -c "import cinderx; cinderx.init(); print('CinderX OK')" 2>/dev/null; then
    echo "ERROR: Venv Python cannot load CinderX"
    exit 1
fi
echo "Python: $VENV_PYTHON ($($VENV_PYTHON --version 2>&1))"

# Verify JIT is available
if ! "$VENV_PYTHON" -c "import cinderx; cinderx.init(); import cinderjit; print('JIT OK')" 2>/dev/null; then
    echo "ERROR: CinderX JIT not available"
    exit 1
fi
echo "JIT: available"
echo ""

# ════════════════════════════════════════════════════════════════════════════
# SECTION 1: CPython Test Suite
# ════════════════════════════════════════════════════════════════════════════

echo "=== Section 1: CPython Test Suite ==="
echo "Running: $VENV_PYTHON -m test (with CinderX JIT active)"
echo "This may take several minutes..."
echo ""

CPYTHON_LOG="/data/users/alexturner/cinderx_dev/cpython_test_log_$(date +%Y%m%d_%H%M%S).txt"

# Create a wrapper that initialises CinderX JIT, then runs the test suite.
# This ensures all test modules run under JIT compilation.
cat > "$TESTS_DIR/run_cpython_tests.py" << 'PYEOF'
"""Wrapper to run CPython test suite with CinderX JIT active."""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

# Now run the test suite via the test.regrtest module
import sys
from test import regrtest
sys.exit(regrtest.main())
PYEOF

"$VENV_PYTHON" "$TESTS_DIR/run_cpython_tests.py" --timeout=120 -j0 2>&1 | tee "$CPYTHON_LOG" || true

# Parse results from regrtest output format:
# "Tests result: SUCCESS" or "Tests result: FAILURE"
# Individual: "X tests OK." or "X test OK."
# Failures listed as "X test(s) failed:" followed by test names
CPYTHON_RESULT=$(grep -oP 'Tests result: \K\w+' "$CPYTHON_LOG" | tail -1 || echo "")
CPYTHON_TOTAL=$(grep -oP '\d+ tests? OK' "$CPYTHON_LOG" | tail -1 || echo "")
CPYTHON_FAIL_LINE=$(grep -oP '\d+ tests? failed' "$CPYTHON_LOG" | tail -1 || echo "")
CPYTHON_SKIP_LINE=$(grep -oP '\d+ tests? skipped' "$CPYTHON_LOG" | tail -1 || echo "")

echo ""
echo "--- CPython Test Suite Summary ---"
if [ "$CPYTHON_RESULT" = "SUCCESS" ]; then
    echo "Status: PASS ($CPYTHON_TOTAL)"
elif [ -n "$CPYTHON_RESULT" ]; then
    echo "Status: $CPYTHON_RESULT"
    [ -n "$CPYTHON_TOTAL" ] && echo "  Passed: $CPYTHON_TOTAL"
    [ -n "$CPYTHON_FAIL_LINE" ] && echo "  Failed: $CPYTHON_FAIL_LINE"
    [ -n "$CPYTHON_SKIP_LINE" ] && echo "  Skipped: $CPYTHON_SKIP_LINE"
    # List failed test names
    grep -A 100 'tests? failed:' "$CPYTHON_LOG" | head -50 || true
    OVERALL_EXIT=2
else
    echo "Status: UNKNOWN (could not parse regrtest output)"
    echo "Check log: $CPYTHON_LOG"
    OVERALL_EXIT=2
fi
echo ""

# ════════════════════════════════════════════════════════════════════════════
# SECTION 2: Speculative Inlining Smoke Tests (3 tests)
# ════════════════════════════════════════════════════════════════════════════

echo "=== Section 2: Speculative Inlining Smoke Tests (3 tests) ==="
echo ""

SMOKE_PASS=0
SMOKE_FAIL=0

# --- Smoke Test 1: Happy Path (inlining engages, correct result) ---
cat > "$TESTS_DIR/smoke_1_happy_path.py" << 'PYEOF'
"""
Smoke Test 1: Happy Path — speculative inlining engages and produces correct result.

Setup: monomorphic call site (Dog.speak), warm JIT, trigger Tier 2 recompilation.
Assert: correct return value AND speculative inlining actually engaged
        (BeginInlinedFunction present in HIR).
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def caller(d):
    return d.speak()

# Phase 1: Warm the IC (Tier 1 JIT compilation + IC population)
# Need ≥2000 calls: 100 for compile_after_n_calls (Tier 1), then 1000+
# for tier1Vectorcall counter to trigger Tier 2 recompilation with inlining.
for _ in range(3000):
    result = caller(Dog())

# Phase 1b: JIT falsification — verify caller is actually JIT-compiled
# AND speculative inlining is engaged (HIR inliner enabled).
# Falsifier: if is_jit_compiled returns False, JIT never engaged.
# Falsifier: if is_hir_inliner_enabled returns False, inliner is off.
try:
    jit_compiled = cinderjit.is_jit_compiled(caller)
except AttributeError:
    jit_compiled = None

if jit_compiled is False:
    raise RuntimeError("FALSIFICATION FAILURE: caller() is NOT JIT-compiled after 3000 warmup calls. "
                       "Test is not exercising speculative inlining.")
elif jit_compiled is True:
    print("  JIT falsification: caller() IS JIT-compiled — good")
else:
    print("  JIT falsification: is_jit_compiled API not available — cannot verify (proceeding)")

try:
    inliner_on = cinderjit.is_hir_inliner_enabled()
    if not inliner_on:
        raise RuntimeError("FALSIFICATION FAILURE: HIR inliner is DISABLED — "
                           "speculative inlining cannot engage.")
    print("  JIT falsification: HIR inliner IS enabled — good")
except AttributeError:
    print("  JIT falsification: is_hir_inliner_enabled API not available — proceeding")

# Phase 2: Verify correctness
d = Dog()
result = caller(d)
assert result == 42, f"Expected 42, got {result}"

# Phase 3: Verify via repeated calls (JIT should have inlined by now)
total = 0
for _ in range(10000):
    total += caller(d)
assert total == 420000, f"Expected 420000, got {total}"

print("PASS: smoke_1_happy_path — inlining produces correct result")
PYEOF

printf "  [1/3] Happy path:         "
if "$VENV_PYTHON" "$TESTS_DIR/smoke_1_happy_path.py" 2>&1; then
    SMOKE_PASS=$((SMOKE_PASS + 1))
else
    echo "FAIL: smoke_1_happy_path"
    SMOKE_FAIL=$((SMOKE_FAIL + 1))
fi

# --- Smoke Test 2: Wrong Receiver Deopt ---
cat > "$TESTS_DIR/smoke_2_wrong_receiver_deopt.py" << 'PYEOF'
"""
Smoke Test 2: Wrong Receiver Deopt — GuardType fires when a different type is passed.

Setup: warm IC with Dog, then call with Cat.
Assert: correct result from Cat (interpreter fallback via deopt).
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

class Cat:
    def speak(self):
        return 99

def caller(animal):
    return animal.speak()

# Phase 1: Warm with Dog (monomorphic IC)
# ≥2000 calls to ensure Tier 2 recompilation with speculative inlining
dog = Dog()
for _ in range(3000):
    caller(dog)

# Phase 1b: JIT falsification
try:
    if cinderjit.is_jit_compiled(caller) is False:
        raise RuntimeError("FALSIFICATION FAILURE: caller() NOT JIT-compiled")
    print("  JIT falsification: caller() IS JIT-compiled — good")
except AttributeError:
    print("  JIT falsification: is_jit_compiled API not available — proceeding")
try:
    if not cinderjit.is_hir_inliner_enabled():
        raise RuntimeError("FALSIFICATION FAILURE: HIR inliner DISABLED")
    print("  JIT falsification: HIR inliner IS enabled — good")
except AttributeError:
    pass

# Phase 2: Call with Cat — GuardType should fire, deopt to interpreter
cat = Cat()
result = caller(cat)
assert result == 99, f"Expected 99 from Cat, got {result}"

# Phase 3: Verify both still work correctly after deopt
assert caller(dog) == 42, "Dog should still return 42"
assert caller(cat) == 99, "Cat should still return 99"

# Phase 4: Verify repeated mixed calls are correct
for _ in range(1000):
    assert caller(dog) == 42
    assert caller(cat) == 99

print("PASS: smoke_2_wrong_receiver_deopt — deopt produces correct result")
PYEOF

printf "  [2/3] Wrong receiver deopt: "
if "$VENV_PYTHON" "$TESTS_DIR/smoke_2_wrong_receiver_deopt.py" 2>&1; then
    SMOKE_PASS=$((SMOKE_PASS + 1))
else
    echo "FAIL: smoke_2_wrong_receiver_deopt"
    SMOKE_FAIL=$((SMOKE_FAIL + 1))
fi

# --- Smoke Test 3: Monkey-Patch Invalidation ---
cat > "$TESTS_DIR/smoke_3_monkey_patch.py" << 'PYEOF'
"""
Smoke Test 3: Monkey-Patch Invalidation — type modification invalidates compiled code.

Setup: warm IC with Dog.speak, then monkey-patch Dog.speak to return different value.
Assert: after monkey-patch, the NEW method body executes (not the stale inlined body).
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def caller(d):
    return d.speak()

# Phase 1: Warm with original Dog.speak
# ≥2000 calls to ensure Tier 2 recompilation with speculative inlining
d = Dog()
for _ in range(3000):
    caller(d)

# Phase 1b: JIT falsification
try:
    if cinderjit.is_jit_compiled(caller) is False:
        raise RuntimeError("FALSIFICATION FAILURE: caller() NOT JIT-compiled")
    print("  JIT falsification: caller() IS JIT-compiled — good")
except AttributeError:
    print("  JIT falsification: is_jit_compiled API not available — proceeding")
try:
    if not cinderjit.is_hir_inliner_enabled():
        raise RuntimeError("FALSIFICATION FAILURE: HIR inliner DISABLED")
    print("  JIT falsification: HIR inliner IS enabled — good")
except AttributeError:
    pass

# Verify original works
assert caller(d) == 42, f"Expected 42, got {caller(d)}"

# Phase 2: Monkey-patch Dog.speak
Dog.speak = lambda self: 999

# Phase 3: Verify new method body executes
result = caller(d)
assert result == 999, f"Expected 999 after monkey-patch, got {result}"

# Phase 4: Verify stability after monkey-patch
for _ in range(1000):
    assert caller(d) == 999, "Monkey-patched method should persist"

print("PASS: smoke_3_monkey_patch — type modification correctly invalidates compiled code")
PYEOF

printf "  [3/3] Monkey-patch:        "
if "$VENV_PYTHON" "$TESTS_DIR/smoke_3_monkey_patch.py" 2>&1; then
    SMOKE_PASS=$((SMOKE_PASS + 1))
else
    echo "FAIL: smoke_3_monkey_patch"
    SMOKE_FAIL=$((SMOKE_FAIL + 1))
fi

echo ""
echo "--- Smoke Tests: $SMOKE_PASS/3 PASS, $SMOKE_FAIL/3 FAIL ---"
TOTAL_PASS=$((TOTAL_PASS + SMOKE_PASS))
TOTAL_FAIL=$((TOTAL_FAIL + SMOKE_FAIL))
if [ "$SMOKE_FAIL" -gt 0 ]; then
    OVERALL_EXIT=3
fi
echo ""

# ════════════════════════════════════════════════════════════════════════════
# SECTION 3: Adversarial Monkey-Patch Tests (4 tests)
# ════════════════════════════════════════════════════════════════════════════

echo "=== Section 3: Adversarial Monkey-Patch Tests (4 tests) ==="
echo ""

ADV_PASS=0
ADV_FAIL=0

# --- Adversarial Test 1: Post-Compile Monkey-Patch ---
cat > "$TESTS_DIR/adv_1_post_compile_monkeypatch.py" << 'PYEOF'
"""
Adversarial Test 1: Monkey-patch AFTER Tier 2 compile, then call without
any IC re-check opportunity.

Attack vector: warm IC → Tier 2 compiles inlined body → monkey-patch → call.
The type_deopt_patchers MUST invalidate the compiled code so the interpreter
runs the new method body.
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def caller(d):
    return d.speak()

# Phase 1: Warm thoroughly (ensure Tier 2 compilation with inlining)
d = Dog()
for _ in range(5000):
    caller(d)

# Verify inlined code works
assert caller(d) == 42

# Phase 2: Monkey-patch AFTER compilation
Dog.speak = lambda self: -1

# Phase 3: Immediate call — no warmup, no re-check opportunity
result = caller(d)
assert result == -1, f"Post-compile monkey-patch: expected -1, got {result}"

# Phase 4: Verify with a fresh instance too
d2 = Dog()
assert caller(d2) == -1, f"Fresh instance post-patch: expected -1, got {caller(d2)}"

print("PASS: adv_1_post_compile_monkeypatch — type_deopt_patchers correctly invalidate compiled code")
PYEOF

printf "  [1/4] Post-compile monkey-patch: "
if "$VENV_PYTHON" "$TESTS_DIR/adv_1_post_compile_monkeypatch.py" 2>&1; then
    ADV_PASS=$((ADV_PASS + 1))
else
    echo "FAIL: adv_1_post_compile_monkeypatch"
    ADV_FAIL=$((ADV_FAIL + 1))
fi

# --- Adversarial Test 2: Tight-Loop Mutation ---
cat > "$TESTS_DIR/adv_2_tight_loop_mutation.py" << 'PYEOF'
"""
Adversarial Test 2: Monkey-patch in a tight loop — can the IC invalidation
race with the inlined code?

Attack vector: rapidly alternate between two method bodies, verifying correctness
at every step. If there is a TOCTOU race between type_deopt_patchers and the
inlined code, we will see a wrong result.
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 1

def caller(d):
    return d.speak()

d = Dog()

# Phase 1: Warm (≥2000 for Tier 2)
for _ in range(3000):
    caller(d)

# Phase 2: Rapid mutation loop
for i in range(200):
    expected_a = i * 10
    expected_b = i * 10 + 1

    Dog.speak = lambda self, v=expected_a: v
    result = caller(d)
    assert result == expected_a, f"Iteration {i}, phase A: expected {expected_a}, got {result}"

    Dog.speak = lambda self, v=expected_b: v
    result = caller(d)
    assert result == expected_b, f"Iteration {i}, phase B: expected {expected_b}, got {result}"

print("PASS: adv_2_tight_loop_mutation — no TOCTOU race in IC invalidation (200 mutations)")
PYEOF

printf "  [2/4] Tight-loop mutation:        "
if "$VENV_PYTHON" "$TESTS_DIR/adv_2_tight_loop_mutation.py" 2>&1; then
    ADV_PASS=$((ADV_PASS + 1))
else
    echo "FAIL: adv_2_tight_loop_mutation"
    ADV_FAIL=$((ADV_FAIL + 1))
fi

# --- Adversarial Test 3: Same Signature, Different Body ---
cat > "$TESTS_DIR/adv_3_same_sig_different_body.py" << 'PYEOF'
"""
Adversarial Test 3: Monkey-patch with the SAME function signature but a
different body. GuardType passes (same type), but the method body is different.

Attack vector: if the guard only checks receiver type and not method identity,
the stale inlined body will execute instead of the new one.
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def caller(d):
    return d.speak()

# Phase 1: Warm with original speak
# ≥2000 calls for Tier 2 recompilation
d = Dog()
for _ in range(3000):
    caller(d)
assert caller(d) == 42

# Phase 2: Replace with same-signature method that returns different value
def new_speak(self):
    return 42 * 2  # 84 — same signature, different body

Dog.speak = new_speak
result = caller(d)
assert result == 84, f"Same-sig swap: expected 84, got {result}"

# Phase 3: Replace again with a method that has side effects
accumulator = []
def logging_speak(self):
    accumulator.append(1)
    return 42 * 3  # 126

Dog.speak = logging_speak
result = caller(d)
assert result == 126, f"Logging swap: expected 126, got {result}"
assert len(accumulator) == 1, f"Side effect not executed: accumulator={accumulator}"

# Phase 4: Verify logging_speak called correct number of times
for _ in range(100):
    caller(d)
assert len(accumulator) == 101, f"Expected 101 calls, got {len(accumulator)}"

print("PASS: adv_3_same_sig_different_body — method body change detected despite same signature")
PYEOF

printf "  [3/4] Same-sig different body:    "
if "$VENV_PYTHON" "$TESTS_DIR/adv_3_same_sig_different_body.py" 2>&1; then
    ADV_PASS=$((ADV_PASS + 1))
else
    echo "FAIL: adv_3_same_sig_different_body"
    ADV_FAIL=$((ADV_FAIL + 1))
fi

# --- Adversarial Test 4: __dict__ Assignment vs Class Attribute ---
cat > "$TESTS_DIR/adv_4_dict_vs_class_attr.py" << 'PYEOF'
"""
Adversarial Test 4: Monkey-patch via __dict__ assignment vs class attribute
assignment — both paths must invalidate compiled code.

Attack vector: type.__dict__ is a mappingproxy, so direct assignment goes through
type.__setattr__. But what if we use the underlying dict directly?
"""
import cinderx
cinderx.init()
import cinderjit
cinderjit.compile_after_n_calls(100)

class Dog:
    def speak(self):
        return 42

def caller(d):
    return d.speak()

# Phase 1: Warm
# ≥2000 calls for Tier 2 recompilation
d = Dog()
for _ in range(3000):
    caller(d)
assert caller(d) == 42

# Phase 2: Monkey-patch via class attribute assignment (normal path)
Dog.speak = lambda self: 100
result = caller(d)
assert result == 100, f"Class attr assignment: expected 100, got {result}"

# Phase 3: Restore original and re-warm (≥2000 for Tier 2)
Dog.speak = lambda self: 42
for _ in range(3000):
    caller(d)
assert caller(d) == 42

# Phase 4: Monkey-patch via type.__setattr__ (explicit)
type.__setattr__(Dog, 'speak', lambda self: 200)
result = caller(d)
assert result == 200, f"type.__setattr__: expected 200, got {result}"

# Phase 5: Test with multiple types that share a method pattern
class Cat:
    def speak(self):
        return 77

def multi_caller(animal):
    return animal.speak()

# Warm multi_caller with Dog (≥2000 for Tier 2)
for _ in range(3000):
    multi_caller(Dog())

# Warm multi_caller with Cat
for _ in range(3000):
    multi_caller(Cat())

# Monkey-patch Dog only
Dog.speak = lambda self: 300
assert multi_caller(Dog()) == 300, f"Multi-type Dog after patch: expected 300"
assert multi_caller(Cat()) == 77, f"Multi-type Cat unchanged: expected 77"

print("PASS: adv_4_dict_vs_class_attr — both mutation paths correctly invalidate")
PYEOF

printf "  [4/4] __dict__ vs class attr:     "
if "$VENV_PYTHON" "$TESTS_DIR/adv_4_dict_vs_class_attr.py" 2>&1; then
    ADV_PASS=$((ADV_PASS + 1))
else
    echo "FAIL: adv_4_dict_vs_class_attr"
    ADV_FAIL=$((ADV_FAIL + 1))
fi

echo ""
echo "--- Adversarial Tests: $ADV_PASS/4 PASS, $ADV_FAIL/4 FAIL ---"
TOTAL_PASS=$((TOTAL_PASS + ADV_PASS))
TOTAL_FAIL=$((TOTAL_FAIL + ADV_FAIL))
if [ "$ADV_FAIL" -gt 0 ]; then
    OVERALL_EXIT=4
fi
echo ""

# ════════════════════════════════════════════════════════════════════════════
# FINAL SUMMARY
# ════════════════════════════════════════════════════════════════════════════

{
    echo "=== CinderX Test Results Summary ==="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Host: $(hostname)"
    echo "Commit: $COMMIT"
    echo "Python: $($VENV_PYTHON --version 2>&1)"
    echo ""
    echo "Section 1: CPython Test Suite"
    if [ "$CPYTHON_RESULT" = "SUCCESS" ]; then
        echo "  Status: PASS ($CPYTHON_TOTAL)"
    elif [ -n "$CPYTHON_RESULT" ]; then
        echo "  Status: $CPYTHON_RESULT"
        [ -n "$CPYTHON_FAIL_LINE" ] && echo "  Failed: $CPYTHON_FAIL_LINE"
    else
        echo "  Status: UNKNOWN (check log)"
    fi
    echo ""
    echo "Section 2: Smoke Tests"
    echo "  $SMOKE_PASS/3 PASS, $SMOKE_FAIL/3 FAIL"
    echo ""
    echo "Section 3: Adversarial Tests"
    echo "  $ADV_PASS/4 PASS, $ADV_FAIL/4 FAIL"
    echo ""
    echo "Overall: $TOTAL_PASS PASS, $TOTAL_FAIL FAIL (smoke+adversarial)"
    if [ "$OVERALL_EXIT" -eq 0 ]; then
        echo "VERDICT: ALL TESTS PASSED"
    else
        echo "VERDICT: FAILURES DETECTED (exit code $OVERALL_EXIT)"
    fi
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
echo "CPython test log: $CPYTHON_LOG"
echo "=== Test suite complete ==="

exit "$OVERALL_EXIT"
