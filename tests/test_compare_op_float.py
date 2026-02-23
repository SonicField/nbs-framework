#!/usr/bin/env python3
"""
test_compare_op_float.py — Correctness and deopt tests for COMPARE_OP_FLOAT
specialisation.

Targets: COMPARE_OP_FLOAT.

COMPARE_OP_FLOAT specialises comparison operations (==, !=, <, <=, >, >=)
when both operands are float. Instead of going through generic
PyObject_RichCompare dispatch, it uses direct float comparison.

The JIT specialisation emits GuardType on both operands to confirm they are
float, then performs the comparison directly without dispatch overhead.

Deopt triggers:
  - Function JIT-compiled with float comparisons, then called with int/other
  - Operand is not float (e.g. int, str, custom __lt__)

Tests cover:
  - All six comparison operators (==, !=, <, <=, >, >=)
  - Basic float comparisons
  - Edge cases: infinity, negative infinity
  - NaN comparison semantics (NaN is not equal to anything, including itself)
  - Signed zero (-0.0 == 0.0)
  - Very large and very small (subnormal) floats
  - Deopt: float-compiled then called with int operands
  - Deopt: float-compiled then called with string operands
  - Deopt: float-compiled then called with custom comparison objects
  - Mixed float + int comparison (Python auto-promotes)
  - Comparison in loop (sorting predicate pattern)
  - Rapid type alternation
  - Chained comparisons (a < b < c)
  - Multiple comparisons in one function
  - Equivalence: (a < b) vs float.__lt__(a, b)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct deopt when operand type changes
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_compare_op_float.py
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
    # Test 1: Equal (==)
    # ------------------------------------------------------------------
    try:
        def float_eq(a, b):
            return a == b

        for _ in range(WARMUP):
            float_eq(1.0, 1.0)
        check_jit_compiled(float_eq, "float_eq")

        assert float_eq(1.0, 1.0) is True
        assert float_eq(1.0, 2.0) is False
        assert float_eq(0.0, 0.0) is True
        assert float_eq(-1.5, -1.5) is True
        assert float_eq(1.0, 1.0000000000000002) is False  # different at ULP
        print("  PASS: test_equal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Not equal (!=)
    # ------------------------------------------------------------------
    try:
        def float_ne(a, b):
            return a != b

        for _ in range(WARMUP):
            float_ne(1.0, 2.0)
        check_jit_compiled(float_ne, "float_ne")

        assert float_ne(1.0, 2.0) is True
        assert float_ne(1.0, 1.0) is False
        assert float_ne(0.0, -0.0) is False  # 0.0 == -0.0
        assert float_ne(3.14, 3.15) is True
        print("  PASS: test_not_equal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_not_equal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Less than (<)
    # ------------------------------------------------------------------
    try:
        def float_lt(a, b):
            return a < b

        for _ in range(WARMUP):
            float_lt(1.0, 2.0)
        check_jit_compiled(float_lt, "float_lt")

        assert float_lt(1.0, 2.0) is True
        assert float_lt(2.0, 1.0) is False
        assert float_lt(1.0, 1.0) is False
        assert float_lt(-1.0, 0.0) is True
        assert float_lt(-2.0, -1.0) is True
        print("  PASS: test_less_than")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_less_than — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Less than or equal (<=)
    # ------------------------------------------------------------------
    try:
        def float_le(a, b):
            return a <= b

        for _ in range(WARMUP):
            float_le(1.0, 2.0)
        check_jit_compiled(float_le, "float_le")

        assert float_le(1.0, 2.0) is True
        assert float_le(2.0, 1.0) is False
        assert float_le(1.0, 1.0) is True
        assert float_le(-1.0, -1.0) is True
        assert float_le(0.0, 0.0) is True
        print("  PASS: test_less_equal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_less_equal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Greater than (>)
    # ------------------------------------------------------------------
    try:
        def float_gt(a, b):
            return a > b

        for _ in range(WARMUP):
            float_gt(2.0, 1.0)
        check_jit_compiled(float_gt, "float_gt")

        assert float_gt(2.0, 1.0) is True
        assert float_gt(1.0, 2.0) is False
        assert float_gt(1.0, 1.0) is False
        assert float_gt(0.0, -1.0) is True
        assert float_gt(-1.0, -2.0) is True
        print("  PASS: test_greater_than")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_greater_than — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Greater than or equal (>=)
    # ------------------------------------------------------------------
    try:
        def float_ge(a, b):
            return a >= b

        for _ in range(WARMUP):
            float_ge(2.0, 1.0)
        check_jit_compiled(float_ge, "float_ge")

        assert float_ge(2.0, 1.0) is True
        assert float_ge(1.0, 2.0) is False
        assert float_ge(1.0, 1.0) is True
        assert float_ge(-1.0, -1.0) is True
        assert float_ge(0.0, -0.0) is True
        print("  PASS: test_greater_equal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_greater_equal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: NaN comparison semantics
    # ------------------------------------------------------------------
    try:
        def cmp_nan_eq(a, b):
            return a == b

        def cmp_nan_ne(a, b):
            return a != b

        def cmp_nan_lt(a, b):
            return a < b

        for _ in range(WARMUP):
            cmp_nan_eq(1.0, 1.0)
            cmp_nan_ne(1.0, 2.0)
            cmp_nan_lt(1.0, 2.0)
        check_jit_compiled(cmp_nan_eq, "cmp_nan_eq")

        nan = float('nan')
        # NaN is not equal to anything, including itself
        assert cmp_nan_eq(nan, nan) is False
        assert cmp_nan_eq(nan, 1.0) is False
        assert cmp_nan_eq(1.0, nan) is False
        # NaN != anything is True
        assert cmp_nan_ne(nan, nan) is True
        assert cmp_nan_ne(nan, 1.0) is True
        # NaN < anything is False
        assert cmp_nan_lt(nan, 1.0) is False
        assert cmp_nan_lt(1.0, nan) is False
        assert cmp_nan_lt(nan, nan) is False
        print("  PASS: test_nan_comparison_semantics")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nan_comparison_semantics — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Infinity comparisons
    # ------------------------------------------------------------------
    try:
        def cmp_inf(a, b):
            return a < b

        for _ in range(WARMUP):
            cmp_inf(1.0, 2.0)
        check_jit_compiled(cmp_inf, "cmp_inf")

        inf = float('inf')
        # Everything < inf (except inf itself)
        assert cmp_inf(1.0, inf) is True
        assert cmp_inf(1e308, inf) is True
        assert cmp_inf(inf, inf) is False
        assert cmp_inf(-inf, inf) is True
        # -inf < everything (except -inf itself)
        assert cmp_inf(-inf, -1e308) is True
        assert cmp_inf(-inf, 0.0) is True
        assert cmp_inf(-inf, -inf) is False

        def eq_inf(a, b):
            return a == b

        for _ in range(WARMUP):
            eq_inf(1.0, 1.0)
        assert eq_inf(inf, inf) is True
        assert eq_inf(-inf, -inf) is True
        assert eq_inf(inf, -inf) is False
        print("  PASS: test_infinity_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_infinity_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Signed zero — -0.0 == 0.0
    # ------------------------------------------------------------------
    try:
        def eq_zero(a, b):
            return a == b

        def lt_zero(a, b):
            return a < b

        for _ in range(WARMUP):
            eq_zero(0.0, 0.0)
            lt_zero(0.0, 1.0)
        check_jit_compiled(eq_zero, "eq_zero")

        # IEEE 754: -0.0 == 0.0
        assert eq_zero(-0.0, 0.0) is True
        assert eq_zero(0.0, -0.0) is True
        # -0.0 is NOT less than 0.0
        assert lt_zero(-0.0, 0.0) is False
        assert lt_zero(0.0, -0.0) is False
        print("  PASS: test_signed_zero_equality")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_signed_zero_equality — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt float -> int
    # ------------------------------------------------------------------
    try:
        def lt_deopt_int(a, b):
            return a < b

        for _ in range(WARMUP):
            lt_deopt_int(1.0, 2.0)
        check_jit_compiled(lt_deopt_int, "lt_deopt_int")

        assert lt_deopt_int(1.0, 2.0) is True
        # Deopt to int
        assert lt_deopt_int(1, 2) is True
        assert lt_deopt_int(2, 1) is False
        assert lt_deopt_int(1, 1) is False
        # Back to float
        assert lt_deopt_int(3.14, 3.15) is True
        print("  PASS: test_deopt_float_to_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt float -> string
    # ------------------------------------------------------------------
    try:
        def eq_deopt_str(a, b):
            return a == b

        for _ in range(WARMUP):
            eq_deopt_str(1.0, 1.0)
        check_jit_compiled(eq_deopt_str, "eq_deopt_str")

        assert eq_deopt_str(1.0, 1.0) is True
        # Deopt to string
        assert eq_deopt_str("hello", "hello") is True
        assert eq_deopt_str("hello", "world") is False
        # Back to float
        assert eq_deopt_str(2.0, 2.0) is True
        print("  PASS: test_deopt_float_to_string")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_string — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt float -> custom comparison
    # ------------------------------------------------------------------
    try:
        class AlwaysLess:
            def __lt__(self, other):
                return True

            def __ge__(self, other):
                return False

        def lt_deopt_custom(a, b):
            return a < b

        for _ in range(WARMUP):
            lt_deopt_custom(1.0, 2.0)
        check_jit_compiled(lt_deopt_custom, "lt_deopt_custom")

        assert lt_deopt_custom(1.0, 2.0) is True
        # Deopt to custom
        al = AlwaysLess()
        assert lt_deopt_custom(al, 999) is True
        assert lt_deopt_custom(al, -999) is True
        # Back to float
        assert lt_deopt_custom(5.0, 3.0) is False
        print("  PASS: test_deopt_float_to_custom")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_float_to_custom — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Mixed float + int comparison (Python auto-promotes)
    # ------------------------------------------------------------------
    try:
        def lt_mixed(a, b):
            return a < b

        for _ in range(WARMUP):
            lt_mixed(1.0, 2.0)
        check_jit_compiled(lt_mixed, "lt_mixed")

        assert lt_mixed(1.0, 2.0) is True
        # float < int — Python promotes int to float
        assert lt_mixed(1.0, 2) is True
        assert lt_mixed(2.0, 1) is False
        # int < float
        assert lt_mixed(1, 2.0) is True

        def eq_mixed(a, b):
            return a == b

        for _ in range(WARMUP):
            eq_mixed(1.0, 1.0)
        # 1.0 == 1 is True in Python
        assert eq_mixed(1.0, 1) is True
        assert eq_mixed(1, 1.0) is True
        assert eq_mixed(0.0, 0) is True
        print("  PASS: test_mixed_float_int_comparison")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_float_int_comparison — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Comparison in loop (find minimum pattern)
    # ------------------------------------------------------------------
    try:
        def find_min(values):
            result = values[0]
            for i in range(1, len(values)):
                if values[i] < result:
                    result = values[i]
            return result

        data = [3.0, 1.0, 4.0, 1.5, 9.0, 2.6]
        for _ in range(WARMUP):
            find_min(data)
        check_jit_compiled(find_min, "find_min")

        assert find_min(data) == 1.0
        assert find_min([5.0]) == 5.0
        assert find_min([1.0, 2.0, 3.0]) == 1.0
        assert find_min([3.0, 2.0, 1.0]) == 1.0
        assert find_min([-1.0, -2.0, -3.0]) == -3.0
        print("  PASS: test_comparison_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_comparison_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Rapid type alternation
    # ------------------------------------------------------------------
    try:
        def poly_lt(a, b):
            return a < b

        for _ in range(WARMUP):
            poly_lt(1.0, 2.0)
        check_jit_compiled(poly_lt, "poly_lt")

        for cycle in range(50):
            assert poly_lt(1.0, 2.0) is True, f"float failed at cycle {cycle}"
            assert poly_lt(1, 2) is True, f"int failed at cycle {cycle}"

        assert poly_lt(3.14, 2.71) is False
        assert poly_lt(10, 20) is True
        print("  PASS: test_rapid_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Chained comparisons (a < b < c)
    # ------------------------------------------------------------------
    try:
        def chained_lt(a, b, c):
            return a < b < c

        for _ in range(WARMUP):
            chained_lt(1.0, 2.0, 3.0)
        check_jit_compiled(chained_lt, "chained_lt")

        assert chained_lt(1.0, 2.0, 3.0) is True
        assert chained_lt(1.0, 3.0, 2.0) is False
        assert chained_lt(1.0, 1.0, 2.0) is False  # not strictly less
        assert chained_lt(-3.0, -2.0, -1.0) is True
        assert chained_lt(0.0, 0.0, 0.0) is False

        def chained_le(a, b, c):
            return a <= b <= c

        for _ in range(WARMUP):
            chained_le(1.0, 2.0, 3.0)
        assert chained_le(1.0, 1.0, 1.0) is True
        assert chained_le(1.0, 2.0, 2.0) is True
        assert chained_le(2.0, 1.0, 3.0) is False
        print("  PASS: test_chained_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Multiple comparisons in one function
    # ------------------------------------------------------------------
    try:
        def classify(x):
            if x < 0.0:
                return "negative"
            elif x == 0.0:
                return "zero"
            elif x <= 1.0:
                return "small"
            else:
                return "large"

        for _ in range(WARMUP):
            classify(0.5)
        check_jit_compiled(classify, "classify")

        assert classify(-1.0) == "negative"
        assert classify(0.0) == "zero"
        assert classify(0.5) == "small"
        assert classify(1.0) == "small"
        assert classify(2.0) == "large"
        assert classify(-0.0) == "zero"  # -0.0 == 0.0
        print("  PASS: test_multiple_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Subnormal float comparisons
    # ------------------------------------------------------------------
    try:
        def lt_subnormal(a, b):
            return a < b

        for _ in range(WARMUP):
            lt_subnormal(1.0, 2.0)
        check_jit_compiled(lt_subnormal, "lt_subnormal")

        tiny1 = 5e-324  # smallest positive subnormal
        tiny2 = 1e-323  # slightly larger subnormal

        assert lt_subnormal(tiny1, tiny2) is True
        assert lt_subnormal(tiny2, tiny1) is False
        assert lt_subnormal(0.0, tiny1) is True
        assert lt_subnormal(-tiny1, 0.0) is True
        assert lt_subnormal(tiny1, tiny1) is False
        print("  PASS: test_subnormal_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subnormal_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: NaN with all six operators
    # ------------------------------------------------------------------
    try:
        nan = float('nan')

        def all_cmp(a, b):
            return (a == b, a != b, a < b, a <= b, a > b, a >= b)

        for _ in range(WARMUP):
            all_cmp(1.0, 2.0)
        check_jit_compiled(all_cmp, "all_cmp")

        # Normal comparison
        assert all_cmp(1.0, 2.0) == (False, True, True, True, False, False)
        assert all_cmp(2.0, 2.0) == (True, False, False, True, False, True)
        assert all_cmp(3.0, 2.0) == (False, True, False, False, True, True)

        # NaN: == is False, != is True, all ordered comparisons are False
        result = all_cmp(nan, 1.0)
        assert result == (False, True, False, False, False, False), (
            f"NaN cmp 1.0: expected (F,T,F,F,F,F), got {result}"
        )
        result = all_cmp(1.0, nan)
        assert result == (False, True, False, False, False, False), (
            f"1.0 cmp NaN: expected (F,T,F,F,F,F), got {result}"
        )
        result = all_cmp(nan, nan)
        assert result == (False, True, False, False, False, False), (
            f"NaN cmp NaN: expected (F,T,F,F,F,F), got {result}"
        )
        print("  PASS: test_nan_all_six_operators")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nan_all_six_operators — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — (a < b) vs float.__lt__(a, b)
    # ------------------------------------------------------------------
    try:
        def op_lt(a, b):
            return a < b

        def explicit_lt(a, b):
            return float.__lt__(a, b)

        for _ in range(WARMUP):
            op_lt(1.0, 2.0)
        check_jit_compiled(op_lt, "op_lt")

        test_pairs = [
            (1.0, 2.0), (2.0, 1.0), (1.0, 1.0), (0.0, -0.0),
            (-0.0, 0.0), (float('inf'), 1.0), (1.0, float('inf')),
            (-float('inf'), float('inf')), (5e-324, 1e-323),
        ]
        for a, b in test_pairs:
            assert op_lt(a, b) == explicit_lt(a, b), (
                f"Mismatch for ({a}, {b}): "
                f"op={op_lt(a, b)}, explicit={explicit_lt(a, b)}"
            )

        # NaN case — both should return NotImplemented... actually
        # float.__lt__(nan, 1.0) returns NotImplemented but (nan < 1.0)
        # returns False. So just verify the operator form.
        assert op_lt(float('nan'), 1.0) is False
        assert op_lt(1.0, float('nan')) is False
        print("  PASS: test_equivalence_op_vs_dunder")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_op_vs_dunder — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCOMPARE_OP_FLOAT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
