#!/usr/bin/env python3
"""
test_call_kw.py — Correctness and deopt tests for CALL_KW specialisation.

Targets: CALL_KW.

CALL_KW is the CPython 3.12 opcode for function calls that include keyword
arguments. When the adaptive interpreter detects repeated calls with keyword
arguments, it specialises the call path.

In CPython 3.12, CALL_KW handles calls like f(a, b, key=val) where there
is a tuple of keyword names on top of the stack. The specialisation avoids
the overhead of building a full kwargs dict by passing the keyword names
tuple directly.

The CinderX JIT compiles CALL_KW by emitting a GuardType on the callable,
then calling it with the keyword arguments resolved at compile time.

Deopt triggers:
  - Callable type changes
  - Callable is replaced at runtime
  - Different function at same call site
  - Keyword argument names change

Tests cover:
  - Simple function with keyword args
  - Function with positional + keyword args
  - Function with **kwargs
  - Function with defaults overridden by keywords
  - Method call with keyword args
  - Built-in function with keyword args
  - Lambda with keyword args (not applicable — lambdas don't take kw)
  - Class instantiation with keyword args
  - Keyword-only parameters
  - Mixed positional, keyword, and default args
  - Deopt: different function at same call site
  - Deopt: callable replaced at runtime
  - Deopt: class vs function at same site
  - Rapid keyword calls (1000 iterations)
  - Stability — 10000 keyword calls
  - Nested keyword calls
  - Multiple keyword args
  - Keyword args with None/bool values
  - **kwargs passthrough
  - Equivalence: keyword call vs explicit dict unpacking

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after callable change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_kw.py
"""

import sys

WARMUP = 15000  # CinderX auto-compilation typically needs 10000+ calls

# Set to True to require JIT compilation when cinderjit is available.
REQUIRE_JIT = True


def check_jit_compiled(func, name):
    """Verify function is JIT-compiled.

    If REQUIRE_JIT is True and cinderjit is importable, raises AssertionError
    when the function is not compiled. If cinderjit is not available, always
    returns False (interpreter-only mode, tests still run for correctness).

    NOTE: cinderjit.is_jit_compiled() is BROKEN on AArch64 (always returns
    False). Use get_compiled_functions() as fallback.
    """
    try:
        import cinderjit
        if cinderjit.is_jit_compiled(func):
            return True
        compiled = cinderjit.get_compiled_functions()
        func_name = getattr(func, '__qualname__', getattr(func, '__name__', str(func)))
        for cf in compiled:
            if func_name in str(cf):
                return True
        if REQUIRE_JIT:
            assert False, (
                f"REQUIRE_JIT is set but {name} is NOT JIT-compiled. "
                f"Compiled functions: {[str(cf) for cf in compiled[:10]]}"
            )
        return False
    except ImportError:
        return False


