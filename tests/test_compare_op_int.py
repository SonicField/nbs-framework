#!/usr/bin/env python3
"""
test_compare_op_int.py — Correctness and deopt tests for
COMPARE_OP_INT specialisation.

Targets: COMPARE_OP_INT.

COMPARE_OP_INT specialises comparison operations (==, !=, <, <=, >, >=)
when both operands are integers. Instead of going through the generic
COMPARE_OP path (which must check types, look up __eq__/__lt__/etc., and
dispatch through the rich comparison protocol), the specialisation directly
compares the underlying C long values.

The adaptive specialiser emits COMPARE_OP_INT after observing repeated
comparisons of integer operands.

Deopt triggers:
  - One or both operands are not int (float, str, custom __eq__)
  - Operand type changes between calls

Tests cover:
  - Basic equality (==)
  - Basic inequality (!=)
  - Less than (<)
  - Less than or equal (<=)
  - Greater than (>)
  - Greater than or equal (>=)
  - Negative integers
  - Zero comparisons
  - Large integers (arbitrary precision)
  - Deopt: switch to float operand
  - Deopt: switch to string operand
  - Deopt: switch to custom __eq__
  - Boolean result identity (is True / is False)
  - Chained comparisons (a < b < c)
  - Loop with comparison accumulator
  - Rapid int-vs-float alternation
  - Reflexive property (a == a)
  - Antisymmetry ((a < b) implies not (b < a))
  - Transitivity (a < b < c implies a < c)
  - Equivalence: (a < b) vs operator.lt(a, b)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Mathematical properties preserved (reflexivity, antisymmetry, transitivity)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_compare_op_int.py
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
    # Test 1: Basic equality (==)
    # ------------------------------------------------------------------
    try:
        def cmp_eq(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_eq(5, 5)
        check_jit_compiled(cmp_eq, "cmp_eq")

        assert cmp_eq(5, 5) is True
        assert cmp_eq(5, 6) is False
        assert cmp_eq(0, 0) is True
        assert cmp_eq(-1, -1) is True
        assert cmp_eq(-1, 1) is False
        print("  PASS: test_basic_equality")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_equality — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Basic inequality (!=)
    # ------------------------------------------------------------------
    try:
        def cmp_ne(a, b):
            return a != b

        for _ in range(WARMUP):
            cmp_ne(5, 6)
        check_jit_compiled(cmp_ne, "cmp_ne")

        assert cmp_ne(5, 6) is True
        assert cmp_ne(5, 5) is False
        assert cmp_ne(0, 1) is True
        assert cmp_ne(0, 0) is False
        assert cmp_ne(-1, 1) is True
        print("  PASS: test_basic_inequality")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_inequality — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Less than (<)
    # ------------------------------------------------------------------
    try:
        def cmp_lt(a, b):
            return a < b

        for _ in range(WARMUP):
            cmp_lt(3, 5)
        check_jit_compiled(cmp_lt, "cmp_lt")

        assert cmp_lt(3, 5) is True
        assert cmp_lt(5, 3) is False
        assert cmp_lt(5, 5) is False
        assert cmp_lt(-1, 0) is True
        assert cmp_lt(0, -1) is False
        print("  PASS: test_less_than")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_less_than — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Less than or equal (<=)
    # ------------------------------------------------------------------
    try:
        def cmp_le(a, b):
            return a <= b

        for _ in range(WARMUP):
            cmp_le(3, 5)
        check_jit_compiled(cmp_le, "cmp_le")

        assert cmp_le(3, 5) is True
        assert cmp_le(5, 5) is True
        assert cmp_le(6, 5) is False
        assert cmp_le(-1, 0) is True
        assert cmp_le(0, 0) is True
        print("  PASS: test_less_than_or_equal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_less_than_or_equal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Greater than (>)
    # ------------------------------------------------------------------
    try:
        def cmp_gt(a, b):
            return a > b

        for _ in range(WARMUP):
            cmp_gt(5, 3)
        check_jit_compiled(cmp_gt, "cmp_gt")

        assert cmp_gt(5, 3) is True
        assert cmp_gt(3, 5) is False
        assert cmp_gt(5, 5) is False
        assert cmp_gt(0, -1) is True
        assert cmp_gt(-1, 0) is False
        print("  PASS: test_greater_than")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_greater_than — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Greater than or equal (>=)
    # ------------------------------------------------------------------
    try:
        def cmp_ge(a, b):
            return a >= b

        for _ in range(WARMUP):
            cmp_ge(5, 3)
        check_jit_compiled(cmp_ge, "cmp_ge")

        assert cmp_ge(5, 3) is True
        assert cmp_ge(5, 5) is True
        assert cmp_ge(4, 5) is False
        assert cmp_ge(0, -1) is True
        assert cmp_ge(0, 0) is True
        print("  PASS: test_greater_than_or_equal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_greater_than_or_equal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Negative integers
    # ------------------------------------------------------------------
    try:
        def cmp_neg(a, b):
            return a < b

        for _ in range(WARMUP):
            cmp_neg(-10, -5)
        check_jit_compiled(cmp_neg, "cmp_neg")

        assert cmp_neg(-10, -5) is True
        assert cmp_neg(-5, -10) is False
        assert cmp_neg(-100, -99) is True
        assert cmp_neg(-1, -1) is False
        assert cmp_neg(-10**18, -10**17) is True
        assert cmp_neg(-10**17, -10**18) is False
        print("  PASS: test_negative_integers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_integers — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Zero comparisons
    # ------------------------------------------------------------------
    try:
        def cmp_zero(a, b):
            return (a == b, a < b, a > b)

        for _ in range(WARMUP):
            cmp_zero(0, 0)
        check_jit_compiled(cmp_zero, "cmp_zero")

        assert cmp_zero(0, 0) == (True, False, False)
        assert cmp_zero(0, 1) == (False, True, False)
        assert cmp_zero(1, 0) == (False, False, True)
        assert cmp_zero(0, -1) == (False, False, True)
        assert cmp_zero(-1, 0) == (False, True, False)
        print("  PASS: test_zero_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_zero_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Large integers (arbitrary precision)
    # ------------------------------------------------------------------
    try:
        def cmp_large(a, b):
            return a < b

        big_a = 10**100
        big_b = 10**100 + 1

        for _ in range(WARMUP):
            cmp_large(big_a, big_b)
        check_jit_compiled(cmp_large, "cmp_large")

        assert cmp_large(big_a, big_b) is True
        assert cmp_large(big_b, big_a) is False
        assert cmp_large(big_a, big_a) is False

        def eq_large(a, b):
            return a == b

        for _ in range(WARMUP):
            eq_large(big_a, big_a)
        assert eq_large(big_a, big_a) is True
        assert eq_large(big_a, big_b) is False
        assert eq_large(10**200, 10**200) is True
        assert eq_large(10**200, 10**200 + 1) is False
        print("  PASS: test_large_integers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_integers — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt int -> float
    # ------------------------------------------------------------------
    try:
        def cmp_deopt_float(a, b):
            return a < b

        for _ in range(WARMUP):
            cmp_deopt_float(3, 5)
        check_jit_compiled(cmp_deopt_float, "cmp_deopt_float")

        assert cmp_deopt_float(3, 5) is True
        # Deopt: float operand
        assert cmp_deopt_float(3.0, 5.0) is True
        assert cmp_deopt_float(5.0, 3.0) is False
        assert cmp_deopt_float(3, 5.0) is True  # mixed int/float
        # Back to int
        assert cmp_deopt_float(3, 5) is True
        print("  PASS: test_deopt_int_to_float")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_float — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt int -> string
    # ------------------------------------------------------------------
    try:
        def cmp_deopt_str(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_deopt_str(42, 42)
        check_jit_compiled(cmp_deopt_str, "cmp_deopt_str")

        assert cmp_deopt_str(42, 42) is True
        # Deopt: string operands
        assert cmp_deopt_str("hello", "hello") is True
        assert cmp_deopt_str("hello", "world") is False
        # Cross-type comparison (int == str is always False)
        assert cmp_deopt_str(42, "42") is False
        # Back to int
        assert cmp_deopt_str(42, 42) is True
        print("  PASS: test_deopt_int_to_string")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_string — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt int -> custom __eq__
    # ------------------------------------------------------------------
    try:
        class AlwaysEqual:
            def __eq__(self, other):
                return True

        class NeverEqual:
            def __eq__(self, other):
                return False

        def cmp_deopt_custom(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_deopt_custom(10, 10)
        check_jit_compiled(cmp_deopt_custom, "cmp_deopt_custom")

        assert cmp_deopt_custom(10, 10) is True
        # Deopt: custom __eq__
        always = AlwaysEqual()
        never = NeverEqual()
        assert cmp_deopt_custom(always, 999) is True
        assert cmp_deopt_custom(never, 999) is False
        assert cmp_deopt_custom(always, always) is True
        # Back to int
        assert cmp_deopt_custom(10, 20) is False
        print("  PASS: test_deopt_custom_eq")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_custom_eq — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Boolean result identity (is True / is False)
    # ------------------------------------------------------------------
    try:
        def cmp_identity(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_identity(5, 5)
        check_jit_compiled(cmp_identity, "cmp_identity")

        # Comparison results must be exactly True or False (not just truthy/falsy)
        result_true = cmp_identity(5, 5)
        result_false = cmp_identity(5, 6)
        assert result_true is True, f"Expected True, got {result_true!r}"
        assert result_false is False, f"Expected False, got {result_false!r}"
        assert type(result_true) is bool
        assert type(result_false) is bool

        def cmp_lt_identity(a, b):
            return a < b
        for _ in range(WARMUP):
            cmp_lt_identity(3, 5)
        result_lt = cmp_lt_identity(3, 5)
        assert result_lt is True
        assert type(result_lt) is bool
        print("  PASS: test_boolean_result_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_result_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Chained comparisons (a < b < c)
    # ------------------------------------------------------------------
    try:
        def chained_lt(a, b, c):
            return a < b < c

        for _ in range(WARMUP):
            chained_lt(1, 2, 3)
        check_jit_compiled(chained_lt, "chained_lt")

        assert chained_lt(1, 2, 3) is True
        assert chained_lt(1, 2, 2) is False  # 2 < 2 is False
        assert chained_lt(3, 2, 1) is False
        assert chained_lt(1, 1, 1) is False
        assert chained_lt(-3, 0, 3) is True

        def chained_le(a, b, c):
            return a <= b <= c

        for _ in range(WARMUP):
            chained_le(1, 2, 3)
        check_jit_compiled(chained_le, "chained_le")

        assert chained_le(1, 2, 3) is True
        assert chained_le(1, 2, 2) is True   # 2 <= 2 is True
        assert chained_le(1, 1, 1) is True
        assert chained_le(3, 2, 1) is False
        print("  PASS: test_chained_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Loop with comparison accumulator
    # ------------------------------------------------------------------
    try:
        def count_less_than(lst, threshold):
            count = 0
            for x in lst:
                if x < threshold:
                    count += 1
            return count

        data = list(range(100))
        for _ in range(WARMUP):
            count_less_than(data, 50)
        check_jit_compiled(count_less_than, "count_less_than")

        assert count_less_than(data, 50) == 50
        assert count_less_than(data, 0) == 0
        assert count_less_than(data, 100) == 100
        assert count_less_than(data, 1) == 1
        assert count_less_than([], 50) == 0
        print("  PASS: test_loop_comparison_accumulator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_loop_comparison_accumulator — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Rapid int-vs-float alternation
    # ------------------------------------------------------------------
    try:
        def cmp_poly(a, b):
            return a < b

        for _ in range(WARMUP):
            cmp_poly(3, 5)
        check_jit_compiled(cmp_poly, "cmp_poly")

        for cycle in range(50):
            r_int = cmp_poly(3, 5)
            r_float = cmp_poly(3.0, 5.0)
            assert r_int is True, f"int compare failed at cycle {cycle}"
            assert r_float is True, f"float compare failed at cycle {cycle}"

            r_int2 = cmp_poly(5, 3)
            r_float2 = cmp_poly(5.0, 3.0)
            assert r_int2 is False, f"int reverse failed at cycle {cycle}"
            assert r_float2 is False, f"float reverse failed at cycle {cycle}"
        print("  PASS: test_rapid_int_float_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_int_float_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Reflexive property (a == a)
    # ------------------------------------------------------------------
    try:
        def cmp_reflexive(a):
            return (a == a, a <= a, a >= a, a < a, a > a, a != a)

        for _ in range(WARMUP):
            cmp_reflexive(42)
        check_jit_compiled(cmp_reflexive, "cmp_reflexive")

        for val in [0, 1, -1, 42, -42, 10**50, -10**50]:
            eq, le, ge, lt, gt, ne = cmp_reflexive(val)
            assert eq is True, f"a==a failed for {val}"
            assert le is True, f"a<=a failed for {val}"
            assert ge is True, f"a>=a failed for {val}"
            assert lt is False, f"a<a should be False for {val}"
            assert gt is False, f"a>a should be False for {val}"
            assert ne is False, f"a!=a should be False for {val}"
        print("  PASS: test_reflexive_property")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_reflexive_property — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Antisymmetry ((a < b) implies not (b < a))
    # ------------------------------------------------------------------
    try:
        def check_antisymmetry(a, b):
            if a < b:
                return not (b < a)
            elif b < a:
                return not (a < b)
            else:  # a == b
                return not (a < b) and not (b < a)

        for _ in range(WARMUP):
            check_antisymmetry(3, 5)
        check_jit_compiled(check_antisymmetry, "check_antisymmetry")

        pairs = [
            (1, 2), (2, 1), (0, 0), (-5, 5), (5, -5),
            (10**50, 10**50 + 1), (10**50, 10**50),
            (-10**50, 10**50), (0, 10**100),
        ]
        for a, b in pairs:
            assert check_antisymmetry(a, b) is True, (
                f"Antisymmetry violated for ({a}, {b})"
            )
        print("  PASS: test_antisymmetry")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_antisymmetry — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Transitivity (a < b and b < c implies a < c)
    # ------------------------------------------------------------------
    try:
        def check_transitivity(a, b, c):
            if a < b and b < c:
                return a < c
            return True  # vacuously true if precondition not met

        for _ in range(WARMUP):
            check_transitivity(1, 2, 3)
        check_jit_compiled(check_transitivity, "check_transitivity")

        triples = [
            (1, 2, 3), (0, 1, 2), (-3, -2, -1),
            (-100, 0, 100), (10**50, 10**50 + 1, 10**50 + 2),
            (1, 5, 10), (-10, -5, 0),
        ]
        for a, b, c in triples:
            assert check_transitivity(a, b, c) is True, (
                f"Transitivity violated for ({a}, {b}, {c})"
            )
        print("  PASS: test_transitivity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_transitivity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — (a < b) vs int.__lt__(a, b)
    # ------------------------------------------------------------------
    try:
        def cmp_operator(a, b):
            return a < b

        def cmp_dunder(a, b):
            return int.__lt__(a, b)

        for _ in range(WARMUP):
            cmp_operator(3, 5)
        check_jit_compiled(cmp_operator, "cmp_operator")

        pairs = [
            (0, 0), (1, 2), (2, 1), (-1, 0), (0, -1),
            (10**50, 10**50), (10**50, 10**50 + 1),
            (-10**50, 10**50), (42, 42), (0, 1),
        ]
        for a, b in pairs:
            r_op = cmp_operator(a, b)
            r_du = cmp_dunder(a, b)
            assert r_op == r_du, (
                f"Mismatch for ({a}, {b}): operator={r_op}, dunder={r_du}"
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
    print(f"\nCOMPARE_OP_INT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
