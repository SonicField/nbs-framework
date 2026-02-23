#!/usr/bin/env python3
"""
test_binary_op_subtract_float.py — Correctness and deopt tests for
BINARY_OP_SUBTRACT_FLOAT specialisation.

Targets: BINARY_OP_SUBTRACT_FLOAT.

BINARY_OP_SUBTRACT_FLOAT specialises binary subtraction (a - b) when both
operands are floats. Instead of going through the generic BINARY_OP path
(which must check types and dispatch to __sub__/__rsub__), the specialisation
directly subtracts the underlying C double values.

The adaptive specialiser emits BINARY_OP_SUBTRACT_FLOAT after observing
repeated subtraction of float operands.

Deopt triggers:
  - One or both operands are not float (int, complex, custom __sub__)
  - Operand type changes between calls

Tests cover:
  - Basic subtraction
  - Subtraction producing zero
  - Subtraction with negative results
  - Subtraction by zero (identity)
  - Small float differences (precision)
  - Large float subtraction
  - Subnormal results
  - Infinity subtraction
  - NaN propagation
  - Deopt: switch to int operand
  - Deopt: switch to complex operand
  - Deopt: switch to custom __sub__
  - Accumulator loop (sum of differences)
  - Rapid float-vs-int alternation
  - Anti-commutativity (a-b == -(b-a))
  - Associativity failure (floating point)
  - Subtraction from self (a-a == 0.0)
  - Chained subtraction (a-b-c)
  - Mixed positive and negative
  - Equivalence: (a-b) vs float.__sub__(a, b)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. IEEE 754 properties preserved

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_subtract_float.py
"""

