#!/usr/bin/env python3
"""
test_binary_op_add_int.py — Correctness and deopt tests for BINARY_OP integer
arithmetic specialisations.

Targets: BINARY_OP_ADD_INT, BINARY_OP_SUBTRACT_INT, BINARY_OP_MULTIPLY_INT.

These specialisations emit GuardType on both operands to confirm they are int,
then use the fast nb_add/nb_subtract/nb_multiply slot directly (or an inlined
integer arithmetic path) instead of generic binary_op dispatch.

When a function is JIT-compiled with int operands and then called with a
different operand type (float, str, custom __add__), the GuardType must fire,
triggering deoptimisation back to the interpreter. The interpreter must then
produce the correct result.

Tests cover:
  - Basic int arithmetic correctness (add, subtract, multiply)
  - Edge cases: zero, negative, large values
  - Overflow to bigint (sys.maxsize + 1)
  - Deopt: int-compiled then called with float operands
  - Deopt: int-compiled then called with custom __add__ objects
  - Deopt: mixed type operands (int + float)
  - Accumulator loops (common pattern in real code)
  - Rapid type alternation stability
  - Subtraction and multiplication deopt paths

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct deopt when operand type changes
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_add_int.py
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
    print("=== BINARY_OP Integer Arithmetic Correctness & Deopt Tests ===")
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

    # ── Test 1: Basic int addition ───────────────────────────────────────

    def add_ints_1(a, b):
        return a + b

    for _ in range(WARMUP):
        add_ints_1(3, 7)

    check_jit_compiled(add_ints_1, "add_ints_1")

    try:
        assert add_ints_1(3, 7) == 10
        assert add_ints_1(0, 0) == 0
        assert add_ints_1(-5, 5) == 0
        assert add_ints_1(-3, -7) == -10
        assert add_ints_1(1, 0) == 1
        assert add_ints_1(0, 1) == 1
        print("PASS  Test 1: basic int addition")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic int addition — {e}")
        failed += 1

    # ── Test 2: Basic int subtraction ────────────────────────────────────

    def sub_ints_2(a, b):
        return a - b

    for _ in range(WARMUP):
        sub_ints_2(10, 3)

    check_jit_compiled(sub_ints_2, "sub_ints_2")

    try:
        assert sub_ints_2(10, 3) == 7
        assert sub_ints_2(0, 0) == 0
        assert sub_ints_2(5, 5) == 0
        assert sub_ints_2(-3, 7) == -10
        assert sub_ints_2(3, -7) == 10
        assert sub_ints_2(-3, -7) == 4
        print("PASS  Test 2: basic int subtraction")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: basic int subtraction — {e}")
        failed += 1

    # ── Test 3: Basic int multiplication ─────────────────────────────────

    def mul_ints_3(a, b):
        return a * b

    for _ in range(WARMUP):
        mul_ints_3(3, 7)

    check_jit_compiled(mul_ints_3, "mul_ints_3")

    try:
        assert mul_ints_3(3, 7) == 21
        assert mul_ints_3(0, 999) == 0
        assert mul_ints_3(999, 0) == 0
        assert mul_ints_3(-3, 7) == -21
        assert mul_ints_3(-3, -7) == 21
        assert mul_ints_3(1, 42) == 42
        print("PASS  Test 3: basic int multiplication")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: basic int multiplication — {e}")
        failed += 1

    # ── Test 4: Overflow to bigint (add) ─────────────────────────────────

    def add_overflow_4(a, b):
        return a + b

    for _ in range(WARMUP):
        add_overflow_4(100, 200)

    check_jit_compiled(add_overflow_4, "add_overflow_4")

    try:
        maxint = sys.maxsize
        result = add_overflow_4(maxint, 1)
        assert result == maxint + 1, f"got {result}, expected {maxint + 1}"
        assert isinstance(result, int), "result should still be int (Python bigint)"

        result2 = add_overflow_4(maxint, maxint)
        assert result2 == 2 * maxint, f"got {result2}"

        # Negative overflow
        minint = -sys.maxsize - 1
        result3 = add_overflow_4(minint, -1)
        assert result3 == minint - 1, f"got {result3}"

        print("PASS  Test 4: overflow to bigint (add)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: overflow to bigint — {e}")
        failed += 1

    # ── Test 5: Overflow to bigint (multiply) ────────────────────────────

    def mul_overflow_5(a, b):
        return a * b

    for _ in range(WARMUP):
        mul_overflow_5(10, 20)

    check_jit_compiled(mul_overflow_5, "mul_overflow_5")

    try:
        maxint = sys.maxsize
        result = mul_overflow_5(maxint, 2)
        assert result == maxint * 2, f"got {result}"
        assert isinstance(result, int)

        result2 = mul_overflow_5(maxint, maxint)
        assert result2 == maxint * maxint, f"got {result2}"

        print("PASS  Test 5: overflow to bigint (multiply)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: overflow to bigint (multiply) — {e}")
        failed += 1

    # ── Test 6: Add deopt — int-compiled, then float operands ────────────

    def add_deopt_6(a, b):
        return a + b

    for _ in range(WARMUP):
        add_deopt_6(3, 7)

    check_jit_compiled(add_deopt_6, "add_deopt_6")

    try:
        # Verify int path still works
        assert add_deopt_6(3, 7) == 10

        # Now call with floats — should trigger GuardType deopt
        float_result = add_deopt_6(3.5, 7.5)
        assert float_result == 11.0, f"float deopt: got {float_result}, expected 11.0"
        assert isinstance(float_result, float), f"expected float, got {type(float_result)}"

        # Verify int path STILL works after deopt
        int_result = add_deopt_6(3, 7)
        assert int_result == 10, f"int path after deopt: got {int_result}"
        assert isinstance(int_result, int), f"expected int after deopt, got {type(int_result)}"

        print("PASS  Test 6: add deopt int -> float")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: add deopt int -> float — {e}")
        failed += 1

    # ── Test 7: Subtract deopt — int-compiled, then float operands ───────

    def sub_deopt_7(a, b):
        return a - b

    for _ in range(WARMUP):
        sub_deopt_7(10, 3)

    check_jit_compiled(sub_deopt_7, "sub_deopt_7")

    try:
        assert sub_deopt_7(10, 3) == 7

        float_result = sub_deopt_7(10.5, 3.5)
        assert float_result == 7.0, f"got {float_result}"
        assert isinstance(float_result, float)

        # int still works after deopt
        assert sub_deopt_7(10, 3) == 7

        print("PASS  Test 7: subtract deopt int -> float")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: subtract deopt int -> float — {e}")
        failed += 1

    # ── Test 8: Multiply deopt — int-compiled, then float operands ───────

    def mul_deopt_8(a, b):
        return a * b

    for _ in range(WARMUP):
        mul_deopt_8(3, 7)

    check_jit_compiled(mul_deopt_8, "mul_deopt_8")

    try:
        assert mul_deopt_8(3, 7) == 21

        float_result = mul_deopt_8(3.0, 7.0)
        assert float_result == 21.0, f"got {float_result}"
        assert isinstance(float_result, float)

        assert mul_deopt_8(3, 7) == 21

        print("PASS  Test 8: multiply deopt int -> float")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: multiply deopt int -> float — {e}")
        failed += 1

    # ── Test 9: Mixed operand deopt — one int, one float ─────────────────

    def add_mixed_9(a, b):
        return a + b

    for _ in range(WARMUP):
        add_mixed_9(3, 7)

    check_jit_compiled(add_mixed_9, "add_mixed_9")

    try:
        assert add_mixed_9(3, 7) == 10

        # int + float -> should deopt and produce float
        mixed = add_mixed_9(3, 7.5)
        assert mixed == 10.5, f"got {mixed}"
        assert isinstance(mixed, float), f"expected float, got {type(mixed)}"

        # float + int -> should also work
        mixed2 = add_mixed_9(3.5, 7)
        assert mixed2 == 10.5, f"got {mixed2}"
        assert isinstance(mixed2, float)

        # int + int still works
        assert add_mixed_9(3, 7) == 10

        print("PASS  Test 9: mixed operand deopt (int + float)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: mixed operand deopt — {e}")
        failed += 1

    # ── Test 10: Custom __add__ deopt ────────────────────────────────────

    class Vector:
        def __init__(self, x, y):
            self.x = x
            self.y = y
        def __add__(self, other):
            return Vector(self.x + other.x, self.y + other.y)
        def __eq__(self, other):
            return self.x == other.x and self.y == other.y

    def add_custom_10(a, b):
        return a + b

    for _ in range(WARMUP):
        add_custom_10(3, 7)

    check_jit_compiled(add_custom_10, "add_custom_10")

    try:
        assert add_custom_10(3, 7) == 10

        # Call with custom objects — should deopt
        v1 = Vector(1, 2)
        v2 = Vector(3, 4)
        result = add_custom_10(v1, v2)
        assert result == Vector(4, 6), f"custom __add__: got ({result.x}, {result.y})"

        # int path still works after custom deopt
        assert add_custom_10(3, 7) == 10

        print("PASS  Test 10: custom __add__ deopt")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: custom __add__ deopt — {e}")
        failed += 1

    # ── Test 11: String concatenation deopt ──────────────────────────────

    def add_str_11(a, b):
        return a + b

    for _ in range(WARMUP):
        add_str_11(3, 7)

    check_jit_compiled(add_str_11, "add_str_11")

    try:
        assert add_str_11(3, 7) == 10

        # Call with strings — should deopt to string concatenation
        str_result = add_str_11("hello", " world")
        assert str_result == "hello world", f"got {str_result!r}"
        assert isinstance(str_result, str)

        # int still works
        assert add_str_11(3, 7) == 10

        print("PASS  Test 11: string concatenation deopt")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: string concatenation deopt — {e}")
        failed += 1

    # ── Test 12: Accumulator loop (common pattern) ───────────────────────

    def accumulate_12(values):
        total = 0
        for v in values:
            total += v
        return total

    data = list(range(100))
    for _ in range(WARMUP):
        accumulate_12(data)

    check_jit_compiled(accumulate_12, "accumulate_12")

    try:
        assert accumulate_12(data) == 4950
        assert accumulate_12([]) == 0
        assert accumulate_12([1]) == 1
        assert accumulate_12([-1, 1, -1, 1]) == 0
        assert accumulate_12([sys.maxsize, 1]) == sys.maxsize + 1

        print("PASS  Test 12: accumulator loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: accumulator loop — {e}")
        failed += 1

    # ── Test 13: Multiply accumulator (factorial-like) ───────────────────

    def product_13(values):
        result = 1
        for v in values:
            result *= v
        return result

    data_mul = list(range(1, 20))
    for _ in range(WARMUP):
        product_13(data_mul)

    check_jit_compiled(product_13, "product_13")

    try:
        import math
        assert product_13(data_mul) == math.factorial(19)
        assert product_13([1]) == 1
        assert product_13([0, 1, 2, 3]) == 0
        assert product_13([-1, 2, 3]) == -6
        assert product_13([-1, -1, -1]) == -1

        print("PASS  Test 13: multiply accumulator (factorial-like)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: multiply accumulator — {e}")
        failed += 1

    # ── Test 14: Subtraction accumulator (countdown) ─────────────────────

    def countdown_14(start, steps):
        value = start
        for s in steps:
            value -= s
        return value

    steps = [1] * 100
    for _ in range(WARMUP):
        countdown_14(100, steps)

    check_jit_compiled(countdown_14, "countdown_14")

    try:
        assert countdown_14(100, steps) == 0
        assert countdown_14(0, [1, 2, 3]) == -6
        assert countdown_14(10, [-1, -2, -3]) == 16
        assert countdown_14(sys.maxsize, [sys.maxsize]) == 0

        print("PASS  Test 14: subtraction accumulator")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: subtraction accumulator — {e}")
        failed += 1

    # ── Test 15: Accumulator deopt — int loop then float input ───────────

    def accumulate_deopt_15(values):
        total = 0
        for v in values:
            total += v
        return total

    for _ in range(WARMUP):
        accumulate_deopt_15(list(range(50)))

    check_jit_compiled(accumulate_deopt_15, "accumulate_deopt_15")

    try:
        # Int path
        assert accumulate_deopt_15(list(range(50))) == 1225

        # Float input — deopt inside loop
        float_data = [1.5, 2.5, 3.0]
        float_result = accumulate_deopt_15(float_data)
        assert float_result == 7.0, f"float accumulate: got {float_result}"
        # Note: 0 (int) + 1.5 (float) -> first iteration produces float
        assert isinstance(float_result, float), f"expected float, got {type(float_result)}"

        # Int path still works after float deopt
        assert accumulate_deopt_15(list(range(50))) == 1225

        print("PASS  Test 15: accumulator deopt int -> float")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: accumulator deopt — {e}")
        failed += 1

    # ── Test 16: Rapid type alternation ──────────────────────────────────

    def add_alt_16(a, b):
        return a + b

    for _ in range(WARMUP):
        add_alt_16(3, 7)

    check_jit_compiled(add_alt_16, "add_alt_16")

    try:
        all_correct = True
        for _ in range(200):
            if add_alt_16(3, 7) != 10:
                all_correct = False
                break
            if add_alt_16(3.0, 7.0) != 10.0:
                all_correct = False
                break
            if add_alt_16("a", "b") != "ab":
                all_correct = False
                break
        assert all_correct, "rapid alternation produced incorrect result"

        print("PASS  Test 16: rapid type alternation (int/float/str)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: rapid type alternation — {e}")
        failed += 1

    # ── Test 17: TypeError for incompatible types ────────────────────────

    def add_typeerr_17(a, b):
        return a + b

    for _ in range(WARMUP):
        add_typeerr_17(3, 7)

    check_jit_compiled(add_typeerr_17, "add_typeerr_17")

    try:
        # int + str should raise TypeError after deopt
        raised = False
        try:
            add_typeerr_17(3, "hello")
        except TypeError:
            raised = True
        assert raised, "expected TypeError for int + str"

        # str + int should also raise
        raised = False
        try:
            add_typeerr_17("hello", 3)
        except TypeError:
            raised = True
        assert raised, "expected TypeError for str + int"

        # int + int still works after TypeError
        assert add_typeerr_17(3, 7) == 10

        print("PASS  Test 17: TypeError for incompatible types after deopt")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: TypeError — {e}")
        failed += 1

    # ── Test 18: Combined arithmetic expression ──────────────────────────

    def combined_18(a, b, c):
        return a + b * c - a

    for _ in range(WARMUP):
        combined_18(5, 3, 7)

    check_jit_compiled(combined_18, "combined_18")

    try:
        # a + b*c - a = b*c
        assert combined_18(5, 3, 7) == 21, f"got {combined_18(5, 3, 7)}"
        assert combined_18(0, 0, 0) == 0
        assert combined_18(100, 2, 3) == 6
        assert combined_18(-5, -3, -7) == 21

        # Deopt with floats
        float_r = combined_18(5.0, 3.0, 7.0)
        assert float_r == 21.0, f"float: got {float_r}"

        # int still works
        assert combined_18(5, 3, 7) == 21

        print("PASS  Test 18: combined arithmetic expression")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: combined expression — {e}")
        failed += 1

    # ── Test 19: Boolean operands (bool is subclass of int) ──────────────

    def add_bool_19(a, b):
        return a + b

    for _ in range(WARMUP):
        add_bool_19(3, 7)

    check_jit_compiled(add_bool_19, "add_bool_19")

    try:
        # bool is a subclass of int — should work through int path
        # or deopt cleanly if GuardType checks exact type
        result = add_bool_19(True, True)
        assert result == 2, f"True + True: got {result}"

        result2 = add_bool_19(True, 5)
        assert result2 == 6, f"True + 5: got {result2}"

        result3 = add_bool_19(False, 0)
        assert result3 == 0, f"False + 0: got {result3}"

        # int still works
        assert add_bool_19(3, 7) == 10

        print("PASS  Test 19: bool operands (subclass of int)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: bool operands — {e}")
        failed += 1

    # ── Test 20: List/str multiply deopt (int * list, int * str) ─────────

    def mul_repeat_20(a, b):
        return a * b

    for _ in range(WARMUP):
        mul_repeat_20(3, 7)

    check_jit_compiled(mul_repeat_20, "mul_repeat_20")

    try:
        assert mul_repeat_20(3, 7) == 21

        # int * str -> string repetition (deopt)
        str_result = mul_repeat_20(3, "ab")
        assert str_result == "ababab", f"got {str_result!r}"

        # str * int
        str_result2 = mul_repeat_20("xy", 2)
        assert str_result2 == "xyxy", f"got {str_result2!r}"

        # int * list -> list repetition (deopt)
        list_result = mul_repeat_20(3, [1, 2])
        assert list_result == [1, 2, 1, 2, 1, 2], f"got {list_result}"

        # int still works
        assert mul_repeat_20(3, 7) == 21

        print("PASS  Test 20: multiply deopt to str/list repetition")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: multiply str/list repetition — {e}")
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
