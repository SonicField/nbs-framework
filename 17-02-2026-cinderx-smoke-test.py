#!/usr/bin/env python3
"""CinderX ARM JIT Smoke Test — Phase 2 pre-baseline

Minimal tests to verify the JIT can compile and execute basic functions
on aarch64 before running the full PyTorch baseline.

Tests are ordered by complexity — earliest failure pinpoints the gap.

Usage:
    PYTHONPATH=scratch/lib.linux-aarch64-cpython-312 python3 17-02-2026-cinderx-smoke-test.py
"""

import sys
import traceback


def test_import():
    """Can CinderX be imported?"""
    import cinderx
    assert cinderx is not None
    return True


def test_init():
    """Can CinderX be initialised?"""
    import cinderx
    cinderx.init()
    assert cinderx.is_supported_runtime()
    return True


def test_frame_evaluator():
    """Can the JIT frame evaluator be installed?"""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()
    assert cinderx.is_frame_evaluator_installed()
    return True


def test_trivial_add():
    """Can the JIT compile and execute a trivial addition?

    This exercises: function entry/exit (frame_asm), arithmetic (autogen),
    and return value passing (calling convention).
    """
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def add(a, b):
        return a + b

    # Call multiple times to trigger JIT compilation
    for _ in range(200):
        result = add(3, 4)
    assert result == 7, f"Expected 7, got {result}"
    return True


def test_integer_arithmetic():
    """Test integer arithmetic operations."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def arith(x, y):
        a = x + y
        b = x - y
        c = x * y
        d = x // y
        e = x % y
        return a, b, c, d, e

    for _ in range(200):
        result = arith(17, 5)
    assert result == (22, 12, 85, 3, 2), f"Expected (22, 12, 85, 3, 2), got {result}"
    return True


def test_float_arithmetic():
    """Test floating-point operations."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def float_ops(x, y):
        return x + y, x * y, x / y

    for _ in range(200):
        result = float_ops(3.14, 2.0)
    assert abs(result[0] - 5.14) < 1e-10, f"float add failed: {result[0]}"
    assert abs(result[1] - 6.28) < 1e-10, f"float mul failed: {result[1]}"
    assert abs(result[2] - 1.57) < 1e-10, f"float div failed: {result[2]}"
    return True


def test_control_flow():
    """Test branches and loops."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def fib(n):
        if n <= 1:
            return n
        a, b = 0, 1
        for _ in range(n - 1):
            a, b = b, a + b
        return b

    for _ in range(200):
        result = fib(10)
    assert result == 55, f"Expected 55, got {result}"
    return True


def test_function_calls():
    """Test nested function calls (exercises frame management)."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def inner(x):
        return x * 2

    def outer(x):
        return inner(x) + inner(x + 1)

    for _ in range(200):
        result = outer(5)
    assert result == 22, f"Expected 22, got {result}"
    return True


def test_closures():
    """Test closures."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def make_adder(n):
        def adder(x):
            return x + n
        return adder

    add5 = make_adder(5)
    for _ in range(200):
        result = add5(10)
    assert result == 15, f"Expected 15, got {result}"
    return True


def test_generators():
    """Test generators (exercises yield/resume — known gap in autogen.cpp:786)."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def gen_range(n):
        i = 0
        while i < n:
            yield i
            i += 1

    for _ in range(50):
        result = list(gen_range(5))
    assert result == [0, 1, 2, 3, 4], f"Expected [0,1,2,3,4], got {result}"
    return True


def test_class_operations():
    """Test class instantiation and method calls."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    class Point:
        def __init__(self, x, y):
            self.x = x
            self.y = y

        def magnitude_sq(self):
            return self.x * self.x + self.y * self.y

    for _ in range(200):
        p = Point(3, 4)
        result = p.magnitude_sq()
    assert result == 25, f"Expected 25, got {result}"
    return True


def test_exception_handling():
    """Test try/except (exercises deoptimisation paths)."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def safe_div(a, b):
        try:
            return a / b
        except ZeroDivisionError:
            return float('inf')

    for _ in range(200):
        r1 = safe_div(10, 2)
        r2 = safe_div(10, 0)
    assert r1 == 5.0, f"Expected 5.0, got {r1}"
    assert r2 == float('inf'), f"Expected inf, got {r2}"
    return True


def test_list_comprehension():
    """Test list comprehension (common Python pattern)."""
    import cinderx
    cinderx.init()
    cinderx.install_frame_evaluator()

    def squares(n):
        return [x * x for x in range(n)]

    for _ in range(200):
        result = squares(5)
    assert result == [0, 1, 4, 9, 16], f"Expected [0,1,4,9,16], got {result}"
    return True


TESTS = [
    ("import", test_import),
    ("init", test_init),
    ("frame_evaluator", test_frame_evaluator),
    ("trivial_add", test_trivial_add),
    ("integer_arithmetic", test_integer_arithmetic),
    ("float_arithmetic", test_float_arithmetic),
    ("control_flow", test_control_flow),
    ("function_calls", test_function_calls),
    ("closures", test_closures),
    ("generators", test_generators),
    ("class_operations", test_class_operations),
    ("exception_handling", test_exception_handling),
    ("list_comprehension", test_list_comprehension),
]


def main():
    print(f"CinderX ARM JIT Smoke Test")
    print(f"Python: {sys.version}")
    print(f"Platform: {sys.platform}")
    print(f"Architecture: {__import__('platform').machine()}")
    print()

    passed = 0
    failed = 0
    errors = []

    for name, test_func in TESTS:
        try:
            test_func()
            print(f"  PASS: {name}")
            passed += 1
        except Exception as e:
            print(f"  FAIL: {name} — {e}")
            traceback.print_exc(limit=3)
            failed += 1
            errors.append((name, str(e)))
        except SystemExit:
            print(f"  CRASH: {name} — process attempted exit")
            failed += 1
            errors.append((name, "SystemExit"))

    print()
    print(f"Results: {passed} passed, {failed} failed out of {len(TESTS)}")

    if errors:
        print()
        print("Failures:")
        for name, err in errors:
            print(f"  {name}: {err}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
