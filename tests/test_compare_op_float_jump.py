#!/usr/bin/env python3
"""
test_compare_op_float_jump.py — Correctness and deopt tests for
COMPARE_OP_FLOAT_JUMP specialisation.

Targets: COMPARE_AND_BRANCH (float path) / COMPARE_OP_FLOAT_JUMP.

In CPython 3.12, the adaptive interpreter fuses comparison + conditional
branch for float operands. When the interpreter detects repeated float
comparisons followed by a branch (e.g. if x < y:), it specialises to
avoid generic comparison dispatch and directly compares the C doubles.

The CinderX JIT compiles this by emitting GuardType checks on both
operands to confirm they are floats, then performing a direct double
comparison and branch.

Deopt triggers:
  - Either operand is not a float (int, str, custom type, etc.)
  - Comparison with NaN (IEEE 754 special semantics)
  - Float subclass with overridden __lt__/__gt__/etc.

Tests cover:
  - Basic less-than comparison
  - All six comparison operators (lt, le, eq, ne, gt, ge)
  - Negative floats
  - Comparison with zero
  - Very large floats
  - Very small floats (subnormals)
  - Deopt: int operand
  - Deopt: string operand
  - NaN comparisons (NaN != NaN, etc.)
  - Infinity comparisons
  - Equality of identical floats
  - Close but not equal floats
  - Deopt: mixed float/int in loop
  - Comparison in tight loop (rapid, 1000)
  - Stability (10000 calls)
  - Negative zero vs positive zero
  - Float subclass
  - Chained comparisons (a < b < c)
  - Deopt: operand type changes mid-loop
  - Equivalence: specialised vs operator module

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_compare_op_float_jump.py
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

    print("=== COMPARE_OP_FLOAT_JUMP Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Basic less-than comparison ──────────────────────────────

    def cmp_lt_1(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_lt_1(1.0, 2.0)

    check_jit_compiled(cmp_lt_1, "cmp_lt_1")

    try:
        assert cmp_lt_1(1.0, 2.0) is True
        assert cmp_lt_1(2.0, 1.0) is False
        assert cmp_lt_1(1.0, 1.0) is False
        assert cmp_lt_1(-1.0, 0.0) is True
        print("PASS  Test 1: basic less-than comparison")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic less-than — {e}")
        failed += 1

    # ── Test 2: All six comparison operators ─────────────────────────────

    def cmp_le_2(a, b):
        if a <= b:
            return True
        return False

    def cmp_eq_2(a, b):
        if a == b:
            return True
        return False

    def cmp_ne_2(a, b):
        if a != b:
            return True
        return False

    def cmp_gt_2(a, b):
        if a > b:
            return True
        return False

    def cmp_ge_2(a, b):
        if a >= b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_le_2(1.0, 2.0)
        cmp_eq_2(1.0, 1.0)
        cmp_ne_2(1.0, 2.0)
        cmp_gt_2(2.0, 1.0)
        cmp_ge_2(2.0, 1.0)

    check_jit_compiled(cmp_le_2, "cmp_le_2")
    check_jit_compiled(cmp_eq_2, "cmp_eq_2")
    check_jit_compiled(cmp_ne_2, "cmp_ne_2")
    check_jit_compiled(cmp_gt_2, "cmp_gt_2")
    check_jit_compiled(cmp_ge_2, "cmp_ge_2")

    try:
        assert cmp_le_2(1.0, 2.0) is True
        assert cmp_le_2(2.0, 2.0) is True
        assert cmp_le_2(3.0, 2.0) is False
        assert cmp_eq_2(1.0, 1.0) is True
        assert cmp_eq_2(1.0, 2.0) is False
        assert cmp_ne_2(1.0, 2.0) is True
        assert cmp_ne_2(1.0, 1.0) is False
        assert cmp_gt_2(3.0, 2.0) is True
        assert cmp_gt_2(1.0, 2.0) is False
        assert cmp_ge_2(2.0, 2.0) is True
        assert cmp_ge_2(1.0, 2.0) is False
        print("PASS  Test 2: all six comparison operators")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: six operators — {e}")
        failed += 1

    # ── Test 3: Negative floats ──────────────────────────────────────────

    def cmp_neg_3(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_neg_3(-2.0, -1.0)

    check_jit_compiled(cmp_neg_3, "cmp_neg_3")

    try:
        assert cmp_neg_3(-2.0, -1.0) is True
        assert cmp_neg_3(-1.0, -2.0) is False
        assert cmp_neg_3(-1.0, -1.0) is False
        assert cmp_neg_3(-100.5, -100.4) is True
        assert cmp_neg_3(-0.001, -0.002) is False
        print("PASS  Test 3: negative float comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: negative floats — {e}")
        failed += 1

    # ── Test 4: Comparison with zero ─────────────────────────────────────

    def cmp_zero_4(a):
        if a > 0.0:
            return "positive"
        elif a < 0.0:
            return "negative"
        else:
            return "zero"

    for _ in range(WARMUP):
        cmp_zero_4(1.0)

    check_jit_compiled(cmp_zero_4, "cmp_zero_4")

    try:
        assert cmp_zero_4(1.0) == "positive"
        assert cmp_zero_4(-1.0) == "negative"
        assert cmp_zero_4(0.0) == "zero"
        assert cmp_zero_4(0.001) == "positive"
        assert cmp_zero_4(-0.001) == "negative"
        print("PASS  Test 4: comparison with zero")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: zero comparison — {e}")
        failed += 1

    # ── Test 5: Very large floats ────────────────────────────────────────

    def cmp_large_5(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_large_5(1e300, 1e301)

    check_jit_compiled(cmp_large_5, "cmp_large_5")

    try:
        assert cmp_large_5(1e300, 1e301) is True
        assert cmp_large_5(1e308, 1e307) is False
        assert cmp_large_5(1.7976931348623157e+308, 1.7976931348623157e+308) is False
        assert cmp_large_5(-1e308, 1e308) is True
        print("PASS  Test 5: very large float comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: large floats — {e}")
        failed += 1

    # ── Test 6: Very small floats (subnormals) ───────────────────────────

    def cmp_small_6(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_small_6(5e-324, 1e-323)

    check_jit_compiled(cmp_small_6, "cmp_small_6")

    try:
        assert cmp_small_6(5e-324, 1e-323) is True
        assert cmp_small_6(1e-323, 5e-324) is False
        assert cmp_small_6(0.0, 5e-324) is True
        assert cmp_small_6(5e-324, 0.0) is False
        print("PASS  Test 6: subnormal float comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: subnormals — {e}")
        failed += 1

    # ── Test 7: Deopt — int operand ──────────────────────────────────────

    def cmp_deopt_int_7(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_deopt_int_7(1.0, 2.0)

    check_jit_compiled(cmp_deopt_int_7, "cmp_deopt_int_7")

    try:
        # Float path
        assert cmp_deopt_int_7(1.0, 2.0) is True

        # Int operand (deopt)
        assert cmp_deopt_int_7(1, 2) is True
        assert cmp_deopt_int_7(2, 1) is False

        # Mixed float/int (deopt)
        assert cmp_deopt_int_7(1.0, 2) is True
        assert cmp_deopt_int_7(1, 2.0) is True

        # Float path still works
        assert cmp_deopt_int_7(3.0, 2.0) is False

        print("PASS  Test 7: deopt — int operand")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: int deopt — {e}")
        failed += 1

    # ── Test 8: Deopt — string operand ───────────────────────────────────

    def cmp_deopt_str_8(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_deopt_str_8(1.0, 2.0)

    check_jit_compiled(cmp_deopt_str_8, "cmp_deopt_str_8")

    try:
        # Float path
        assert cmp_deopt_str_8(1.0, 2.0) is True

        # String operand (deopt)
        assert cmp_deopt_str_8("a", "b") is True
        assert cmp_deopt_str_8("b", "a") is False

        # Float path still works
        assert cmp_deopt_str_8(3.0, 2.0) is False

        print("PASS  Test 8: deopt — string operand")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: string deopt — {e}")
        failed += 1

    # ── Test 9: NaN comparisons ──────────────────────────────────────────

    def cmp_nan_eq_9(a, b):
        if a == b:
            return True
        return False

    def cmp_nan_ne_9(a, b):
        if a != b:
            return True
        return False

    def cmp_nan_lt_9(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_nan_eq_9(1.0, 1.0)
        cmp_nan_ne_9(1.0, 2.0)
        cmp_nan_lt_9(1.0, 2.0)

    check_jit_compiled(cmp_nan_eq_9, "cmp_nan_eq_9")

    try:
        nan = float('nan')
        # NaN is not equal to itself
        assert cmp_nan_eq_9(nan, nan) is False
        assert cmp_nan_eq_9(nan, 1.0) is False
        assert cmp_nan_eq_9(1.0, nan) is False
        # NaN is not equal to anything
        assert cmp_nan_ne_9(nan, nan) is True
        assert cmp_nan_ne_9(nan, 1.0) is True
        # NaN is not less than anything
        assert cmp_nan_lt_9(nan, 1.0) is False
        assert cmp_nan_lt_9(1.0, nan) is False
        assert cmp_nan_lt_9(nan, nan) is False
        print("PASS  Test 9: NaN comparisons (IEEE 754)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: NaN — {e}")
        failed += 1

    # ── Test 10: Infinity comparisons ────────────────────────────────────

    def cmp_inf_10(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_inf_10(1.0, 2.0)

    check_jit_compiled(cmp_inf_10, "cmp_inf_10")

    try:
        inf = float('inf')
        ninf = float('-inf')
        assert cmp_inf_10(1.0, inf) is True
        assert cmp_inf_10(inf, 1.0) is False
        assert cmp_inf_10(ninf, 1.0) is True
        assert cmp_inf_10(1.0, ninf) is False
        assert cmp_inf_10(ninf, inf) is True
        assert cmp_inf_10(inf, ninf) is False
        assert cmp_inf_10(inf, inf) is False
        assert cmp_inf_10(ninf, ninf) is False
        print("PASS  Test 10: infinity comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: infinity — {e}")
        failed += 1

    # ── Test 11: Equality of identical floats ────────────────────────────

    def cmp_eq_11(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_eq_11(3.14, 3.14)

    check_jit_compiled(cmp_eq_11, "cmp_eq_11")

    try:
        assert cmp_eq_11(3.14, 3.14) is True
        assert cmp_eq_11(0.0, 0.0) is True
        assert cmp_eq_11(-0.0, -0.0) is True
        assert cmp_eq_11(1e-10, 1e-10) is True
        assert cmp_eq_11(1.0000000000001, 1.0000000000001) is True
        print("PASS  Test 11: equality of identical floats")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: identical equality — {e}")
        failed += 1

    # ── Test 12: Close but not equal floats ──────────────────────────────

    def cmp_close_12(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_close_12(1.0, 1.0)

    check_jit_compiled(cmp_close_12, "cmp_close_12")

    try:
        assert cmp_close_12(0.1 + 0.2, 0.3) is False  # Classic IEEE 754
        assert cmp_close_12(1.0, 1.0 + 1e-15) is False
        assert cmp_close_12(1.0, 1.0 + 1e-16) is True  # Within double precision
        assert cmp_close_12(1e10, 1e10 + 1.0) is False
        print("PASS  Test 12: close but not equal floats")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: close floats — {e}")
        failed += 1

    # ── Test 13: Deopt — mixed float/int in loop ────────────────────────

    def cmp_mixed_13(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_mixed_13(1.0, 2.0)

    check_jit_compiled(cmp_mixed_13, "cmp_mixed_13")

    try:
        results = []
        for i in range(100):
            if i % 2 == 0:
                results.append(cmp_mixed_13(float(i), float(i + 1)))
            else:
                results.append(cmp_mixed_13(i, i + 1))  # int deopt
        assert all(results), "all comparisons should be True (n < n+1)"

        # Float path still works
        assert cmp_mixed_13(1.5, 2.5) is True
        assert cmp_mixed_13(2.5, 1.5) is False

        print("PASS  Test 13: deopt — mixed float/int in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: mixed loop — {e}")
        failed += 1

    # ── Test 14: Rapid comparisons (1000 iterations) ─────────────────────

    def cmp_rapid_14(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_rapid_14(1.0, 2.0)

    check_jit_compiled(cmp_rapid_14, "cmp_rapid_14")

    try:
        for i in range(1000):
            x = float(i)
            y = float(i + 1)
            assert cmp_rapid_14(x, y) is True
            assert cmp_rapid_14(y, x) is False
        print("PASS  Test 14: rapid comparisons (1000 iterations)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: rapid — {e}")
        failed += 1

    # ── Test 15: Stability (10000 calls) ─────────────────────────────────

    def cmp_stable_15(a, b):
        if a <= b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_stable_15(1.0, 2.0)

    check_jit_compiled(cmp_stable_15, "cmp_stable_15")

    try:
        for i in range(10000):
            x = float(i) * 0.1
            y = float(i + 1) * 0.1
            assert cmp_stable_15(x, y) is True
        assert cmp_stable_15(1.0, 1.0) is True  # Equal case
        assert cmp_stable_15(2.0, 1.0) is False
        print("PASS  Test 15: stability (10000 calls)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: stability — {e}")
        failed += 1

    # ── Test 16: Negative zero vs positive zero ──────────────────────────

    def cmp_negzero_eq_16(a, b):
        if a == b:
            return True
        return False

    def cmp_negzero_lt_16(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_negzero_eq_16(0.0, 0.0)
        cmp_negzero_lt_16(0.0, 1.0)

    check_jit_compiled(cmp_negzero_eq_16, "cmp_negzero_eq_16")

    try:
        # IEEE 754: -0.0 == +0.0
        assert cmp_negzero_eq_16(-0.0, 0.0) is True
        assert cmp_negzero_eq_16(0.0, -0.0) is True
        # -0.0 is NOT less than +0.0
        assert cmp_negzero_lt_16(-0.0, 0.0) is False
        assert cmp_negzero_lt_16(0.0, -0.0) is False
        # But they are distinct in memory (copysign check)
        assert math.copysign(1.0, -0.0) == -1.0
        assert math.copysign(1.0, 0.0) == 1.0
        print("PASS  Test 16: negative zero vs positive zero")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: negative zero — {e}")
        failed += 1

    # ── Test 17: Float subclass ──────────────────────────────────────────

    class MyFloat(float):
        pass

    def cmp_subclass_17(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_subclass_17(1.0, 2.0)

    check_jit_compiled(cmp_subclass_17, "cmp_subclass_17")

    try:
        # Float path
        assert cmp_subclass_17(1.0, 2.0) is True

        # Float subclass (deopt — different type)
        mf1 = MyFloat(1.0)
        mf2 = MyFloat(2.0)
        assert cmp_subclass_17(mf1, mf2) is True
        assert cmp_subclass_17(mf2, mf1) is False

        # Mixed
        assert cmp_subclass_17(mf1, 2.0) is True
        assert cmp_subclass_17(1.0, mf2) is True

        # Float path still works
        assert cmp_subclass_17(3.0, 2.0) is False

        print("PASS  Test 17: float subclass comparison")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: float subclass — {e}")
        failed += 1

    # ── Test 18: Chained comparisons (a < b < c) ────────────────────────

    def cmp_chain_18(a, b, c):
        if a < b < c:
            return True
        return False

    for _ in range(WARMUP):
        cmp_chain_18(1.0, 2.0, 3.0)

    check_jit_compiled(cmp_chain_18, "cmp_chain_18")

    try:
        assert cmp_chain_18(1.0, 2.0, 3.0) is True
        assert cmp_chain_18(1.0, 3.0, 2.0) is False
        assert cmp_chain_18(3.0, 2.0, 1.0) is False
        assert cmp_chain_18(1.0, 1.0, 2.0) is False  # Not strictly less
        assert cmp_chain_18(-3.0, -2.0, -1.0) is True
        assert cmp_chain_18(0.0, 0.0, 0.0) is False
        print("PASS  Test 18: chained comparisons (a < b < c)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: chained — {e}")
        failed += 1

    # ── Test 19: Deopt — operand type changes mid-loop ───────────────────

    def cmp_typechange_19(a, b):
        if a >= b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_typechange_19(2.0, 1.0)

    check_jit_compiled(cmp_typechange_19, "cmp_typechange_19")

    try:
        # Start with floats
        assert cmp_typechange_19(2.0, 1.0) is True
        assert cmp_typechange_19(1.0, 2.0) is False

        # Switch to ints (deopt)
        assert cmp_typechange_19(2, 1) is True
        assert cmp_typechange_19(1, 2) is False

        # Switch to strings (deopt)
        assert cmp_typechange_19("b", "a") is True
        assert cmp_typechange_19("a", "b") is False

        # Back to floats
        assert cmp_typechange_19(5.0, 3.0) is True
        assert cmp_typechange_19(3.0, 5.0) is False

        print("PASS  Test 19: deopt — operand type changes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: type change — {e}")
        failed += 1

    # ── Test 20: Equivalence — specialised vs operator module ────────────

    import operator

    def cmp_equiv_20(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_equiv_20(1.0, 2.0)

    check_jit_compiled(cmp_equiv_20, "cmp_equiv_20")

    try:
        test_pairs = [
            (1.0, 2.0), (2.0, 1.0), (1.0, 1.0),
            (-1.0, 1.0), (0.0, 0.0), (-0.0, 0.0),
            (1e-300, 1e-299), (1e300, 1e301),
            (float('inf'), 1.0), (1.0, float('inf')),
            (float('-inf'), float('inf')),
        ]
        for a, b in test_pairs:
            specialised = cmp_equiv_20(a, b)
            reference = operator.lt(a, b)
            assert specialised == reference, (
                f"mismatch for ({a}, {b}): specialised={specialised}, "
                f"operator.lt={reference}"
            )
        print("PASS  Test 20: equivalence — specialised vs operator.lt")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: equivalence — {e}")
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
