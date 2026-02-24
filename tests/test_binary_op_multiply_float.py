#!/usr/bin/env python3
"""
test_binary_op_multiply_float.py — Correctness and deopt tests for
BINARY_OP_MULTIPLY_FLOAT specialisation.

Targets: BINARY_OP_MULTIPLY_FLOAT.

BINARY_OP_MULTIPLY_FLOAT specialises binary multiplication (a * b) when both
operands are floats. Instead of going through the generic BINARY_OP path
(which must check types and dispatch to __mul__/__rmul__), the specialisation
directly multiplies the underlying C double values.

The adaptive specialiser emits BINARY_OP_MULTIPLY_FLOAT after observing
repeated multiplication of float operands.

Deopt triggers:
  - One or both operands are not float (int, complex, custom __mul__)
  - Operand type changes between calls

Tests cover:
  - Basic multiplication
  - Multiplication by one (identity)
  - Multiplication by zero
  - Multiplication with negative operands
  - Small float multiplication (precision)
  - Large float multiplication (overflow to inf)
  - Subnormal results (underflow)
  - Infinity multiplication
  - NaN propagation
  - Deopt: switch to int operand
  - Deopt: switch to complex operand
  - Deopt: switch to custom __mul__
  - Accumulator loop (product of factors)
  - Rapid float-vs-int alternation
  - Commutativity (a*b == b*a)
  - Associativity failure (floating point)
  - Signed zero (sign rules for zero results)
  - Chained multiplication (a*b*c)
  - Mixed positive and negative
  - Equivalence: (a*b) vs float.__mul__(a, b)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. IEEE 754 properties preserved

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_multiply_float.py
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
    # Test 1: Basic multiplication
    # ------------------------------------------------------------------
    try:
        def mul_float(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_float(3.0, 4.0)
        check_jit_compiled(mul_float, "mul_float")

        assert mul_float(3.0, 4.0) == 12.0
        assert mul_float(2.5, 4.0) == 10.0
        assert mul_float(0.5, 0.5) == 0.25
        assert mul_float(1.5, 2.0) == 3.0
        print("  PASS: test_basic_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Multiplication by one (identity)
    # ------------------------------------------------------------------
    try:
        def mul_one(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_one(42.0, 1.0)
        check_jit_compiled(mul_one, "mul_one")

        assert mul_one(42.0, 1.0) == 42.0
        assert mul_one(1.0, 42.0) == 42.0
        assert mul_one(-42.0, 1.0) == -42.0
        assert mul_one(3.14159, 1.0) == 3.14159
        assert mul_one(1e308, 1.0) == 1e308
        assert mul_one(0.0, 1.0) == 0.0
        print("  PASS: test_multiply_by_one")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiply_by_one — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Multiplication by zero
    # ------------------------------------------------------------------
    try:
        def mul_zero(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_zero(42.0, 0.0)
        check_jit_compiled(mul_zero, "mul_zero")

        assert mul_zero(42.0, 0.0) == 0.0
        assert mul_zero(0.0, 42.0) == 0.0
        assert mul_zero(0.0, 0.0) == 0.0
        assert mul_zero(-42.0, 0.0) == 0.0  # -0.0 == 0.0 in Python
        assert mul_zero(1e308, 0.0) == 0.0
        # inf * 0 = NaN (IEEE 754 special case)
        assert math.isnan(mul_zero(float('inf'), 0.0))
        print("  PASS: test_multiply_by_zero")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiply_by_zero — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Multiplication with negative operands
    # ------------------------------------------------------------------
    try:
        def mul_neg(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_neg(-3.0, 4.0)
        check_jit_compiled(mul_neg, "mul_neg")

        # neg * pos = neg
        assert mul_neg(-3.0, 4.0) == -12.0
        # pos * neg = neg
        assert mul_neg(3.0, -4.0) == -12.0
        # neg * neg = pos
        assert mul_neg(-3.0, -4.0) == 12.0
        # pos * pos = pos
        assert mul_neg(3.0, 4.0) == 12.0
        print("  PASS: test_negative_operands")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_operands — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Small float multiplication (precision)
    # ------------------------------------------------------------------
    try:
        def mul_precise(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_precise(0.1, 0.2)
        check_jit_compiled(mul_precise, "mul_precise")

        # 0.1 * 0.2 is not exactly 0.02 in IEEE 754
        result = mul_precise(0.1, 0.2)
        assert abs(result - 0.02) < 1e-17, f"Expected ~0.02, got {result}"

        # 0.1 * 10.0 should be exactly 1.0
        assert mul_precise(0.1, 10.0) == 1.0

        # 1/3 * 3 is not exactly 1.0
        result2 = mul_precise(1.0/3.0, 3.0)
        assert abs(result2 - 1.0) < 1e-15, f"Expected ~1.0, got {result2}"
        print("  PASS: test_small_float_precision")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_small_float_precision — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Large float multiplication (overflow to inf)
    # ------------------------------------------------------------------
    try:
        def mul_large(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_large(1e100, 1e100)
        check_jit_compiled(mul_large, "mul_large")

        assert mul_large(1e100, 1e100) == 1e200
        assert mul_large(1e200, 1e100) == 1e300
        # Overflow to infinity
        assert mul_large(1e308, 10.0) == float('inf')
        assert mul_large(-1e308, 10.0) == float('-inf')
        # Just below overflow
        assert mul_large(1e308, 1.0) == 1e308
        print("  PASS: test_large_float_overflow")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_float_overflow — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Subnormal results (underflow)
    # ------------------------------------------------------------------
    try:
        def mul_subnormal(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_subnormal(1.0, 0.5)
        check_jit_compiled(mul_subnormal, "mul_subnormal")

        tiny = sys.float_info.min  # ~2.2e-308
        # Multiplying two small normals can produce a subnormal
        result = mul_subnormal(tiny, 0.5)
        assert result > 0.0, "Subnormal result should be positive"
        assert result < tiny, "Result should be smaller than min normal"

        # Very small * very small = underflow to zero
        result2 = mul_subnormal(5e-324, 0.5)
        assert result2 == 0.0, "Expected gradual underflow to zero"

        # Smallest positive * 1.0 preserves value
        smallest = 5e-324
        assert mul_subnormal(smallest, 1.0) == smallest
        print("  PASS: test_subnormal_underflow")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subnormal_underflow — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Infinity multiplication
    # ------------------------------------------------------------------
    try:
        def mul_inf(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_inf(1.0, 2.0)
        check_jit_compiled(mul_inf, "mul_inf")

        inf = float('inf')

        # inf * positive = inf
        assert mul_inf(inf, 42.0) == inf
        # inf * negative = -inf
        assert mul_inf(inf, -42.0) == -inf
        # -inf * negative = inf
        assert mul_inf(-inf, -42.0) == inf
        # inf * inf = inf
        assert mul_inf(inf, inf) == inf
        # -inf * -inf = inf
        assert mul_inf(-inf, -inf) == inf
        # inf * -inf = -inf
        assert mul_inf(inf, -inf) == -inf
        # inf * 0 = NaN
        assert math.isnan(mul_inf(inf, 0.0))
        print("  PASS: test_infinity_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_infinity_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: NaN propagation
    # ------------------------------------------------------------------
    try:
        def mul_nan(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_nan(1.0, 2.0)
        check_jit_compiled(mul_nan, "mul_nan")

        nan = float('nan')
        assert math.isnan(mul_nan(nan, 1.0))
        assert math.isnan(mul_nan(1.0, nan))
        assert math.isnan(mul_nan(nan, nan))
        assert math.isnan(mul_nan(nan, 0.0))
        assert math.isnan(mul_nan(0.0, nan))
        assert math.isnan(mul_nan(nan, float('inf')))
        print("  PASS: test_nan_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nan_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt float -> int
    # ------------------------------------------------------------------
    try:
        def mul_deopt_int(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_int(3.0, 4.0)
        check_jit_compiled(mul_deopt_int, "mul_deopt_int")

        assert mul_deopt_int(3.0, 4.0) == 12.0
        # Deopt: int operands
        assert mul_deopt_int(3, 4) == 12
        assert mul_deopt_int(3, 4.0) == 12.0  # mixed
        assert mul_deopt_int(3.0, 4) == 12.0  # mixed
        # Back to float
        assert mul_deopt_int(3.0, 4.0) == 12.0
        print("  PASS: test_deopt_float_to_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt float -> complex
    # ------------------------------------------------------------------
    try:
        def mul_deopt_complex(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_complex(3.0, 4.0)
        check_jit_compiled(mul_deopt_complex, "mul_deopt_complex")

        assert mul_deopt_complex(3.0, 4.0) == 12.0
        # Deopt: complex operands
        assert mul_deopt_complex(3+0j, 4+0j) == 12+0j
        assert mul_deopt_complex(1+2j, 3+4j) == (1*3 - 2*4) + (1*4 + 2*3)*1j  # -5+10j
        # Back to float
        assert mul_deopt_complex(3.0, 4.0) == 12.0
        print("  PASS: test_deopt_float_to_complex")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_complex — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt float -> custom __mul__
    # ------------------------------------------------------------------
    try:
        class Scale:
            def __init__(self, val):
                self.val = val
            def __mul__(self, other):
                if isinstance(other, Scale):
                    return Scale(self.val * other.val)
                return Scale(self.val * other)
            def __rmul__(self, other):
                return Scale(other * self.val)

        def mul_deopt_custom(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt_custom(3.0, 4.0)
        check_jit_compiled(mul_deopt_custom, "mul_deopt_custom")

        assert mul_deopt_custom(3.0, 4.0) == 12.0
        # Deopt: custom __mul__
        result = mul_deopt_custom(Scale(3), Scale(4))
        assert result.val == 12
        result2 = mul_deopt_custom(Scale(5), 2)
        assert result2.val == 10
        # Back to float
        assert mul_deopt_custom(3.0, 4.0) == 12.0
        print("  PASS: test_deopt_custom_mul")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_custom_mul — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Accumulator loop (product of factors)
    # ------------------------------------------------------------------
    try:
        def product_of_factors(values):
            result = 1.0
            for v in values:
                result *= v
            return result

        data = [1.01] * 100  # 1.01^100
        for _ in range(WARMUP):
            product_of_factors(data)
        check_jit_compiled(product_of_factors, "product_of_factors")

        # 1.01^100 ≈ 2.7048
        result = product_of_factors(data)
        assert abs(result - 1.01**100) < 1e-10, f"Expected ~{1.01**100}, got {result}"
        assert product_of_factors([2.0, 3.0, 4.0]) == 24.0
        assert product_of_factors([1.0]) == 1.0
        assert product_of_factors([]) == 1.0
        print("  PASS: test_accumulator_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_accumulator_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid float-vs-int alternation
    # ------------------------------------------------------------------
    try:
        def mul_poly(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_poly(3.0, 4.0)
        check_jit_compiled(mul_poly, "mul_poly")

        for cycle in range(50):
            r_float = mul_poly(3.0, 4.0)
            r_int = mul_poly(3, 4)
            assert r_float == 12.0, f"float mul failed at cycle {cycle}"
            assert r_int == 12, f"int mul failed at cycle {cycle}"
        print("  PASS: test_rapid_float_int_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_float_int_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Commutativity (a*b == b*a)
    # ------------------------------------------------------------------
    try:
        def mul_commute(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_commute(5.0, 3.0)
        check_jit_compiled(mul_commute, "mul_commute")

        pairs = [
            (5.0, 3.0), (0.0, 1.0), (-2.0, 3.0),
            (1e100, 1e-100), (3.14, 2.72), (0.1, 0.2),
        ]
        for a, b in pairs:
            ab = mul_commute(a, b)
            ba = mul_commute(b, a)
            assert ab == ba, (
                f"Commutativity failed for ({a}, {b}): "
                f"a*b={ab}, b*a={ba}"
            )
        print("  PASS: test_commutativity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_commutativity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Associativity failure (floating point)
    # ------------------------------------------------------------------
    try:
        def mul_left(a, b, c):
            return (a * b) * c

        def mul_right(a, b, c):
            return a * (b * c)

        for _ in range(WARMUP):
            mul_left(1.1, 2.2, 3.3)
        for _ in range(WARMUP):
            mul_right(1.1, 2.2, 3.3)
        check_jit_compiled(mul_left, "mul_left")
        check_jit_compiled(mul_right, "mul_right")

        # For exact values they should agree
        r1 = mul_left(2.0, 3.0, 4.0)
        r2 = mul_right(2.0, 3.0, 4.0)
        assert r1 == r2 == 24.0

        # For precision-sensitive values they may differ
        r3 = mul_left(1.1, 2.2, 3.3)
        r4 = mul_right(1.1, 2.2, 3.3)
        # Both should be close to the true value
        expected = 1.1 * 2.2 * 3.3
        assert abs(r3 - expected) < 1e-14
        assert abs(r4 - expected) < 1e-14
        print("  PASS: test_associativity_failure")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_associativity_failure — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Signed zero (IEEE 754 sign rules)
    # ------------------------------------------------------------------
    try:
        def mul_signed_zero(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_signed_zero(1.0, 2.0)
        check_jit_compiled(mul_signed_zero, "mul_signed_zero")

        # +0 * +x = +0
        r1 = mul_signed_zero(0.0, 42.0)
        assert r1 == 0.0 and not math.copysign(1.0, r1) < 0

        # -0 * +x = -0 (IEEE 754: negative * positive = negative)
        r2 = mul_signed_zero(-0.0, 42.0)
        assert r2 == 0.0 and math.copysign(1.0, r2) < 0

        # +0 * -x = -0
        r3 = mul_signed_zero(0.0, -42.0)
        assert r3 == 0.0 and math.copysign(1.0, r3) < 0

        # -0 * -x = +0
        r4 = mul_signed_zero(-0.0, -42.0)
        assert r4 == 0.0 and math.copysign(1.0, r4) > 0

        print("  PASS: test_signed_zero")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_signed_zero — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained multiplication (a * b * c)
    # ------------------------------------------------------------------
    try:
        def mul_chained(a, b, c):
            return a * b * c

        for _ in range(WARMUP):
            mul_chained(2.0, 3.0, 4.0)
        check_jit_compiled(mul_chained, "mul_chained")

        assert mul_chained(2.0, 3.0, 4.0) == 24.0
        assert mul_chained(0.5, 0.5, 4.0) == 1.0
        assert mul_chained(0.0, 100.0, 100.0) == 0.0
        assert mul_chained(-1.0, -1.0, -1.0) == -1.0
        assert mul_chained(10.0, 10.0, 10.0) == 1000.0
        print("  PASS: test_chained_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Mixed positive and negative
    # ------------------------------------------------------------------
    try:
        def mul_mixed(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_mixed(-5.0, 3.0)
        check_jit_compiled(mul_mixed, "mul_mixed")

        # Sign rules
        assert mul_mixed(-5.0, 3.0) == -15.0
        assert mul_mixed(5.0, -3.0) == -15.0
        assert mul_mixed(-5.0, -3.0) == 15.0
        assert mul_mixed(5.0, 3.0) == 15.0

        # Multiply by -1 negates
        assert mul_mixed(42.0, -1.0) == -42.0
        assert mul_mixed(-42.0, -1.0) == 42.0
        assert mul_mixed(0.0, -1.0) == 0.0  # -0.0 == 0.0
        print("  PASS: test_mixed_positive_negative")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_positive_negative — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — (a * b) vs float.__mul__(a, b)
    # ------------------------------------------------------------------
    try:
        def mul_operator(a, b):
            return a * b

        def mul_dunder(a, b):
            return float.__mul__(a, b)

        for _ in range(WARMUP):
            mul_operator(3.0, 4.0)
        check_jit_compiled(mul_operator, "mul_operator")

        pairs = [
            (3.0, 4.0), (0.0, 0.0), (-1.0, 1.0),
            (3.14, 2.72), (1e100, 1e-100), (0.1, 0.2),
            (1e-300, 1e10), (1e308, 1.0), (-0.0, 1.0),
        ]
        for a, b in pairs:
            r_op = mul_operator(a, b)
            r_du = mul_dunder(a, b)
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
    print(f"\nBINARY_OP_MULTIPLY_FLOAT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