import sys
import math

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
        def sub_float(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_float(10.0, 3.0)
        check_jit_compiled(sub_float, "sub_float")

        assert sub_float(10.0, 3.0) == 7.0
        assert sub_float(100.5, 50.5) == 50.0
        assert sub_float(1.0, 0.5) == 0.5
        assert sub_float(3.14, 1.14) == 2.0
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
            sub_to_zero(5.0, 5.0)
        check_jit_compiled(sub_to_zero, "sub_to_zero")

        assert sub_to_zero(5.0, 5.0) == 0.0
        assert sub_to_zero(0.0, 0.0) == 0.0
        assert sub_to_zero(-3.14, -3.14) == 0.0
        assert sub_to_zero(1e100, 1e100) == 0.0
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
            sub_negative(3.0, 10.0)
        check_jit_compiled(sub_negative, "sub_negative")

        assert sub_negative(3.0, 10.0) == -7.0
        assert sub_negative(0.0, 5.0) == -5.0
        assert sub_negative(-3.0, 4.0) == -7.0
        assert sub_negative(-3.0, -10.0) == 7.0
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
            sub_zero(42.0, 0.0)
        check_jit_compiled(sub_zero, "sub_zero")

        assert sub_zero(42.0, 0.0) == 42.0
        assert sub_zero(-42.0, 0.0) == -42.0
        assert sub_zero(0.0, 0.0) == 0.0
        assert sub_zero(3.14159, 0.0) == 3.14159
        assert sub_zero(1e308, 0.0) == 1e308
        print("  PASS: test_subtract_zero_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subtract_zero_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Small float differences (precision)
    # ------------------------------------------------------------------
    try:
        def sub_precise(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_precise(1.0, 0.1)
        check_jit_compiled(sub_precise, "sub_precise")

        # 1.0 - 0.1 is not exactly 0.9 in IEEE 754
        result = sub_precise(1.0, 0.1)
        assert abs(result - 0.9) < 1e-15, f"Expected ~0.9, got {result}"

        # 0.3 - 0.1 is not exactly 0.2
        result2 = sub_precise(0.3, 0.1)
        assert abs(result2 - 0.2) < 1e-15, f"Expected ~0.2, got {result2}"

        # Catastrophic cancellation
        result3 = sub_precise(1.0000000000000002, 1.0)
        assert result3 > 0.0, "Catastrophic cancellation lost the difference"
        print("  PASS: test_small_float_precision")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_small_float_precision — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Large float subtraction
    # ------------------------------------------------------------------
    try:
        def sub_large(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_large(1e100, 1e99)
        check_jit_compiled(sub_large, "sub_large")

        assert sub_large(1e100, 1e99) == 9e99
        assert sub_large(1e308, 1e307) == 9e307
        assert sub_large(1e200, 1e200) == 0.0
        assert sub_large(-1e100, -1e99) == -9e99
        print("  PASS: test_large_float_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_float_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Subnormal results
    # ------------------------------------------------------------------
    try:
        def sub_subnormal(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_subnormal(1.0, 0.5)
        check_jit_compiled(sub_subnormal, "sub_subnormal")

        # Smallest normal float
        tiny = sys.float_info.min  # ~2.2e-308
        result = sub_subnormal(tiny, tiny / 2.0)
        assert result > 0.0, "Subnormal result should be positive"
        assert result < tiny, "Result should be subnormal"

        # Difference of very small numbers
        result2 = sub_subnormal(5e-324, 0.0)
        assert result2 > 0.0
        print("  PASS: test_subnormal_results")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subnormal_results — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Infinity subtraction
    # ------------------------------------------------------------------
    try:
        def sub_inf(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_inf(1.0, 2.0)
        check_jit_compiled(sub_inf, "sub_inf")

        inf = float('inf')

        # inf - finite = inf
        assert sub_inf(inf, 42.0) == inf
        # -inf - finite = -inf
        assert sub_inf(-inf, 42.0) == -inf
        # finite - inf = -inf
        assert sub_inf(42.0, inf) == -inf
        # inf - inf = NaN
        assert math.isnan(sub_inf(inf, inf))
        # -inf - (-inf) = NaN
        assert math.isnan(sub_inf(-inf, -inf))
        # inf - (-inf) = inf
        assert sub_inf(inf, -inf) == inf
        print("  PASS: test_infinity_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_infinity_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: NaN propagation
    # ------------------------------------------------------------------
    try:
        def sub_nan(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_nan(1.0, 2.0)
        check_jit_compiled(sub_nan, "sub_nan")

        nan = float('nan')
        assert math.isnan(sub_nan(nan, 1.0))
        assert math.isnan(sub_nan(1.0, nan))
        assert math.isnan(sub_nan(nan, nan))
        assert math.isnan(sub_nan(nan, 0.0))
        assert math.isnan(sub_nan(0.0, nan))
        assert math.isnan(sub_nan(nan, float('inf')))
        print("  PASS: test_nan_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nan_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt float -> int
    # ------------------------------------------------------------------
    try:
        def sub_deopt_int(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt_int(10.0, 3.0)
        check_jit_compiled(sub_deopt_int, "sub_deopt_int")

        assert sub_deopt_int(10.0, 3.0) == 7.0
        # Deopt: int operands
        assert sub_deopt_int(10, 3) == 7
        assert sub_deopt_int(10, 3.0) == 7.0  # mixed
        assert sub_deopt_int(10.0, 3) == 7.0  # mixed
        # Back to float
        assert sub_deopt_int(10.0, 3.0) == 7.0
        print("  PASS: test_deopt_float_to_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt float -> complex
    # ------------------------------------------------------------------
    try:
        def sub_deopt_complex(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt_complex(10.0, 3.0)
        check_jit_compiled(sub_deopt_complex, "sub_deopt_complex")

        assert sub_deopt_complex(10.0, 3.0) == 7.0
        # Deopt: complex operands
        assert sub_deopt_complex(10+0j, 3+0j) == 7+0j
        assert sub_deopt_complex(5+3j, 2+1j) == 3+2j
        # Back to float
        assert sub_deopt_complex(10.0, 3.0) == 7.0
        print("  PASS: test_deopt_float_to_complex")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_complex — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt float -> custom __sub__
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
            sub_deopt_custom(10.0, 3.0)
        check_jit_compiled(sub_deopt_custom, "sub_deopt_custom")

        assert sub_deopt_custom(10.0, 3.0) == 7.0
        # Deopt: custom __sub__
        result = sub_deopt_custom(Offset(10), Offset(3))
        assert result.val == 7
        result2 = sub_deopt_custom(Offset(5), 2)
        assert result2.val == 3
        # Back to float
        assert sub_deopt_custom(10.0, 3.0) == 7.0
        print("  PASS: test_deopt_custom_sub")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_custom_sub — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Accumulator loop (sum of differences)
    # ------------------------------------------------------------------
    try:
        def sum_differences(values):
            total = 0.0
            for i in range(1, len(values)):
                total += values[i] - values[i-1]
            return total

        data = [float(i) for i in range(100)]
        for _ in range(WARMUP):
            sum_differences(data)
        check_jit_compiled(sum_differences, "sum_differences")

        # Sum of consecutive differences = last - first
        assert sum_differences(data) == 99.0
        assert sum_differences([0.0]) == 0.0
        assert sum_differences([1.0, 3.0, 6.0, 10.0]) == 9.0  # 10-1
        assert sum_differences([]) == 0.0
        print("  PASS: test_accumulator_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_accumulator_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid float-vs-int alternation
    # ------------------------------------------------------------------
    try:
        def sub_poly(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_poly(10.0, 3.0)
        check_jit_compiled(sub_poly, "sub_poly")

        for cycle in range(50):
            r_float = sub_poly(10.0, 3.0)
            r_int = sub_poly(10, 3)
            assert r_float == 7.0, f"float sub failed at cycle {cycle}"
            assert r_int == 7, f"int sub failed at cycle {cycle}"
        print("  PASS: test_rapid_float_int_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_float_int_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Anti-commutativity (a-b == -(b-a))
    # ------------------------------------------------------------------
    try:
        def sub_anticommute(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_anticommute(5.0, 3.0)
        check_jit_compiled(sub_anticommute, "sub_anticommute")

        pairs = [
            (5.0, 3.0), (0.0, 1.0), (-2.0, 3.0),
            (1e100, 1e99), (3.14, 2.72), (0.1, 0.2),
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
    # Test 16: Associativity failure (floating point)
    # ------------------------------------------------------------------
    try:
        def sub_assoc_left(a, b, c):
            return (a - b) - c

        def sub_assoc_right(a, b, c):
            return a - (b + c)

        for _ in range(WARMUP):
            sub_assoc_left(1.0, 0.1, 0.2)
        for _ in range(WARMUP):
            sub_assoc_right(1.0, 0.1, 0.2)
        check_jit_compiled(sub_assoc_left, "sub_assoc_left")
        check_jit_compiled(sub_assoc_right, "sub_assoc_right")

        # For well-behaved values they should agree closely
        r1 = sub_assoc_left(100.0, 30.0, 20.0)
        r2 = sub_assoc_right(100.0, 30.0, 20.0)
        assert r1 == r2 == 50.0

        # For precision-sensitive values they may differ slightly
        r3 = sub_assoc_left(1.0, 1e-16, 1e-16)
        r4 = sub_assoc_right(1.0, 1e-16, 1e-16)
        # Both should be very close to 1.0
        assert abs(r3 - 1.0) < 1e-14
        assert abs(r4 - 1.0) < 1e-14
        print("  PASS: test_associativity_failure")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_associativity_failure — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Subtraction from self (a-a == 0.0)
    # ------------------------------------------------------------------
    try:
        def sub_self(a):
            return a - a

        for _ in range(WARMUP):
            sub_self(42.0)
        check_jit_compiled(sub_self, "sub_self")

        for val in [0.0, 1.0, -1.0, 3.14, 1e100, -1e100, 1e-300]:
            result = sub_self(val)
            assert result == 0.0, f"a-a != 0.0 for a={val}, got {result}"

        # inf - inf = NaN (special case)
        assert math.isnan(sub_self(float('inf')))
        # NaN - NaN = NaN
        assert math.isnan(sub_self(float('nan')))
        print("  PASS: test_self_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_self_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained subtraction (a - b - c)
    # ------------------------------------------------------------------
    try:
        def sub_chained(a, b, c):
            return a - b - c

        for _ in range(WARMUP):
            sub_chained(100.0, 30.0, 20.0)
        check_jit_compiled(sub_chained, "sub_chained")

        assert sub_chained(100.0, 30.0, 20.0) == 50.0
        assert sub_chained(10.0, 3.0, 2.0) == 5.0
        assert sub_chained(0.0, 0.0, 0.0) == 0.0
        assert sub_chained(1.0, 2.0, 3.0) == -4.0
        assert sub_chained(-1.0, -2.0, -3.0) == 4.0
        print("  PASS: test_chained_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Mixed positive and negative
    # ------------------------------------------------------------------
    try:
        def sub_mixed(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_mixed(-5.0, 3.0)
        check_jit_compiled(sub_mixed, "sub_mixed")

        # neg - pos = neg
        assert sub_mixed(-5.0, 3.0) == -8.0
        # pos - neg = pos (subtraction of negative is addition)
        assert sub_mixed(5.0, -3.0) == 8.0
        # neg - neg
        assert sub_mixed(-5.0, -3.0) == -2.0
        assert sub_mixed(-3.0, -5.0) == 2.0
        # Signed zero
        assert sub_mixed(0.0, 0.0) == 0.0
        assert sub_mixed(-0.0, 0.0) == 0.0
        print("  PASS: test_mixed_positive_negative")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_positive_negative — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — (a - b) vs float.__sub__(a, b)
    # ------------------------------------------------------------------
    try:
        def sub_operator(a, b):
            return a - b

        def sub_dunder(a, b):
            return float.__sub__(a, b)

        for _ in range(WARMUP):
            sub_operator(10.0, 3.0)
        check_jit_compiled(sub_operator, "sub_operator")

        pairs = [
            (10.0, 3.0), (0.0, 0.0), (-1.0, 1.0),
            (3.14, 2.72), (1e100, 1e99), (0.1, 0.2),
            (1e-300, 1e-301), (1e308, 0.0), (-0.0, 0.0),
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
    print(f"\nBINARY_OP_SUBTRACT_FLOAT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