def main():
    # --- CinderX bootstrap (skip gracefully if unavailable) ---
    try:
        import cinderx
        cinderx.init()
    except (ImportError, AttributeError):
        pass

    try:
        import cinderjit
        cinderjit.auto()
    except (ImportError, AttributeError):
        pass

    try:
        import cinderjit
        cinderjit.enable_specialized_opcodes()
    except (ImportError, AttributeError):
        pass

    print("=== CALL_KW Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Simple function with keyword args ─────────────────────

    def simple_kw(x, y):
        return x + y

    def call_kw_1(a, b):
        return simple_kw(x=a, y=b)

    for _ in range(WARMUP):
        call_kw_1(1, 2)

    check_jit_compiled(call_kw_1, "call_kw_1")

    try:
        assert call_kw_1(1, 2) == 3
        assert call_kw_1(10, 20) == 30
        assert call_kw_1(-5, 5) == 0
        assert call_kw_1(0, 0) == 0
        print("PASS  Test 1: simple function with keyword args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: simple kw — {e}")
        failed += 1

    # ── Test 2: Positional + keyword args ─────────────────────────────

    def pos_and_kw(a, b, c):
        return a * 100 + b * 10 + c

    def call_kw_2(a, b, c):
        return pos_and_kw(a, b, c=c)

    for _ in range(WARMUP):
        call_kw_2(1, 2, 3)

    check_jit_compiled(call_kw_2, "call_kw_2")

    try:
        assert call_kw_2(1, 2, 3) == 123
        assert call_kw_2(9, 8, 7) == 987
        assert call_kw_2(0, 0, 0) == 0
        print("PASS  Test 2: positional + keyword args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: pos+kw — {e}")
        failed += 1

    # ── Test 3: Function with **kwargs ────────────────────────────────

    def accepts_kwargs(**kwargs):
        return kwargs

    def call_kw_3(key, val):
        return accepts_kwargs(name=key, value=val)

    for _ in range(WARMUP):
        call_kw_3("test", 42)

    check_jit_compiled(call_kw_3, "call_kw_3")

    try:
        result = call_kw_3("alice", 100)
        assert result == {"name": "alice", "value": 100}
        result2 = call_kw_3("bob", 0)
        assert result2 == {"name": "bob", "value": 0}
        print("PASS  Test 3: function with **kwargs")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: **kwargs — {e}")
        failed += 1

    # ── Test 4: Defaults overridden by keywords ───────────────────────

    def with_defaults(a, b=10, c=100):
        return a + b + c

    def call_kw_4(a, c):
        return with_defaults(a, c=c)

    for _ in range(WARMUP):
        call_kw_4(1, 5)

    check_jit_compiled(call_kw_4, "call_kw_4")

    try:
        assert call_kw_4(1, 5) == 16     # 1 + 10(default) + 5
        assert call_kw_4(0, 0) == 10     # 0 + 10 + 0
        assert call_kw_4(1, 100) == 111  # 1 + 10 + 100
        print("PASS  Test 4: defaults overridden by keywords")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: defaults override — {e}")
        failed += 1

    # ── Test 5: Method call with keyword args ─────────────────────────

    class Calculator:
        def __init__(self, base):
            self.base = base

        def compute(self, x, op="add"):
            if op == "add":
                return self.base + x
            elif op == "mul":
                return self.base * x
            else:
                return self.base

    def call_kw_5(calc, x, op):
        return calc.compute(x, op=op)

    calc = Calculator(10)
    for _ in range(WARMUP):
        call_kw_5(calc, 5, "add")

    check_jit_compiled(call_kw_5, "call_kw_5")

    try:
        assert call_kw_5(calc, 5, "add") == 15
        assert call_kw_5(calc, 5, "mul") == 50
        assert call_kw_5(calc, 99, "other") == 10
        print("PASS  Test 5: method call with keyword args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: method kw — {e}")
        failed += 1

    # ── Test 6: Built-in function with keyword args ───────────────────

    def call_kw_6(iterable, key_func):
        return sorted(iterable, key=key_func)

    data = [3, 1, 4, 1, 5]
    for _ in range(WARMUP):
        call_kw_6(data, lambda x: x)

    check_jit_compiled(call_kw_6, "call_kw_6")

    try:
        assert call_kw_6([3, 1, 2], lambda x: x) == [1, 2, 3]
        assert call_kw_6([3, 1, 2], lambda x: -x) == [3, 2, 1]
        assert call_kw_6(["b", "a", "c"], lambda x: x) == ["a", "b", "c"]
        assert call_kw_6([], lambda x: x) == []
        print("PASS  Test 6: built-in function with keyword args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: builtin kw — {e}")
        failed += 1

    # ── Test 7: Class instantiation with keyword args ─────────────────

    class Point:
        def __init__(self, x, y, z=0):
            self.x = x
            self.y = y
            self.z = z

    def call_kw_7(x, y, z):
        return Point(x=x, y=y, z=z)

    for _ in range(WARMUP):
        call_kw_7(1, 2, 3)

    check_jit_compiled(call_kw_7, "call_kw_7")

    try:
        p = call_kw_7(10, 20, 30)
        assert p.x == 10 and p.y == 20 and p.z == 30
        p2 = call_kw_7(0, 0, 0)
        assert p2.x == 0 and p2.y == 0 and p2.z == 0
        print("PASS  Test 7: class instantiation with keyword args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: class kw — {e}")
        failed += 1

    # ── Test 8: Keyword-only parameters ───────────────────────────────

    def kw_only(a, *, key, reverse=False):
        if reverse:
            return (key, a)
        return (a, key)

    def call_kw_8(a, k, r):
        return kw_only(a, key=k, reverse=r)

    for _ in range(WARMUP):
        call_kw_8(1, "x", False)

    check_jit_compiled(call_kw_8, "call_kw_8")

    try:
        assert call_kw_8(1, "x", False) == (1, "x")
        assert call_kw_8(1, "x", True) == ("x", 1)
        assert call_kw_8("a", "b", False) == ("a", "b")
        print("PASS  Test 8: keyword-only parameters")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: kw-only — {e}")
        failed += 1

    # ── Test 9: Mixed positional, keyword, and default args ───────────

    def mixed(a, b, c=30, d=40, e=50):
        return a + b + c + d + e

    def call_kw_9(a, b, d):
        return mixed(a, b, d=d)

    for _ in range(WARMUP):
        call_kw_9(1, 2, 4)

    check_jit_compiled(call_kw_9, "call_kw_9")

    try:
        assert call_kw_9(1, 2, 4) == 1 + 2 + 30 + 4 + 50   # 87
        assert call_kw_9(0, 0, 0) == 30 + 0 + 50             # 80
        assert call_kw_9(10, 20, 40) == 10 + 20 + 30 + 40 + 50  # 150
        print("PASS  Test 9: mixed positional, keyword, and default args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: mixed args — {e}")
        failed += 1

    # ── Test 10: Deopt — different function at same call site ─────────

    def func_a(x, y):
        return x + y

    def func_b(x, y):
        return x * y

    def call_kw_10(fn, a, b):
        return fn(x=a, y=b)

    for _ in range(WARMUP):
        call_kw_10(func_a, 1, 2)

    check_jit_compiled(call_kw_10, "call_kw_10")

    try:
        assert call_kw_10(func_a, 3, 4) == 7
        # Deopt: different function
        assert call_kw_10(func_b, 3, 4) == 12
        # Original still works
        assert call_kw_10(func_a, 10, 20) == 30
        print("PASS  Test 10: deopt — different function at same call site")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: deopt diff func — {e}")
        failed += 1

    # ── Test 11: Deopt — callable replaced at runtime ─────────────────

    def original(x, y):
        return x + y

    container = {"fn": original}

    def call_kw_11(c, a, b):
        return c["fn"](x=a, y=b)

    for _ in range(WARMUP):
        call_kw_11(container, 1, 2)

    check_jit_compiled(call_kw_11, "call_kw_11")

    try:
        assert call_kw_11(container, 3, 4) == 7
        # Replace function
        container["fn"] = lambda x, y: x - y
        assert call_kw_11(container, 10, 3) == 7
        # Replace again
        container["fn"] = original
        assert call_kw_11(container, 5, 5) == 10
        print("PASS  Test 11: deopt — callable replaced at runtime")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: deopt replace — {e}")
        failed += 1

    # ── Test 12: Deopt — class vs function at same site ───────────────

    class Wrapper:
        def __init__(self, value, label="default"):
            self.value = value
            self.label = label

    def make_tuple(value, label="default"):
        return (value, label)

    def call_kw_12(callable_obj, v, l):
        return callable_obj(value=v, label=l)

    for _ in range(WARMUP):
        call_kw_12(make_tuple, 1, "a")

    check_jit_compiled(call_kw_12, "call_kw_12")

    try:
        result = call_kw_12(make_tuple, 42, "test")
        assert result == (42, "test")
        # Deopt: class instead of function
        obj = call_kw_12(Wrapper, 42, "test")
        assert obj.value == 42 and obj.label == "test"
        # Back to function
        result2 = call_kw_12(make_tuple, 0, "zero")
        assert result2 == (0, "zero")
        print("PASS  Test 12: deopt — class vs function at same site")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: deopt class/func — {e}")
        failed += 1

    # ── Test 13: Rapid keyword calls (1000 iterations) ────────────────

    def adder(x, y):
        return x + y

    def rapid_kw_13(n):
        total = 0
        for i in range(n):
            total += adder(x=i, y=1)
        return total

    for _ in range(WARMUP):
        rapid_kw_13(1)

    check_jit_compiled(rapid_kw_13, "rapid_kw_13")

    try:
        result = rapid_kw_13(1000)
        expected = sum(i + 1 for i in range(1000))
        assert result == expected, f"got {result}, expected {expected}"
        print("PASS  Test 13: rapid keyword calls (1000 iterations)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: rapid kw — {e}")
        failed += 1

    # ── Test 14: Stability — 10000 keyword calls ─────────────────────

    def echo_kw(val, tag="none"):
        return (val, tag)

    def stability_kw_14(n):
        for i in range(n):
            result = echo_kw(i, tag=str(i))
            assert result == (i, str(i)), f"iteration {i}: got {result}"
        return True

    for _ in range(WARMUP):
        echo_kw(0, tag="0")

    try:
        assert stability_kw_14(10000)
        print("PASS  Test 14: stability — 10000 keyword calls")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: stability — {e}")
        failed += 1

    # ── Test 15: Nested keyword calls ─────────────────────────────────

    def inner(a, b):
        return a + b

    def outer(x, y):
        return inner(a=x, b=y) * 2

    def call_kw_15(x, y):
        return outer(x=x, y=y)

    for _ in range(WARMUP):
        call_kw_15(1, 2)

    check_jit_compiled(call_kw_15, "call_kw_15")

    try:
        assert call_kw_15(1, 2) == 6     # (1+2)*2
        assert call_kw_15(10, 20) == 60  # (10+20)*2
        assert call_kw_15(0, 0) == 0
        print("PASS  Test 15: nested keyword calls")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: nested kw — {e}")
        failed += 1

    # ── Test 16: Multiple keyword args ────────────────────────────────

    def multi_kw(a, b, c, d, e):
        return a * 10000 + b * 1000 + c * 100 + d * 10 + e

    def call_kw_16(a, b, c, d, e):
        return multi_kw(a=a, b=b, c=c, d=d, e=e)

    for _ in range(WARMUP):
        call_kw_16(1, 2, 3, 4, 5)

    check_jit_compiled(call_kw_16, "call_kw_16")

    try:
        assert call_kw_16(1, 2, 3, 4, 5) == 12345
        assert call_kw_16(9, 8, 7, 6, 5) == 98765
        assert call_kw_16(0, 0, 0, 0, 0) == 0
        print("PASS  Test 16: multiple keyword args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: multi kw — {e}")
        failed += 1

    # ── Test 17: Keyword args with None/bool values ───────────────────

    def with_special(val, flag=None, enabled=True):
        if flag is None:
            flag = "default"
        if not enabled:
            return None
        return (val, flag)

    def call_kw_17(v, f, e):
        return with_special(v, flag=f, enabled=e)

    for _ in range(WARMUP):
        call_kw_17(1, None, True)

    check_jit_compiled(call_kw_17, "call_kw_17")

    try:
        assert call_kw_17(42, None, True) == (42, "default")
        assert call_kw_17(42, "custom", True) == (42, "custom")
        assert call_kw_17(42, "custom", False) is None
        assert call_kw_17(0, None, False) is None
        print("PASS  Test 17: keyword args with None/bool values")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: None/bool kw — {e}")
        failed += 1

    # ── Test 18: **kwargs passthrough ─────────────────────────────────

    def final_handler(a, b, extra=None):
        return (a, b, extra)

    def passthrough(**kwargs):
        return final_handler(**kwargs)

    def call_kw_18(a, b, extra):
        return passthrough(a=a, b=b, extra=extra)

    for _ in range(WARMUP):
        call_kw_18(1, 2, "x")

    check_jit_compiled(call_kw_18, "call_kw_18")

    try:
        assert call_kw_18(1, 2, "x") == (1, 2, "x")
        assert call_kw_18(10, 20, None) == (10, 20, None)
        assert call_kw_18("a", "b", "c") == ("a", "b", "c")
        print("PASS  Test 18: **kwargs passthrough")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: kwargs passthrough — {e}")
        failed += 1

    # ── Test 19: Keyword arg order independence ───────────────────────

    def order_test(first, second, third):
        return (first, second, third)

    def call_kw_19a(a, b, c):
        return order_test(first=a, second=b, third=c)

    def call_kw_19b(a, b, c):
        return order_test(third=c, first=a, second=b)

    for _ in range(WARMUP):
        call_kw_19a(1, 2, 3)
        call_kw_19b(1, 2, 3)

    check_jit_compiled(call_kw_19a, "call_kw_19a")
    check_jit_compiled(call_kw_19b, "call_kw_19b")

    try:
        assert call_kw_19a(1, 2, 3) == (1, 2, 3)
        assert call_kw_19b(1, 2, 3) == (1, 2, 3)
        # Both orderings must produce identical results
        for a, b, c in [(0, 0, 0), (10, 20, 30), (-1, -2, -3)]:
            assert call_kw_19a(a, b, c) == call_kw_19b(a, b, c)
        print("PASS  Test 19: keyword arg order independence")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: kw order — {e}")
        failed += 1

    # ── Test 20: Equivalence — keyword call vs dict unpacking ─────────

    def target(x, y, z):
        return x * 100 + y * 10 + z

    def via_kw(a, b, c):
        return target(x=a, y=b, z=c)

    def via_unpack(a, b, c):
        d = {"x": a, "y": b, "z": c}
        return target(**d)

    for _ in range(WARMUP):
        via_kw(1, 2, 3)
        via_unpack(1, 2, 3)

    check_jit_compiled(via_kw, "via_kw")
    check_jit_compiled(via_unpack, "via_unpack")

    try:
        for a, b, c in [(1, 2, 3), (9, 8, 7), (0, 0, 0), (5, 5, 5)]:
            kw_result = via_kw(a, b, c)
            unpack_result = via_unpack(a, b, c)
            assert kw_result == unpack_result, (
                f"mismatch for ({a}, {b}, {c}): kw={kw_result}, unpack={unpack_result}"
            )
        print("PASS  Test 20: equivalence — keyword call vs dict unpacking")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: equivalence — {e}")
        failed += 1

    # ── Summary ───────────────────────────────────────────────────────

    print()
    total = passed + failed
    print(f"Results: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
