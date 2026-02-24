#!/usr/bin/env python3
"""
test_binary_op_subtract_int.py — Correctness and deopt tests for
BINARY_OP_SUBTRACT_INT specialisation.

Targets: BINARY_OP_SUBTRACT_INT.

BINARY_OP_SUBTRACT_INT specialises binary subtraction (a - b) when both
operands are ints. Instead of going through the generic BINARY_OP path
(which must check types and dispatch to __sub__/__rsub__), the specialisation
uses the fast nb_subtract slot directly or an inlined integer arithmetic path.

The adaptive specialiser emits BINARY_OP_SUBTRACT_INT after observing
repeated subtraction of int operands.

Deopt triggers:
  - One or both operands are not int (float, complex, custom __sub__)
  - Operand type changes between calls

Tests cover:
  - Basic subtraction
  - Subtraction producing zero
  - Subtraction with negative results
  - Subtraction by zero (identity)
  - Negative operand subtraction
  - Overflow to bigint (sys.maxsize boundary)
  - Large integer subtraction (arbitrary precision)
  - Deopt: switch to float operand
  - Deopt: switch to complex operand
  - Deopt: switch to custom __sub__
  - Deopt: switch to bool (int subclass)
  - Accumulator loop (sum of differences)
  - Rapid int-vs-float alternation
  - Anti-commutativity (a-b == -(b-a))
  - Chained subtraction (a-b-c)
  - Self-subtraction (a-a == 0)
  - Subtraction near sys.maxsize boundary
  - Mixed positive and negative
  - Boolean subtraction (True/False are ints)
  - Equivalence: (a-b) vs int.__sub__(a, b)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Arbitrary precision preserved

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_subtract_int.py
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

    passed = 0
    failed = 0

    # ------------------------------------------------------------------
    # Test 1: Basic subtraction
    # ------------------------------------------------------------------
    try:
        def sub_int(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_int(10, 3)
        check_jit_compiled(sub_int, "sub_int")

        assert sub_int(10, 3) == 7
        assert sub_int(100, 50) == 50
        assert sub_int(1, 1) == 0
        assert sub_int(999, 1) == 998
        print("  PASS: test_basic_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Subtraction producing zero
    # ------------------------------------------------------------------
    try:
        def sub_to_zero(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_to_zero(5, 5)
        check_jit_compiled(sub_to_zero, "sub_to_zero")

        assert sub_to_zero(5, 5) == 0
        assert sub_to_zero(0, 0) == 0
        assert sub_to_zero(-3, -3) == 0
        assert sub_to_zero(10**100, 10**100) == 0
        print("  PASS: test_subtraction_to_zero")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subtraction_to_zero — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Subtraction with negative results
    # ------------------------------------------------------------------
    try:
        def sub_negative(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_negative(3, 10)
        check_jit_compiled(sub_negative, "sub_negative")

        assert sub_negative(3, 10) == -7
        assert sub_negative(0, 5) == -5
        assert sub_negative(-3, 4) == -7
        assert sub_negative(1, 1000000) == -999999
        print("  PASS: test_negative_results")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_results — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Subtraction by zero (identity)
    # ------------------------------------------------------------------
    try:
        def sub_zero(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_zero(42, 0)
        check_jit_compiled(sub_zero, "sub_zero")

        assert sub_zero(42, 0) == 42
        assert sub_zero(-42, 0) == -42
        assert sub_zero(0, 0) == 0
        assert sub_zero(sys.maxsize, 0) == sys.maxsize
        assert sub_zero(-sys.maxsize, 0) == -sys.maxsize
        print("  PASS: test_subtract_zero_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subtract_zero_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Negative operand subtraction
    # ------------------------------------------------------------------
    try:
        def sub_neg_ops(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_neg_ops(-5, -3)
        check_jit_compiled(sub_neg_ops, "sub_neg_ops")

        # neg - neg
        assert sub_neg_ops(-5, -3) == -2
        assert sub_neg_ops(-3, -5) == 2
        # neg - pos
        assert sub_neg_ops(-5, 3) == -8
        # pos - neg (subtraction of negative is addition)
        assert sub_neg_ops(5, -3) == 8
        print("  PASS: test_negative_operands")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_operands — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Overflow to bigint (sys.maxsize boundary)
    # ------------------------------------------------------------------
    try:
        def sub_overflow(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_overflow(10, 3)
        check_jit_compiled(sub_overflow, "sub_overflow")

        # Crossing the maxsize boundary into bigint
        result = sub_overflow(-sys.maxsize, 2)
        assert result == -sys.maxsize - 2
        assert type(result) is int

        # Large negative result
        result2 = sub_overflow(-sys.maxsize - 1, sys.maxsize)
        expected = (-sys.maxsize - 1) - sys.maxsize
        assert result2 == expected

        # Verify still works for normal values after bigint
        assert sub_overflow(10, 3) == 7
        print("  PASS: test_overflow_to_bigint")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overflow_to_bigint — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Large integer subtraction (arbitrary precision)
    # ------------------------------------------------------------------
    try:
        def sub_large(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_large(10, 3)
        check_jit_compiled(sub_large, "sub_large")

        big_a = 10**100
        big_b = 10**99
        assert sub_large(big_a, big_b) == 9 * 10**99

        # Very large values
        huge = 2**1000
        assert sub_large(huge, 1) == huge - 1
        assert sub_large(huge, huge) == 0

        # Negative large
        assert sub_large(-big_a, big_b) == -(big_a + big_b)
        print("  PASS: test_large_integer_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_integer_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt int -> float
    # ------------------------------------------------------------------
    try:
        def sub_deopt_float(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt_float(10, 3)
        check_jit_compiled(sub_deopt_float, "sub_deopt_float")

        assert sub_deopt_float(10, 3) == 7
        # Deopt: float operands
        assert sub_deopt_float(10.0, 3.0) == 7.0
        assert isinstance(sub_deopt_float(10.0, 3.0), float)
        # Mixed
        assert sub_deopt_float(10, 3.0) == 7.0
        assert sub_deopt_float(10.0, 3) == 7.0
        # Back to int
        assert sub_deopt_float(10, 3) == 7
        assert isinstance(sub_deopt_float(10, 3), int)
        print("  PASS: test_deopt_int_to_float")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_float — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt int -> complex
    # ------------------------------------------------------------------
    try:
        def sub_deopt_complex(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt_complex(10, 3)
        check_jit_compiled(sub_deopt_complex, "sub_deopt_complex")

        assert sub_deopt_complex(10, 3) == 7
        # Deopt: complex operands
        assert sub_deopt_complex(10+0j, 3+0j) == 7+0j
        assert sub_deopt_complex(5+3j, 2+1j) == 3+2j
        # Back to int
        assert sub_deopt_complex(10, 3) == 7
        print("  PASS: test_deopt_int_to_complex")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_complex — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt int -> custom __sub__
    # ------------------------------------------------------------------
    try:
        class Offset:
            def __init__(self, val):
                self.val = val
            def __sub__(self, other):
                if isinstance(other, Offset):
                    return Offset(self.val - other.val)
                return Offset(self.val - other)
            def __rsub__(self, other):
                return Offset(other - self.val)

        def sub_deopt_custom(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt_custom(10, 3)
        check_jit_compiled(sub_deopt_custom, "sub_deopt_custom")

        assert sub_deopt_custom(10, 3) == 7
        # Deopt: custom __sub__
        result = sub_deopt_custom(Offset(10), Offset(3))
        assert result.val == 7
        result2 = sub_deopt_custom(Offset(5), 2)
        assert result2.val == 3
        # __rsub__: int - Offset
        result3 = sub_deopt_custom(10, Offset(3))
        assert result3.val == 7
        # Back to int
        assert sub_deopt_custom(10, 3) == 7
        print("  PASS: test_deopt_custom_sub")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_custom_sub — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt int -> bool (int subclass)
    # ------------------------------------------------------------------
    try:
        def sub_deopt_bool(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt_bool(10, 3)
        check_jit_compiled(sub_deopt_bool, "sub_deopt_bool")

        assert sub_deopt_bool(10, 3) == 7
        # bool is a subclass of int — subtraction should work
        assert sub_deopt_bool(True, False) == 1
        assert sub_deopt_bool(True, True) == 0
        assert sub_deopt_bool(False, True) == -1
        # Mixed bool/int
        assert sub_deopt_bool(10, True) == 9
        assert sub_deopt_bool(True, 5) == -4
        # Back to int
        assert sub_deopt_bool(10, 3) == 7
        print("  PASS: test_deopt_int_to_bool")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_bool — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Accumulator loop (sum of differences)
    # ------------------------------------------------------------------
    try:
        def sum_differences(values):
            total = 0
            for i in range(1, len(values)):
                total += values[i] - values[i-1]
            return total

        data = list(range(100))
        for _ in range(WARMUP):
            sum_differences(data)
        check_jit_compiled(sum_differences, "sum_differences")

        # Sum of consecutive differences = last - first
        assert sum_differences(data) == 99
        assert sum_differences([0]) == 0
        assert sum_differences([1, 3, 6, 10]) == 9  # 10 - 1
        assert sum_differences([]) == 0
        assert sum_differences([5, 3, 1]) == -4  # 1 - 5
        print("  PASS: test_accumulator_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_accumulator_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid int-vs-float alternation
    # ------------------------------------------------------------------
    try:
        def sub_poly(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_poly(10, 3)
        check_jit_compiled(sub_poly, "sub_poly")

        for cycle in range(50):
            r_int = sub_poly(10, 3)
            r_float = sub_poly(10.0, 3.0)
            assert r_int == 7, f"int sub failed at cycle {cycle}"
            assert r_float == 7.0, f"float sub failed at cycle {cycle}"
            assert isinstance(r_int, int), f"int result type wrong at cycle {cycle}"
            assert isinstance(r_float, float), f"float result type wrong at cycle {cycle}"
        print("  PASS: test_rapid_int_float_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_int_float_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Anti-commutativity (a-b == -(b-a))
    # ------------------------------------------------------------------
    try:
        def sub_anticommute(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_anticommute(5, 3)
        check_jit_compiled(sub_anticommute, "sub_anticommute")

        pairs = [
            (5, 3), (0, 1), (-2, 3), (100, 99),
            (sys.maxsize, 1), (1, sys.maxsize),
            (10**50, 10**49),
        ]
        for a, b in pairs:
            ab = sub_anticommute(a, b)
            ba = sub_anticommute(b, a)
            assert ab == -ba, (
                f"Anti-commutativity failed for ({a}, {b}): "
                f"a-b={ab}, -(b-a)={-ba}"
            )
        print("  PASS: test_anti_commutativity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_anti_commutativity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Chained subtraction (a - b - c)
    # ------------------------------------------------------------------
    try:
        def sub_chained(a, b, c):
            return a - b - c

        for _ in range(WARMUP):
            sub_chained(100, 30, 20)
        check_jit_compiled(sub_chained, "sub_chained")

        assert sub_chained(100, 30, 20) == 50
        assert sub_chained(10, 3, 2) == 5
        assert sub_chained(0, 0, 0) == 0
        assert sub_chained(1, 2, 3) == -4
        assert sub_chained(-1, -2, -3) == 4
        print("  PASS: test_chained_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Self-subtraction (a - a == 0)
    # ------------------------------------------------------------------
    try:
        def sub_self(a):
            return a - a

        for _ in range(WARMUP):
            sub_self(42)
        check_jit_compiled(sub_self, "sub_self")

        for val in [0, 1, -1, 42, -42, sys.maxsize, -sys.maxsize - 1, 10**100, -(10**100)]:
            result = sub_self(val)
            assert result == 0, f"a-a != 0 for a={val}, got {result}"
        print("  PASS: test_self_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_self_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Subtraction near sys.maxsize boundary
    # ------------------------------------------------------------------
    try:
        def sub_boundary(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_boundary(10, 3)
        check_jit_compiled(sub_boundary, "sub_boundary")

        maxs = sys.maxsize
        mins = -sys.maxsize - 1

        # At the boundary
        assert sub_boundary(maxs, 0) == maxs
        assert sub_boundary(maxs, maxs) == 0
        assert sub_boundary(maxs, -1) == maxs + 1  # crosses into bigint
        assert sub_boundary(mins, 0) == mins
        assert sub_boundary(mins, 1) == mins - 1  # crosses into bigint
        assert sub_boundary(0, mins) == maxs + 1  # crosses into bigint

        # Verify type is still int (Python ints are arbitrary precision)
        result = sub_boundary(maxs, -1)
        assert type(result) is int
        assert result == maxs + 1
        print("  PASS: test_maxsize_boundary")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_maxsize_boundary — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Mixed positive and negative
    # ------------------------------------------------------------------
    try:
        def sub_mixed(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_mixed(-5, 3)
        check_jit_compiled(sub_mixed, "sub_mixed")

        # All sign combinations
        assert sub_mixed(-5, 3) == -8
        assert sub_mixed(5, -3) == 8
        assert sub_mixed(-5, -3) == -2
        assert sub_mixed(5, 3) == 2

        # Subtraction is adding the negation
        assert sub_mixed(5, -3) == 5 + 3
        assert sub_mixed(-5, 3) == -(5 + 3)
        print("  PASS: test_mixed_positive_negative")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_positive_negative — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Boolean subtraction (True/False are ints)
    # ------------------------------------------------------------------
    try:
        def sub_bool(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_bool(10, 3)
        check_jit_compiled(sub_bool, "sub_bool")

        # bool is subclass of int: True==1, False==0
        assert sub_bool(True, False) == 1
        assert sub_bool(False, True) == -1
        assert sub_bool(True, True) == 0
        assert sub_bool(False, False) == 0

        # Result type: bool - bool -> int (not bool)
        result = sub_bool(True, False)
        assert type(result) is int
        assert result == 1

        # Mixed with regular int
        assert sub_bool(10, True) == 9
        assert sub_bool(False, 5) == -5
        print("  PASS: test_boolean_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — (a - b) vs int.__sub__(a, b)
    # ------------------------------------------------------------------
    try:
        def sub_operator(a, b):
            return a - b

        def sub_dunder(a, b):
            return int.__sub__(a, b)

        for _ in range(WARMUP):
            sub_operator(10, 3)
        check_jit_compiled(sub_operator, "sub_operator")

        pairs = [
            (10, 3), (0, 0), (-1, 1), (1, -1),
            (sys.maxsize, 1), (1, sys.maxsize),
            (100, 99), (-100, -99), (0, sys.maxsize),
        ]
        for a, b in pairs:
            r_op = sub_operator(a, b)
            r_du = sub_dunder(a, b)
            assert r_op == r_du, (
                f"Mismatch for ({a}, {b}): "
                f"operator={r_op}, dunder={r_du}"
            )
        print("  PASS: test_equivalence_operator_vs_dunder")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_operator_vs_dunder — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nBINARY_OP_SUBTRACT_INT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
