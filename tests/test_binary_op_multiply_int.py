#!/usr/bin/env python3
"""
test_binary_op_multiply_int.py — Correctness and deopt tests for
BINARY_OP_MULTIPLY_INT specialisation.

Targets: BINARY_OP_MULTIPLY_INT.

BINARY_OP_MULTIPLY_INT specialises binary multiplication (a * b) when
both operands are integers. Instead of going through the generic
BINARY_OP path (which must check types and dispatch to __mul__/__rmul__),
the specialisation directly calls the integer multiplication fast path.

The adaptive specialiser emits BINARY_OP_MULTIPLY_INT after observing
repeated multiplication of integer operands.

Deopt triggers:
  - One or both operands are not int (float, str, list, custom __mul__)
  - Operand type changes between calls

Tests cover:
  - Basic integer multiplication
  - Multiplication by zero
  - Multiplication by one (identity)
  - Negative integers
  - Large integers (overflow to arbitrary precision)
  - Multiplication producing zero from non-zero operands (impossible for int,
    but verifies no false optimisation)
  - Deopt: switch to float operand
  - Deopt: switch to string repetition (str * int)
  - Deopt: switch to list repetition (list * int)
  - Deopt: switch to custom __mul__
  - Accumulator loop
  - Factorial pattern
  - Rapid type alternation (int vs float)
  - Commutativity (a*b == b*a)
  - Power of 2 multiplication
  - Very large product (1000-digit numbers)
  - Mixed positive and negative
  - Boolean operands (bool is int subclass)
  - Chained multiplication (a * b * c)
  - Operator vs int.__mul__ equivalence

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Mathematical properties preserved (commutativity, identity, etc.)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_multiply_int.py
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
    print("=== BINARY_OP_MULTIPLY_INT Correctness & Deopt Tests ===")
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

    # ------------------------------------------------------------------ #
    # Test 1: Basic integer multiplication
    # ------------------------------------------------------------------ #
    try:
        def mul_int(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_int(3, 7)

        check_jit_compiled(mul_int, "mul_int")
        assert mul_int(3, 7) == 21, f"3*7=21"
        assert mul_int(6, 8) == 48
        assert mul_int(100, 200) == 20000
        print("  PASS: test_basic_integer_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_integer_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Multiplication by zero
    # ------------------------------------------------------------------ #
    try:
        def mul_zero(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_zero(5, 0)

        check_jit_compiled(mul_zero, "mul_zero")
        assert mul_zero(5, 0) == 0
        assert mul_zero(0, 5) == 0
        assert mul_zero(0, 0) == 0
        assert mul_zero(-999, 0) == 0
        assert mul_zero(0, -999) == 0
        print("  PASS: test_multiplication_by_zero")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiplication_by_zero — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Multiplication by one (identity)
    # ------------------------------------------------------------------ #
    try:
        def mul_one(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_one(42, 1)

        check_jit_compiled(mul_one, "mul_one")
        for val in [0, 1, -1, 42, -42, 10**10, -(10**10)]:
            assert mul_one(val, 1) == val, f"{val}*1 should be {val}"
            assert mul_one(1, val) == val, f"1*{val} should be {val}"
        print("  PASS: test_multiplication_by_one")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiplication_by_one — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Negative integers
    # ------------------------------------------------------------------ #
    try:
        def mul_neg(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_neg(-3, 7)

        check_jit_compiled(mul_neg, "mul_neg")
        assert mul_neg(-3, 7) == -21
        assert mul_neg(3, -7) == -21
        assert mul_neg(-3, -7) == 21
        assert mul_neg(-1, -1) == 1
        assert mul_neg(-1, 1) == -1
        print("  PASS: test_negative_integers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_integers — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: Large integers (arbitrary precision)
    # ------------------------------------------------------------------ #
    try:
        def mul_large(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_large(10**50, 2)

        check_jit_compiled(mul_large, "mul_large")
        result = mul_large(10**50, 10**50)
        assert result == 10**100, "10^50 * 10^50 should be 10^100"

        # Verify with known large product
        a = 2**64  # Exceeds C long
        b = 3**40
        expected = a * b  # Computed by Python's arbitrary precision
        assert mul_large(a, b) == expected
        print("  PASS: test_large_integers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_integers — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: Multiplication result sign rules
    # ------------------------------------------------------------------ #
    try:
        def mul_sign(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_sign(2, 3)

        check_jit_compiled(mul_sign, "mul_sign")
        # pos * pos = pos
        assert mul_sign(5, 7) > 0
        # pos * neg = neg
        assert mul_sign(5, -7) < 0
        # neg * pos = neg
        assert mul_sign(-5, 7) < 0
        # neg * neg = pos
        assert mul_sign(-5, -7) > 0
        # anything * 0 = 0
        assert mul_sign(5, 0) == 0
        assert mul_sign(-5, 0) == 0
        print("  PASS: test_sign_rules")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_sign_rules — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Deopt — switch to float operand
    # ------------------------------------------------------------------ #
    try:
        def mul_deopt_float(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_float(3, 7)

        check_jit_compiled(mul_deopt_float, "mul_deopt_float")
        assert mul_deopt_float(3, 7) == 21

        # Switch to float — should deopt
        result = mul_deopt_float(3.0, 7)
        assert result == 21.0 and isinstance(result, float), (
            f"Float deopt: expected 21.0 (float), got {result} ({type(result).__name__})"
        )
        print("  PASS: test_deopt_to_float")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_float — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: Deopt — switch to string repetition (str * int)
    # ------------------------------------------------------------------ #
    try:
        def mul_deopt_str(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_str(5, 3)

        check_jit_compiled(mul_deopt_str, "mul_deopt_str")
        assert mul_deopt_str(5, 3) == 15

        # str * int — should deopt
        result = mul_deopt_str("ab", 3)
        assert result == "ababab", f"String repetition: expected 'ababab', got {result}"
        print("  PASS: test_deopt_to_string_repetition")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_string_repetition — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Deopt — switch to list repetition (list * int)
    # ------------------------------------------------------------------ #
    try:
        def mul_deopt_list(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_list(4, 5)

        check_jit_compiled(mul_deopt_list, "mul_deopt_list")
        assert mul_deopt_list(4, 5) == 20

        # list * int — should deopt
        result = mul_deopt_list([1, 2], 3)
        assert result == [1, 2, 1, 2, 1, 2], f"List repetition: got {result}"
        print("  PASS: test_deopt_to_list_repetition")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_list_repetition — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 10: Deopt — switch to custom __mul__
    # ------------------------------------------------------------------ #
    try:
        class CustomMul:
            def __init__(self, x):
                self.x = x

            def __mul__(self, other):
                return CustomMul(self.x * other * 10)

        def mul_deopt_custom(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_custom(2, 3)

        check_jit_compiled(mul_deopt_custom, "mul_deopt_custom")
        assert mul_deopt_custom(2, 3) == 6

        # Custom __mul__ — should deopt
        cm = CustomMul(5)
        result = mul_deopt_custom(cm, 3)
        assert isinstance(result, CustomMul) and result.x == 150, (
            f"Custom __mul__: 5*3*10=150, got {getattr(result, 'x', result)}"
        )
        print("  PASS: test_deopt_to_custom_mul")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_custom_mul — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: Accumulator loop
    # ------------------------------------------------------------------ #
    try:
        def mul_accum(base, n):
            result = 1
            for _ in range(n):
                result = result * base
            return result

        for _ in range(WARMUP):
            mul_accum(2, 3)

        check_jit_compiled(mul_accum, "mul_accum")
        assert mul_accum(2, 10) == 1024, f"2^10=1024"
        assert mul_accum(3, 5) == 243, f"3^5=243"
        assert mul_accum(1, 1000) == 1
        assert mul_accum(0, 5) == 0
        print("  PASS: test_accumulator_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_accumulator_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: Factorial pattern
    # ------------------------------------------------------------------ #
    try:
        def factorial(n):
            result = 1
            for i in range(1, n + 1):
                result = result * i
            return result

        for _ in range(WARMUP):
            factorial(5)

        check_jit_compiled(factorial, "factorial")
        assert factorial(0) == 1
        assert factorial(1) == 1
        assert factorial(5) == 120
        assert factorial(10) == 3628800
        assert factorial(20) == 2432902008176640000
        print("  PASS: test_factorial_pattern")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_factorial_pattern — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: Rapid type alternation (int vs float)
    # ------------------------------------------------------------------ #
    try:
        def mul_alt(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_alt(3, 7)

        check_jit_compiled(mul_alt, "mul_alt")

        ok = True
        for i in range(50):
            ri = mul_alt(3, 7)
            rf = mul_alt(3.0, 7.0)
            if ri != 21 or not isinstance(ri, int):
                print(f"  FAIL: int iteration {i}: expected 21 (int), got {ri}")
                ok = False
                break
            if rf != 21.0 or not isinstance(rf, float):
                print(f"  FAIL: float iteration {i}: expected 21.0 (float), got {rf}")
                ok = False
                break

        if ok:
            print("  PASS: test_rapid_type_alternation")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: Commutativity (a*b == b*a)
    # ------------------------------------------------------------------ #
    try:
        def mul_comm_ab(a, b):
            return a * b

        def mul_comm_ba(a, b):
            return b * a

        for _ in range(WARMUP):
            mul_comm_ab(3, 7)
            mul_comm_ba(3, 7)

        check_jit_compiled(mul_comm_ab, "mul_comm_ab")

        for a, b in [(0, 0), (1, 0), (3, 7), (-5, 8), (-3, -4),
                      (10**20, 10**30), (-(10**10), 10**10)]:
            ab = mul_comm_ab(a, b)
            ba = mul_comm_ba(a, b)
            assert ab == ba, f"Commutativity: {a}*{b}={ab} but {b}*{a}={ba}"

        print("  PASS: test_commutativity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_commutativity — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Power of 2 multiplication
    # ------------------------------------------------------------------ #
    try:
        def mul_pow2(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_pow2(7, 2)

        check_jit_compiled(mul_pow2, "mul_pow2")

        # Multiplying by powers of 2 (common optimisation target)
        assert mul_pow2(7, 2) == 14
        assert mul_pow2(7, 4) == 28
        assert mul_pow2(7, 8) == 56
        assert mul_pow2(7, 16) == 112
        assert mul_pow2(7, 1024) == 7168
        assert mul_pow2(1, 2**63) == 2**63
        print("  PASS: test_power_of_2_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_power_of_2_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: Very large product (1000-digit numbers)
    # ------------------------------------------------------------------ #
    try:
        def mul_huge(a, b):
            return a * b

        big_a = 10**500 + 7
        big_b = 10**500 + 3

        for _ in range(WARMUP):
            mul_huge(big_a, big_b)

        check_jit_compiled(mul_huge, "mul_huge")
        result = mul_huge(big_a, big_b)
        expected = big_a * big_b
        assert result == expected, "1000-digit multiplication mismatch"
        # Verify it's actually large
        assert len(str(result)) > 999
        print("  PASS: test_very_large_product")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_very_large_product — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Mixed positive and negative sequence
    # ------------------------------------------------------------------ #
    try:
        def mul_sequence(lst):
            result = 1
            for x in lst:
                result = result * x
            return result

        data = [2, -3, 4, -5, 6]
        for _ in range(WARMUP):
            mul_sequence(data[:2])

        check_jit_compiled(mul_sequence, "mul_sequence")
        # 2 * (-3) * 4 * (-5) * 6 = 720
        result = mul_sequence(data)
        assert result == 720, f"Expected 720, got {result}"
        # Odd number of negatives -> negative
        assert mul_sequence([-1, -1, -1]) == -1
        # Even number of negatives -> positive
        assert mul_sequence([-1, -1]) == 1
        print("  PASS: test_mixed_positive_negative")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_positive_negative — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Boolean operands (bool is int subclass)
    # ------------------------------------------------------------------ #
    try:
        def mul_bool(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_bool(3, 7)

        check_jit_compiled(mul_bool, "mul_bool")

        # True == 1, False == 0 for arithmetic
        assert mul_bool(True, 5) == 5
        assert mul_bool(False, 5) == 0
        assert mul_bool(True, True) == 1
        assert mul_bool(False, False) == 0
        assert mul_bool(True, -3) == -3
        # Result type: bool * int -> int (not bool)
        r = mul_bool(True, 5)
        assert r == 5 and isinstance(r, int)
        print("  PASS: test_boolean_operands")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_operands — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: Chained multiplication (a * b * c)
    # ------------------------------------------------------------------ #
    try:
        def mul_chain(a, b, c):
            return a * b * c

        for _ in range(WARMUP):
            mul_chain(2, 3, 5)

        check_jit_compiled(mul_chain, "mul_chain")
        assert mul_chain(2, 3, 5) == 30
        assert mul_chain(0, 100, 200) == 0
        assert mul_chain(-1, -1, -1) == -1
        assert mul_chain(10, 10, 10) == 1000
        print("  PASS: test_chained_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 20: Operator vs int.__mul__ equivalence
    # ------------------------------------------------------------------ #
    try:
        def via_operator(a, b):
            return a * b

        for _ in range(WARMUP):
            via_operator(3, 7)

        check_jit_compiled(via_operator, "via_operator")

        for a, b in [(0, 0), (1, 1), (3, 7), (-5, 8), (-3, -4),
                      (10**20, 7), (0, 10**30)]:
            op_result = via_operator(a, b)
            dunder_result = int.__mul__(a, b)
            assert op_result == dunder_result, (
                f"Mismatch for {a}*{b}: op={op_result}, __mul__={dunder_result}"
            )

        print("  PASS: test_operator_vs_dunder_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_operator_vs_dunder_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Summary
    # ------------------------------------------------------------------ #
    print()
    print(f"BINARY_OP_MULTIPLY_INT: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
