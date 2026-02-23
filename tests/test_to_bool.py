#!/usr/bin/env python3
"""
test_to_bool.py — Correctness and deopt tests for TO_BOOL specialisation.

Targets: TO_BOOL_BOOL, TO_BOOL_INT, TO_BOOL_STR, TO_BOOL_LIST, TO_BOOL_NONE.

TO_BOOL converts a Python object to a boolean value for use in conditional
contexts (if, while, and, or, not). CPython 3.14 introduces specialised
opcodes for common types:

  TO_BOOL_BOOL: operand is bool — no-op (already boolean)
  TO_BOOL_INT:  operand is int  — value != 0
  TO_BOOL_STR:  operand is str  — len != 0
  TO_BOOL_LIST: operand is list — len != 0
  TO_BOOL_NONE: operand is None — always False

The JIT specialisation emits GuardType on the operand (e.g. TBool,
TLongExact, TUnicodeExact, TListExact, TNoneType), then the existing
Simplify pass (simplifyIsTruthy) can eliminate the generic PyObject_IsTrue
call and use a direct comparison or length check.

Deopt triggers:
  - Function JIT-compiled with int arguments, then called with str/list/None
  - Function compiled with bool, then called with int (bool is a subclass)
  - Custom __bool__ objects that bypass the fast path

Tests cover:
  - Bool truthiness (True, False)
  - Int truthiness (0, 1, -1, large, sys.maxsize)
  - String truthiness (empty, non-empty, whitespace)
  - List truthiness (empty, non-empty)
  - None truthiness (always False)
  - Deopt: int-compiled → str argument
  - Deopt: int-compiled → list argument
  - Deopt: int-compiled → None argument
  - Deopt: bool-compiled → int argument
  - Custom __bool__ returning True/False
  - Custom __bool__ raising exception
  - Negation (not x) exercising TO_BOOL
  - Truthiness in 'and' / 'or' short-circuit
  - Truthiness in while loop condition
  - Rapid type alternation stability
  - Tuple/dict/set/float truthiness (fallback, no specialised opcode)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_to_bool.py
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
                f"{name} not JIT-compiled after {WARMUP} warmup calls. "
                "Test cannot verify JIT path — increase WARMUP or check "
                "cinderjit.auto() is enabled."
            )
        print(f"  WARNING: {name} not found in compiled functions — may not test JIT path")
        return False
    except (ImportError, AttributeError):
        return False


def main():
    print("=== TO_BOOL Correctness & Deopt Tests ===")
    print()

    try:
        import cinderx
        cinderx.init()
        import cinderjit
        cinderjit.auto()
        try:
            cinderjit.enable_specialized_opcodes()
        except AttributeError:
            pass
    except (ImportError, AttributeError):
        print("SKIP — cinderx/cinderjit not available")
        sys.exit(0)

    passed = 0
    failed = 0

    # ── Test 1: Bool truthiness (TO_BOOL_BOOL) ───────────────────────────

    def to_bool_bool_1(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_bool_1(True)

    check_jit_compiled(to_bool_bool_1, "to_bool_bool_1")

    try:
        assert to_bool_bool_1(True) is True
        assert to_bool_bool_1(False) is False
        print("PASS  Test 1: bool truthiness (True/False)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: bool truthiness — {e}")
        failed += 1

    # ── Test 2: Int truthiness (TO_BOOL_INT) ─────────────────────────────

    def to_bool_int_2(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_int_2(42)

    check_jit_compiled(to_bool_int_2, "to_bool_int_2")

    try:
        assert to_bool_int_2(0) is False
        assert to_bool_int_2(1) is True
        assert to_bool_int_2(-1) is True
        assert to_bool_int_2(999999) is True
        assert to_bool_int_2(-999999) is True
        assert to_bool_int_2(sys.maxsize) is True
        assert to_bool_int_2(-sys.maxsize) is True
        print("PASS  Test 2: int truthiness (0, 1, -1, large)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: int truthiness — {e}")
        failed += 1

    # ── Test 3: String truthiness (TO_BOOL_STR) ──────────────────────────

    def to_bool_str_3(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_str_3("hello")

    check_jit_compiled(to_bool_str_3, "to_bool_str_3")

    try:
        assert to_bool_str_3("") is False
        assert to_bool_str_3("a") is True
        assert to_bool_str_3("hello world") is True
        assert to_bool_str_3(" ") is True  # Whitespace is truthy
        assert to_bool_str_3("\0") is True  # Null char is truthy
        assert to_bool_str_3("\n") is True  # Newline is truthy
        print("PASS  Test 3: string truthiness (empty, non-empty, whitespace)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: string truthiness — {e}")
        failed += 1

    # ── Test 4: List truthiness (TO_BOOL_LIST) ───────────────────────────

    def to_bool_list_4(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_list_4([1, 2, 3])

    check_jit_compiled(to_bool_list_4, "to_bool_list_4")

    try:
        assert to_bool_list_4([]) is False
        assert to_bool_list_4([0]) is True  # Non-empty, even with falsy element
        assert to_bool_list_4([1, 2, 3]) is True
        assert to_bool_list_4([None]) is True
        assert to_bool_list_4([[]]) is True  # Nested empty list is truthy
        print("PASS  Test 4: list truthiness (empty, non-empty)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: list truthiness — {e}")
        failed += 1

    # ── Test 5: None truthiness (TO_BOOL_NONE) ───────────────────────────

    def to_bool_none_5(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_none_5(None)

    check_jit_compiled(to_bool_none_5, "to_bool_none_5")

    try:
        assert to_bool_none_5(None) is False
        print("PASS  Test 5: None truthiness (always False)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: None truthiness — {e}")
        failed += 1

    # ── Test 6: Deopt int → str ──────────────────────────────────────────
    # JIT compiles with int (TO_BOOL_INT), then called with str.
    # GuardType must fire, deopt to interpreter.

    def to_bool_deopt_6(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_deopt_6(42)

    check_jit_compiled(to_bool_deopt_6, "to_bool_deopt_6")

    try:
        # Verify int path works
        assert to_bool_deopt_6(42) is True
        assert to_bool_deopt_6(0) is False

        # Deopt: str argument
        assert to_bool_deopt_6("hello") is True
        assert to_bool_deopt_6("") is False

        # Int still works after deopt
        assert to_bool_deopt_6(1) is True
        assert to_bool_deopt_6(0) is False

        print("PASS  Test 6: deopt int → str")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: deopt int → str — {e}")
        failed += 1

    # ── Test 7: Deopt int → list ─────────────────────────────────────────

    def to_bool_deopt_7(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_deopt_7(42)

    check_jit_compiled(to_bool_deopt_7, "to_bool_deopt_7")

    try:
        assert to_bool_deopt_7(42) is True
        assert to_bool_deopt_7(0) is False

        # Deopt: list argument
        assert to_bool_deopt_7([1, 2]) is True
        assert to_bool_deopt_7([]) is False

        # Int still works
        assert to_bool_deopt_7(1) is True

        print("PASS  Test 7: deopt int → list")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: deopt int → list — {e}")
        failed += 1

    # ── Test 8: Deopt int → None ─────────────────────────────────────────

    def to_bool_deopt_8(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_deopt_8(42)

    check_jit_compiled(to_bool_deopt_8, "to_bool_deopt_8")

    try:
        assert to_bool_deopt_8(42) is True
        assert to_bool_deopt_8(0) is False

        # Deopt: None
        assert to_bool_deopt_8(None) is False

        # Int still works
        assert to_bool_deopt_8(1) is True

        print("PASS  Test 8: deopt int → None")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt int → None — {e}")
        failed += 1

    # ── Test 9: Deopt bool → int ─────────────────────────────────────────
    # bool is a subclass of int. TO_BOOL_BOOL compiled, then int argument.
    # The GuardType(TBool) should fire for plain int.

    def to_bool_deopt_9(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_deopt_9(True)

    check_jit_compiled(to_bool_deopt_9, "to_bool_deopt_9")

    try:
        assert to_bool_deopt_9(True) is True
        assert to_bool_deopt_9(False) is False

        # Deopt: plain int (not bool)
        assert to_bool_deopt_9(42) is True
        assert to_bool_deopt_9(0) is False

        # Bool still works
        assert to_bool_deopt_9(True) is True
        assert to_bool_deopt_9(False) is False

        print("PASS  Test 9: deopt bool → int")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt bool → int — {e}")
        failed += 1

    # ── Test 10: Custom __bool__ returning True ──────────────────────────

    class AlwaysTrue:
        def __bool__(self):
            return True

    class AlwaysFalse:
        def __bool__(self):
            return False

    def to_bool_custom_10(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_custom_10(42)

    check_jit_compiled(to_bool_custom_10, "to_bool_custom_10")

    try:
        assert to_bool_custom_10(42) is True

        # Deopt: custom __bool__ objects
        assert to_bool_custom_10(AlwaysTrue()) is True
        assert to_bool_custom_10(AlwaysFalse()) is False

        # Int still works
        assert to_bool_custom_10(1) is True
        assert to_bool_custom_10(0) is False

        print("PASS  Test 10: custom __bool__ objects (deopt from int)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: custom __bool__ — {e}")
        failed += 1

    # ── Test 11: Custom __bool__ raising exception ───────────────────────

    class BadBool:
        def __bool__(self):
            raise ValueError("bad bool")

    def to_bool_exc_11(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_exc_11(42)

    check_jit_compiled(to_bool_exc_11, "to_bool_exc_11")

    try:
        assert to_bool_exc_11(42) is True

        # Deopt: __bool__ raises
        try:
            to_bool_exc_11(BadBool())
            assert False, "expected ValueError from BadBool.__bool__"
        except ValueError as ve:
            assert "bad bool" in str(ve)

        # Int still works after exception
        assert to_bool_exc_11(1) is True
        assert to_bool_exc_11(0) is False

        print("PASS  Test 11: custom __bool__ raising ValueError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: custom __bool__ exception — {e}")
        failed += 1

    # ── Test 12: Negation (not x) ────────────────────────────────────────
    # 'not x' uses TO_BOOL internally then inverts.

    def negate_12(x):
        return not x

    for _ in range(WARMUP):
        negate_12(42)

    check_jit_compiled(negate_12, "negate_12")

    try:
        assert negate_12(0) is True
        assert negate_12(1) is False
        assert negate_12(-1) is False
        assert negate_12("") is True
        assert negate_12("x") is False
        assert negate_12([]) is True
        assert negate_12([1]) is False
        assert negate_12(None) is True
        assert negate_12(True) is False
        assert negate_12(False) is True
        print("PASS  Test 12: negation (not x)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: negation — {e}")
        failed += 1

    # ── Test 13: Short-circuit 'and' ─────────────────────────────────────
    # 'a and b' evaluates truthiness of a (TO_BOOL), returns a if falsy,
    # else returns b.

    def and_op_13(a, b):
        return a and b

    for _ in range(WARMUP):
        and_op_13(1, 2)

    check_jit_compiled(and_op_13, "and_op_13")

    try:
        assert and_op_13(1, 2) == 2
        assert and_op_13(0, 2) == 0
        assert and_op_13("", "hi") == ""
        assert and_op_13("x", "y") == "y"
        assert and_op_13([], [1]) == []
        assert and_op_13([1], [2]) == [2]
        assert and_op_13(None, 5) is None
        assert and_op_13(True, False) is False
        assert and_op_13(False, True) is False
        print("PASS  Test 13: short-circuit 'and'")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: short-circuit 'and' — {e}")
        failed += 1

    # ── Test 14: Short-circuit 'or' ──────────────────────────────────────
    # 'a or b' evaluates truthiness of a (TO_BOOL), returns a if truthy,
    # else returns b.

    def or_op_14(a, b):
        return a or b

    for _ in range(WARMUP):
        or_op_14(0, 2)

    check_jit_compiled(or_op_14, "or_op_14")

    try:
        assert or_op_14(1, 2) == 1
        assert or_op_14(0, 2) == 2
        assert or_op_14("", "hi") == "hi"
        assert or_op_14("x", "y") == "x"
        assert or_op_14([], [1]) == [1]
        assert or_op_14([1], [2]) == [1]
        assert or_op_14(None, 5) == 5
        assert or_op_14(0, None) is None
        assert or_op_14(True, False) is True
        assert or_op_14(False, True) is True
        print("PASS  Test 14: short-circuit 'or'")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: short-circuit 'or' — {e}")
        failed += 1

    # ── Test 15: while loop condition ────────────────────────────────────
    # TO_BOOL fires on every iteration to check the loop condition.

    def while_loop_15(n):
        count = 0
        while n:
            count += 1
            n -= 1
        return count

    for _ in range(WARMUP):
        while_loop_15(5)

    check_jit_compiled(while_loop_15, "while_loop_15")

    try:
        assert while_loop_15(0) == 0
        assert while_loop_15(1) == 1
        assert while_loop_15(5) == 5
        assert while_loop_15(100) == 100
        print("PASS  Test 15: while loop condition (int countdown)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: while loop condition — {e}")
        failed += 1

    # ── Test 16: while loop with list pop ────────────────────────────────
    # List truthiness checked each iteration.

    def while_list_16(items):
        result = []
        while items:
            result.append(items.pop())
        return result

    for _ in range(WARMUP):
        while_list_16([1, 2, 3])

    check_jit_compiled(while_list_16, "while_list_16")

    try:
        assert while_list_16([]) == []
        assert while_list_16([1]) == [1]
        assert while_list_16([1, 2, 3]) == [3, 2, 1]
        print("PASS  Test 16: while loop with list truthiness")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: while loop list — {e}")
        failed += 1

    # ── Test 17: Rapid type alternation ──────────────────────────────────
    # Call with alternating types to stress deopt/reopt cycles.

    def to_bool_rapid_17(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_rapid_17(42)

    check_jit_compiled(to_bool_rapid_17, "to_bool_rapid_17")

    try:
        values = [
            (42, True), (0, False),
            ("hi", True), ("", False),
            ([1], True), ([], False),
            (None, False),
            (True, True), (False, False),
            (3.14, True), (0.0, False),
        ]
        for i in range(10):  # 10 full cycles
            for val, expected in values:
                result = to_bool_rapid_17(val)
                assert result is expected, (
                    f"cycle {i}, val={val!r}: got {result}, expected {expected}"
                )

        print("PASS  Test 17: rapid type alternation (10 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: rapid type alternation — {e}")
        failed += 1

    # ── Test 18: Float truthiness (no specialised opcode, fallback) ──────
    # CPython has no TO_BOOL_FLOAT. Tests that the generic path works.

    def to_bool_float_18(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_float_18(3.14)

    check_jit_compiled(to_bool_float_18, "to_bool_float_18")

    try:
        assert to_bool_float_18(0.0) is False
        assert to_bool_float_18(-0.0) is False  # -0.0 is falsy
        assert to_bool_float_18(1.0) is True
        assert to_bool_float_18(-1.0) is True
        assert to_bool_float_18(float('inf')) is True
        assert to_bool_float_18(float('-inf')) is True
        # NaN is truthy in Python
        assert to_bool_float_18(float('nan')) is True
        print("PASS  Test 18: float truthiness (no specialised opcode)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: float truthiness — {e}")
        failed += 1

    # ── Test 19: Tuple/dict/set truthiness (no specialised opcode) ───────

    def to_bool_container_19(x):
        if x:
            return True
        return False

    for _ in range(WARMUP):
        to_bool_container_19((1, 2))

    check_jit_compiled(to_bool_container_19, "to_bool_container_19")

    try:
        # Tuple
        assert to_bool_container_19(()) is False
        assert to_bool_container_19((1,)) is True
        assert to_bool_container_19((1, 2, 3)) is True

        # Dict
        assert to_bool_container_19({}) is False
        assert to_bool_container_19({"a": 1}) is True

        # Set
        assert to_bool_container_19(set()) is False
        assert to_bool_container_19({1, 2}) is True

        # Frozenset
        assert to_bool_container_19(frozenset()) is False
        assert to_bool_container_19(frozenset({1})) is True

        print("PASS  Test 19: tuple/dict/set/frozenset truthiness")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: container truthiness — {e}")
        failed += 1

    # ── Test 20: Conditional expression (ternary) ────────────────────────
    # 'a if cond else b' uses TO_BOOL on cond.

    def ternary_20(cond, a, b):
        return a if cond else b

    for _ in range(WARMUP):
        ternary_20(True, "yes", "no")

    check_jit_compiled(ternary_20, "ternary_20")

    try:
        assert ternary_20(True, "yes", "no") == "yes"
        assert ternary_20(False, "yes", "no") == "no"
        assert ternary_20(1, "yes", "no") == "yes"
        assert ternary_20(0, "yes", "no") == "no"
        assert ternary_20("x", "yes", "no") == "yes"
        assert ternary_20("", "yes", "no") == "no"
        assert ternary_20([], "yes", "no") == "no"
        assert ternary_20([1], "yes", "no") == "yes"
        assert ternary_20(None, "yes", "no") == "no"
        print("PASS  Test 20: conditional expression (ternary)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: conditional expression — {e}")
        failed += 1

    # ── Summary ──────────────────────────────────────────────────────────

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
