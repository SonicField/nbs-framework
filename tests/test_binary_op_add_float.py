#!/usr/bin/env python3
"""
test_binary_op_add_float.py — Correctness and deopt tests for BINARY_OP float
arithmetic specialisations.

Targets: BINARY_OP_ADD_FLOAT, BINARY_OP_SUBTRACT_FLOAT, BINARY_OP_MULTIPLY_FLOAT.

These specialisations emit GuardType on both operands to confirm they are float,
then use the fast nb_add/nb_subtract/nb_multiply slot directly (or an inlined
float arithmetic path) instead of generic binary_op dispatch.

When a function is JIT-compiled with float operands and then called with a
different operand type (int, str, custom __add__), the GuardType must fire,
triggering deoptimisation back to the interpreter. The interpreter must then
produce the correct result.

Tests cover:
  - Basic float arithmetic correctness (add, subtract, multiply)
  - Edge cases: zero, negative, very large, very small (subnormal)
  - Infinity and NaN propagation
  - Deopt: float-compiled then called with int operands
  - Deopt: float-compiled then called with complex operands
  - Deopt: float-compiled then called with custom __add__ objects
  - Deopt: mixed type operands (float + int)
  - Accumulator loops (common pattern in real code)
  - Rapid type alternation stability
  - Subtraction and multiplication deopt paths
  - IEEE 754 signed zero behaviour
  - Floating-point precision (no unexpected rounding)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct deopt when operand type changes
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_add_float.py
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
    # Test 1: Basic float addition
    # ------------------------------------------------------------------
    try:
        def float_add(a, b):
            return a + b

        for _ in range(WARMUP):
            float_add(1.0, 2.0)
        check_jit_compiled(float_add, "float_add")

        assert float_add(1.0, 2.0) == 3.0
        assert float_add(0.1, 0.2) == 0.1 + 0.2  # IEEE 754 exact match
        assert float_add(100.5, 200.25) == 300.75
        assert float_add(-1.5, 2.5) == 1.0
        print("  PASS: test_basic_float_addition")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_float_addition — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Basic float subtraction
    # ------------------------------------------------------------------
    try:
        def float_sub(a, b):
            return a - b

        for _ in range(WARMUP):
            float_sub(5.0, 3.0)
        check_jit_compiled(float_sub, "float_sub")

        assert float_sub(5.0, 3.0) == 2.0
        assert float_sub(1.0, 1.0) == 0.0
        assert float_sub(0.0, 1.0) == -1.0
        assert float_sub(100.75, 0.75) == 100.0
        print("  PASS: test_basic_float_subtraction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_float_subtraction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Basic float multiplication
    # ------------------------------------------------------------------
    try:
        def float_mul(a, b):
            return a * b

        for _ in range(WARMUP):
            float_mul(2.0, 3.0)
        check_jit_compiled(float_mul, "float_mul")

        assert float_mul(2.0, 3.0) == 6.0
        assert float_mul(0.5, 4.0) == 2.0
        assert float_mul(-2.0, 3.0) == -6.0
        assert float_mul(-2.0, -3.0) == 6.0
        print("  PASS: test_basic_float_multiplication")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_float_multiplication — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Edge cases — zero, negative zero
    # ------------------------------------------------------------------
    try:
        def add_zero(a, b):
            return a + b

        for _ in range(WARMUP):
            add_zero(1.0, 0.0)
        check_jit_compiled(add_zero, "add_zero")

        assert add_zero(0.0, 0.0) == 0.0
        assert add_zero(-0.0, 0.0) == 0.0
        assert add_zero(0.0, -0.0) == 0.0
        # -0.0 + -0.0 == -0.0 per IEEE 754
        result = add_zero(-0.0, -0.0)
        assert result == 0.0  # value is zero
        assert math.copysign(1.0, result) == -1.0, "Expected negative zero"
        print("  PASS: test_zero_and_negative_zero")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_zero_and_negative_zero — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Very large and very small floats
    # ------------------------------------------------------------------
    try:
        def add_extreme(a, b):
            return a + b

        for _ in range(WARMUP):
            add_extreme(1.0, 2.0)
        check_jit_compiled(add_extreme, "add_extreme")

        # Very large
        big = 1e308
        assert add_extreme(big, 0.0) == big
        assert add_extreme(big, -big) == 0.0

        # Very small (subnormal)
        tiny = 5e-324  # smallest positive subnormal
        assert add_extreme(tiny, 0.0) == tiny
        assert add_extreme(tiny, tiny) == tiny + tiny

        # Large + small (precision loss)
        assert add_extreme(1e18, 1.0) == 1e18 + 1.0
        print("  PASS: test_very_large_and_small")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_very_large_and_small — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Infinity propagation
    # ------------------------------------------------------------------
    try:
        def add_inf(a, b):
            return a + b

        for _ in range(WARMUP):
            add_inf(1.0, 2.0)
        check_jit_compiled(add_inf, "add_inf")

        inf = float('inf')
        assert add_inf(inf, 1.0) == inf
        assert add_inf(1.0, inf) == inf
        assert add_inf(inf, inf) == inf
        assert add_inf(-inf, -inf) == -inf
        # inf + (-inf) == nan
        result = add_inf(inf, -inf)
        assert math.isnan(result), f"Expected NaN, got {result}"

        def mul_inf(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_inf(2.0, 3.0)
        assert mul_inf(inf, 2.0) == inf
        assert mul_inf(inf, -1.0) == -inf
        # inf * 0 == nan
        result = mul_inf(inf, 0.0)
        assert math.isnan(result), f"Expected NaN from inf*0, got {result}"
        print("  PASS: test_infinity_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_infinity_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: NaN propagation
    # ------------------------------------------------------------------
    try:
        def add_nan(a, b):
            return a + b

        for _ in range(WARMUP):
            add_nan(1.0, 2.0)
        check_jit_compiled(add_nan, "add_nan")

        nan = float('nan')
        # NaN + anything == NaN
        assert math.isnan(add_nan(nan, 1.0))
        assert math.isnan(add_nan(1.0, nan))
        assert math.isnan(add_nan(nan, nan))
        assert math.isnan(add_nan(nan, 0.0))

        def mul_nan(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_nan(2.0, 3.0)
        assert math.isnan(mul_nan(nan, 1.0))
        assert math.isnan(mul_nan(0.0, nan))
        print("  PASS: test_nan_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nan_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt float -> int
    # ------------------------------------------------------------------
    try:
        def add_deopt_int(a, b):
            return a + b

        for _ in range(WARMUP):
            add_deopt_int(1.0, 2.0)
        check_jit_compiled(add_deopt_int, "add_deopt_int")

        assert add_deopt_int(1.0, 2.0) == 3.0
        # Switch to int operands — deopt
        assert add_deopt_int(1, 2) == 3
        assert add_deopt_int(100, 200) == 300
        # Back to float
        assert add_deopt_int(1.5, 2.5) == 4.0
        print("  PASS: test_deopt_float_to_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt float -> complex
    # ------------------------------------------------------------------
    try:
        def add_deopt_complex(a, b):
            return a + b

        for _ in range(WARMUP):
            add_deopt_complex(1.0, 2.0)
        check_jit_compiled(add_deopt_complex, "add_deopt_complex")

        assert add_deopt_complex(1.0, 2.0) == 3.0
        # Switch to complex — deopt
        assert add_deopt_complex(1+2j, 3+4j) == (4+6j)
        assert add_deopt_complex(0+1j, 0+1j) == 2j
        # Back to float
        assert add_deopt_complex(3.14, 2.86) == 6.0
        print("  PASS: test_deopt_float_to_complex")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_complex — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt float -> custom __add__
    # ------------------------------------------------------------------
    try:
        class Vector:
            def __init__(self, x, y):
                self.x = x
                self.y = y

            def __add__(self, other):
                return Vector(self.x + other.x, self.y + other.y)

        def add_deopt_custom(a, b):
            return a + b

        for _ in range(WARMUP):
            add_deopt_custom(1.0, 2.0)
        check_jit_compiled(add_deopt_custom, "add_deopt_custom")

        assert add_deopt_custom(1.0, 2.0) == 3.0
        # Switch to Vector — deopt
        v = add_deopt_custom(Vector(1, 2), Vector(3, 4))
        assert v.x == 4 and v.y == 6
        # Back to float
        assert add_deopt_custom(10.0, 20.0) == 30.0
        print("  PASS: test_deopt_float_to_custom_add")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_custom_add — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Mixed type — float + int (Python auto-promotes)
    # ------------------------------------------------------------------
    try:
        def add_mixed(a, b):
            return a + b

        for _ in range(WARMUP):
            add_mixed(1.0, 2.0)
        check_jit_compiled(add_mixed, "add_mixed")

        assert add_mixed(1.0, 2.0) == 3.0
        # float + int — Python promotes int to float
        assert add_mixed(1.0, 2) == 3.0
        assert isinstance(add_mixed(1.0, 2), float)
        # int + float
        assert add_mixed(1, 2.0) == 3.0
        assert isinstance(add_mixed(1, 2.0), float)
        print("  PASS: test_mixed_float_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_float_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Accumulator loop (addition)
    # ------------------------------------------------------------------
    try:
        def float_sum_loop(n):
            total = 0.0
            for i in range(n):
                total += float(i)
            return total

        for _ in range(WARMUP):
            float_sum_loop(10)
        check_jit_compiled(float_sum_loop, "float_sum_loop")

        assert float_sum_loop(10) == 45.0
        assert float_sum_loop(100) == 4950.0
        assert float_sum_loop(0) == 0.0
        assert float_sum_loop(1) == 0.0
        print("  PASS: test_accumulator_loop_add")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_accumulator_loop_add — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Accumulator loop (multiplication — product)
    # ------------------------------------------------------------------
    try:
        def float_product_loop(values):
            product = 1.0
            for v in values:
                product *= v
            return product

        vals = [2.0, 3.0, 4.0, 5.0]
        for _ in range(WARMUP):
            float_product_loop(vals)
        check_jit_compiled(float_product_loop, "float_product_loop")

        assert float_product_loop(vals) == 120.0
        assert float_product_loop([0.5, 0.5, 0.5]) == 0.125
        assert float_product_loop([1.0]) == 1.0
        assert float_product_loop([]) == 1.0
        print("  PASS: test_accumulator_loop_mul")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_accumulator_loop_mul — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid type alternation
    # ------------------------------------------------------------------
    try:
        def poly_add(a, b):
            return a + b

        for _ in range(WARMUP):
            poly_add(1.0, 2.0)
        check_jit_compiled(poly_add, "poly_add")

        for cycle in range(50):
            assert poly_add(1.0, 2.0) == 3.0, f"float failed at cycle {cycle}"
            assert poly_add(1, 2) == 3, f"int failed at cycle {cycle}"

        # Final check both still work
        assert poly_add(3.14, 2.86) == 6.0
        assert poly_add(100, 200) == 300
        print("  PASS: test_rapid_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Subtraction deopt — float then int
    # ------------------------------------------------------------------
    try:
        def sub_deopt(a, b):
            return a - b

        for _ in range(WARMUP):
            sub_deopt(5.0, 3.0)
        check_jit_compiled(sub_deopt, "sub_deopt")

        assert sub_deopt(5.0, 3.0) == 2.0
        # Deopt to int
        assert sub_deopt(10, 3) == 7
        # Back to float
        assert sub_deopt(10.5, 0.5) == 10.0
        # Deopt to string (concatenation not supported — should raise)
        raised = False
        try:
            sub_deopt("hello", "world")
        except TypeError:
            raised = True
        assert raised, "Expected TypeError for string subtraction"
        print("  PASS: test_subtraction_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subtraction_deopt — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Multiplication deopt — float then string
    # ------------------------------------------------------------------
    try:
        def mul_deopt(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_deopt(2.0, 3.0)
        check_jit_compiled(mul_deopt, "mul_deopt")

        assert mul_deopt(2.0, 3.0) == 6.0
        # Deopt to int
        assert mul_deopt(4, 5) == 20
        # Deopt to string * int (Python supports this)
        assert mul_deopt("ab", 3) == "ababab"
        # Back to float
        assert mul_deopt(0.5, 6.0) == 3.0
        print("  PASS: test_multiplication_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiplication_deopt — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: IEEE 754 precision — no unexpected rounding
    # ------------------------------------------------------------------
    try:
        def precise_add(a, b):
            return a + b

        for _ in range(WARMUP):
            precise_add(1.0, 2.0)
        check_jit_compiled(precise_add, "precise_add")

        # These are well-known IEEE 754 results
        # 0.1 + 0.2 != 0.3 in IEEE 754
        result = precise_add(0.1, 0.2)
        expected = 0.1 + 0.2  # CPython reference
        assert result == expected, f"Precision mismatch: {result} != {expected}"

        # 1.0 + 2**-52 should be distinguishable from 1.0
        eps = 2.0 ** -52
        assert precise_add(1.0, eps) > 1.0
        assert precise_add(1.0, eps) == 1.0 + eps

        # 1.0 + 2**-53 should NOT be distinguishable from 1.0
        half_eps = 2.0 ** -53
        assert precise_add(1.0, half_eps) == 1.0 + half_eps
        print("  PASS: test_ieee754_precision")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_ieee754_precision — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Multiple operations in one function
    # ------------------------------------------------------------------
    try:
        def multi_op(a, b, c):
            s = a + b
            d = a - b
            p = a * c
            return s + d + p

        for _ in range(WARMUP):
            multi_op(10.0, 3.0, 2.0)
        check_jit_compiled(multi_op, "multi_op")

        # s=13, d=7, p=20, total=40
        assert multi_op(10.0, 3.0, 2.0) == 40.0
        # s=5, d=1, p=9, total=15
        assert multi_op(3.0, 2.0, 3.0) == 15.0
        # s=0, d=0, p=0, total=0
        assert multi_op(0.0, 0.0, 0.0) == 0.0
        print("  PASS: test_multiple_operations")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_operations — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Overflow to infinity
    # ------------------------------------------------------------------
    try:
        def add_overflow(a, b):
            return a + b

        for _ in range(WARMUP):
            add_overflow(1.0, 2.0)
        check_jit_compiled(add_overflow, "add_overflow")

        big = 1.7976931348623157e+308  # sys.float_info.max
        assert add_overflow(big, big) == float('inf')
        assert add_overflow(-big, -big) == float('-inf')

        def mul_overflow(a, b):
            return a * b

        for _ in range(WARMUP):
            mul_overflow(2.0, 3.0)
        assert mul_overflow(big, 2.0) == float('inf')
        assert mul_overflow(big, -2.0) == float('-inf')
        print("  PASS: test_overflow_to_infinity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overflow_to_infinity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — a + b vs float.__add__(a, b)
    # ------------------------------------------------------------------
    try:
        def op_add(a, b):
            return a + b

        def explicit_add(a, b):
            return float.__add__(a, b)

        for _ in range(WARMUP):
            op_add(1.0, 2.0)
        check_jit_compiled(op_add, "op_add")

        test_pairs = [
            (1.0, 2.0), (0.0, 0.0), (-1.0, 1.0), (0.1, 0.2),
            (1e100, 1e-100), (float('inf'), 1.0), (-0.0, 0.0),
        ]
        for a, b in test_pairs:
            r1 = op_add(a, b)
            r2 = explicit_add(a, b)
            if math.isnan(r1):
                assert math.isnan(r2), f"NaN mismatch for ({a}, {b})"
            else:
                assert r1 == r2, (
                    f"Mismatch for ({a}, {b}): op={r1}, explicit={r2}"
                )

        # Also check NaN case
        r1 = op_add(float('nan'), 1.0)
        r2 = explicit_add(float('nan'), 1.0)
        assert math.isnan(r1) and math.isnan(r2)
        print("  PASS: test_equivalence_op_vs_dunder")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_op_vs_dunder — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nBINARY_OP_ADD_FLOAT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
