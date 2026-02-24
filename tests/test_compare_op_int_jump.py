#!/usr/bin/env python3
"""
test_compare_op_int_jump.py — Correctness and deopt tests for
COMPARE_OP_INT_JUMP specialisation.

Targets: COMPARE_AND_BRANCH (int path) / COMPARE_OP_INT_JUMP.

In CPython 3.12, the adaptive interpreter fuses comparison + conditional
branch for int operands. When the interpreter detects repeated int
comparisons followed by a branch (e.g. if x < y:), it specialises to
avoid generic comparison dispatch and directly compares the C longs.

The CinderX JIT compiles this by emitting GuardType checks on both
operands to confirm they are ints, then performing a direct integer
comparison and branch.

Deopt triggers:
  - Either operand is not an int (float, str, custom type, etc.)
  - Int subclass with overridden __lt__/__gt__/etc.
  - Very large ints (arbitrary precision, beyond C long)

Tests cover:
  - Basic less-than comparison
  - All six comparison operators (lt, le, eq, ne, gt, ge)
  - Negative integers
  - Comparison with zero
  - Large integers (within C long)
  - Deopt: float operand
  - Deopt: string operand
  - Boolean operands (bool is int subclass)
  - Comparison in tight loop (rapid, 1000)
  - Stability (10000 calls)
  - Boundary values (sys.maxsize)
  - Deopt: mixed int/float in loop
  - Chained comparisons (a < b < c)
  - Int subclass
  - Deopt: operand type changes mid-loop
  - Equality and identity
  - Comparison with negative large ints
  - Multiple comparisons in one function
  - Deopt: very large ints (arbitrary precision)
  - Equivalence: specialised vs operator module

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_compare_op_int_jump.py
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

    print("=== COMPARE_OP_INT_JUMP Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Basic less-than comparison ──────────────────────────────

    def cmp_lt_1(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_lt_1(1, 2)

    check_jit_compiled(cmp_lt_1, "cmp_lt_1")

    try:
        assert cmp_lt_1(1, 2) is True
        assert cmp_lt_1(2, 1) is False
        assert cmp_lt_1(1, 1) is False
        assert cmp_lt_1(-1, 0) is True
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
        cmp_le_2(1, 2)
        cmp_eq_2(1, 1)
        cmp_ne_2(1, 2)
        cmp_gt_2(2, 1)
        cmp_ge_2(2, 1)

    check_jit_compiled(cmp_le_2, "cmp_le_2")
    check_jit_compiled(cmp_eq_2, "cmp_eq_2")
    check_jit_compiled(cmp_ne_2, "cmp_ne_2")
    check_jit_compiled(cmp_gt_2, "cmp_gt_2")
    check_jit_compiled(cmp_ge_2, "cmp_ge_2")

    try:
        assert cmp_le_2(1, 2) is True
        assert cmp_le_2(2, 2) is True
        assert cmp_le_2(3, 2) is False
        assert cmp_eq_2(1, 1) is True
        assert cmp_eq_2(1, 2) is False
        assert cmp_ne_2(1, 2) is True
        assert cmp_ne_2(1, 1) is False
        assert cmp_gt_2(3, 2) is True
        assert cmp_gt_2(1, 2) is False
        assert cmp_ge_2(2, 2) is True
        assert cmp_ge_2(1, 2) is False
        print("PASS  Test 2: all six comparison operators")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: six operators — {e}")
        failed += 1

    # ── Test 3: Negative integers ────────────────────────────────────────

    def cmp_neg_3(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_neg_3(-2, -1)

    check_jit_compiled(cmp_neg_3, "cmp_neg_3")

    try:
        assert cmp_neg_3(-2, -1) is True
        assert cmp_neg_3(-1, -2) is False
        assert cmp_neg_3(-1, -1) is False
        assert cmp_neg_3(-1000, -999) is True
        assert cmp_neg_3(-1, 0) is True
        print("PASS  Test 3: negative integer comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: negative ints — {e}")
        failed += 1

    # ── Test 4: Comparison with zero ─────────────────────────────────────

    def cmp_zero_4(a):
        if a > 0:
            return "positive"
        elif a < 0:
            return "negative"
        else:
            return "zero"

    for _ in range(WARMUP):
        cmp_zero_4(1)

    check_jit_compiled(cmp_zero_4, "cmp_zero_4")

    try:
        assert cmp_zero_4(1) == "positive"
        assert cmp_zero_4(-1) == "negative"
        assert cmp_zero_4(0) == "zero"
        assert cmp_zero_4(100) == "positive"
        assert cmp_zero_4(-100) == "negative"
        print("PASS  Test 4: comparison with zero")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: zero comparison — {e}")
        failed += 1

    # ── Test 5: Large integers (within C long) ───────────────────────────

    def cmp_large_5(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_large_5(1000000, 2000000)

    check_jit_compiled(cmp_large_5, "cmp_large_5")

    try:
        assert cmp_large_5(1000000, 2000000) is True
        assert cmp_large_5(2000000, 1000000) is False
        assert cmp_large_5(2**30, 2**30 + 1) is True
        assert cmp_large_5(2**62, 2**62 + 1) is True
        assert cmp_large_5(-2**62, 2**62) is True
        print("PASS  Test 5: large integer comparisons (within C long)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: large ints — {e}")
        failed += 1

    # ── Test 6: Deopt — float operand ────────────────────────────────────

    def cmp_deopt_float_6(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_deopt_float_6(1, 2)

    check_jit_compiled(cmp_deopt_float_6, "cmp_deopt_float_6")

    try:
        # Int path
        assert cmp_deopt_float_6(1, 2) is True

        # Float operand (deopt)
        assert cmp_deopt_float_6(1.0, 2.0) is True
        assert cmp_deopt_float_6(2.0, 1.0) is False

        # Mixed int/float (deopt)
        assert cmp_deopt_float_6(1, 2.0) is True
        assert cmp_deopt_float_6(1.0, 2) is True

        # Int path still works
        assert cmp_deopt_float_6(3, 2) is False

        print("PASS  Test 6: deopt — float operand")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: float deopt — {e}")
        failed += 1

    # ── Test 7: Deopt — string operand ───────────────────────────────────

    def cmp_deopt_str_7(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_deopt_str_7(1, 2)

    check_jit_compiled(cmp_deopt_str_7, "cmp_deopt_str_7")

    try:
        # Int path
        assert cmp_deopt_str_7(1, 2) is True

        # String operand (deopt)
        assert cmp_deopt_str_7("a", "b") is True
        assert cmp_deopt_str_7("b", "a") is False

        # Int path still works
        assert cmp_deopt_str_7(3, 2) is False

        print("PASS  Test 7: deopt — string operand")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: string deopt — {e}")
        failed += 1

    # ── Test 8: Boolean operands (bool is int subclass) ──────────────────

    def cmp_bool_8(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_bool_8(0, 1)

    check_jit_compiled(cmp_bool_8, "cmp_bool_8")

    try:
        # bool is subclass of int: True==1, False==0
        assert cmp_bool_8(False, True) is True
        assert cmp_bool_8(True, False) is False
        assert cmp_bool_8(False, False) is False
        assert cmp_bool_8(True, True) is False
        # Mixed bool/int
        assert cmp_bool_8(False, 1) is True
        assert cmp_bool_8(0, True) is True
        assert cmp_bool_8(True, 2) is True
        assert cmp_bool_8(2, True) is False
        print("PASS  Test 8: boolean operands (bool is int subclass)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: bool — {e}")
        failed += 1

    # ── Test 9: Rapid comparisons (1000 iterations) ──────────────────────

    def cmp_rapid_9(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_rapid_9(1, 2)

    check_jit_compiled(cmp_rapid_9, "cmp_rapid_9")

    try:
        for i in range(1000):
            assert cmp_rapid_9(i, i + 1) is True
            assert cmp_rapid_9(i + 1, i) is False
        print("PASS  Test 9: rapid comparisons (1000 iterations)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: rapid — {e}")
        failed += 1

    # ── Test 10: Stability (10000 calls) ─────────────────────────────────

    def cmp_stable_10(a, b):
        if a <= b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_stable_10(1, 2)

    check_jit_compiled(cmp_stable_10, "cmp_stable_10")

    try:
        for i in range(10000):
            assert cmp_stable_10(i, i + 1) is True
        assert cmp_stable_10(1, 1) is True  # Equal case
        assert cmp_stable_10(2, 1) is False
        print("PASS  Test 10: stability (10000 calls)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: stability — {e}")
        failed += 1

    # ── Test 11: Boundary values (sys.maxsize) ───────────────────────────

    def cmp_boundary_11(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_boundary_11(1, 2)

    check_jit_compiled(cmp_boundary_11, "cmp_boundary_11")

    try:
        maxs = sys.maxsize
        assert cmp_boundary_11(maxs - 1, maxs) is True
        assert cmp_boundary_11(maxs, maxs) is False
        assert cmp_boundary_11(-maxs - 1, -maxs) is True
        assert cmp_boundary_11(-maxs, -maxs) is False
        assert cmp_boundary_11(0, maxs) is True
        assert cmp_boundary_11(-maxs, 0) is True
        print("PASS  Test 11: boundary values (sys.maxsize)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: boundary — {e}")
        failed += 1

    # ── Test 12: Deopt — mixed int/float in loop ────────────────────────

    def cmp_mixed_12(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_mixed_12(1, 2)

    check_jit_compiled(cmp_mixed_12, "cmp_mixed_12")

    try:
        results = []
        for i in range(100):
            if i % 2 == 0:
                results.append(cmp_mixed_12(i, i + 1))
            else:
                results.append(cmp_mixed_12(float(i), float(i + 1)))  # deopt
        assert all(results), "all comparisons should be True (n < n+1)"

        # Int path still works
        assert cmp_mixed_12(1, 2) is True
        assert cmp_mixed_12(2, 1) is False

        print("PASS  Test 12: deopt — mixed int/float in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: mixed loop — {e}")
        failed += 1

    # ── Test 13: Chained comparisons (a < b < c) ────────────────────────

    def cmp_chain_13(a, b, c):
        if a < b < c:
            return True
        return False

    for _ in range(WARMUP):
        cmp_chain_13(1, 2, 3)

    check_jit_compiled(cmp_chain_13, "cmp_chain_13")

    try:
        assert cmp_chain_13(1, 2, 3) is True
        assert cmp_chain_13(1, 3, 2) is False
        assert cmp_chain_13(3, 2, 1) is False
        assert cmp_chain_13(1, 1, 2) is False  # Not strictly less
        assert cmp_chain_13(-3, -2, -1) is True
        assert cmp_chain_13(0, 0, 0) is False
        print("PASS  Test 13: chained comparisons (a < b < c)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: chained — {e}")
        failed += 1

    # ── Test 14: Int subclass ────────────────────────────────────────────

    class MyInt(int):
        pass

    def cmp_subclass_14(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_subclass_14(1, 2)

    check_jit_compiled(cmp_subclass_14, "cmp_subclass_14")

    try:
        # Int path
        assert cmp_subclass_14(1, 2) is True

        # Int subclass (deopt — different type)
        mi1 = MyInt(1)
        mi2 = MyInt(2)
        assert cmp_subclass_14(mi1, mi2) is True
        assert cmp_subclass_14(mi2, mi1) is False

        # Mixed
        assert cmp_subclass_14(mi1, 2) is True
        assert cmp_subclass_14(1, mi2) is True

        # Int path still works
        assert cmp_subclass_14(3, 2) is False

        print("PASS  Test 14: int subclass comparison")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: int subclass — {e}")
        failed += 1

    # ── Test 15: Deopt — operand type changes mid-loop ───────────────────

    def cmp_typechange_15(a, b):
        if a >= b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_typechange_15(2, 1)

    check_jit_compiled(cmp_typechange_15, "cmp_typechange_15")

    try:
        # Start with ints
        assert cmp_typechange_15(2, 1) is True
        assert cmp_typechange_15(1, 2) is False

        # Switch to floats (deopt)
        assert cmp_typechange_15(2.0, 1.0) is True
        assert cmp_typechange_15(1.0, 2.0) is False

        # Switch to strings (deopt)
        assert cmp_typechange_15("b", "a") is True
        assert cmp_typechange_15("a", "b") is False

        # Back to ints
        assert cmp_typechange_15(5, 3) is True
        assert cmp_typechange_15(3, 5) is False

        print("PASS  Test 15: deopt — operand type changes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: type change — {e}")
        failed += 1

    # ── Test 16: Equality and identity ───────────────────────────────────

    def cmp_eq_id_16(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_eq_id_16(1, 1)

    check_jit_compiled(cmp_eq_id_16, "cmp_eq_id_16")

    try:
        assert cmp_eq_id_16(0, 0) is True
        assert cmp_eq_id_16(1, 1) is True
        assert cmp_eq_id_16(-1, -1) is True
        assert cmp_eq_id_16(256, 256) is True  # Cached small int
        assert cmp_eq_id_16(1000, 1000) is True  # Beyond cache
        assert cmp_eq_id_16(1, 2) is False
        assert cmp_eq_id_16(0, 1) is False
        print("PASS  Test 16: equality and identity")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: equality — {e}")
        failed += 1

    # ── Test 17: Comparison with negative large ints ─────────────────────

    def cmp_neglarge_17(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_neglarge_17(-1000000, -999999)

    check_jit_compiled(cmp_neglarge_17, "cmp_neglarge_17")

    try:
        assert cmp_neglarge_17(-1000000, -999999) is True
        assert cmp_neglarge_17(-999999, -1000000) is False
        assert cmp_neglarge_17(-2**60, -2**59) is True
        assert cmp_neglarge_17(-2**59, -2**60) is False
        assert cmp_neglarge_17(-sys.maxsize, sys.maxsize) is True
        print("PASS  Test 17: negative large integer comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: negative large — {e}")
        failed += 1

    # ── Test 18: Multiple comparisons in one function ────────────────────

    def cmp_multi_18(a, b, c):
        if a < b and b < c:
            return "ascending"
        elif a > b and b > c:
            return "descending"
        else:
            return "other"

    for _ in range(WARMUP):
        cmp_multi_18(1, 2, 3)

    check_jit_compiled(cmp_multi_18, "cmp_multi_18")

    try:
        assert cmp_multi_18(1, 2, 3) == "ascending"
        assert cmp_multi_18(3, 2, 1) == "descending"
        assert cmp_multi_18(1, 3, 2) == "other"
        assert cmp_multi_18(1, 1, 1) == "other"
        assert cmp_multi_18(-3, -2, -1) == "ascending"
        assert cmp_multi_18(-1, -2, -3) == "descending"
        print("PASS  Test 18: multiple comparisons in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: multiple — {e}")
        failed += 1

    # ── Test 19: Deopt — very large ints (arbitrary precision) ───────────

    def cmp_bigint_19(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_bigint_19(1, 2)

    check_jit_compiled(cmp_bigint_19, "cmp_bigint_19")

    try:
        # Int path
        assert cmp_bigint_19(1, 2) is True

        # Very large ints beyond C long (deopt — arbitrary precision)
        big1 = 2**100
        big2 = 2**100 + 1
        assert cmp_bigint_19(big1, big2) is True
        assert cmp_bigint_19(big2, big1) is False

        # Negative big ints
        assert cmp_bigint_19(-big2, -big1) is True
        assert cmp_bigint_19(-big1, -big2) is False

        # Int path still works
        assert cmp_bigint_19(3, 2) is False

        print("PASS  Test 19: deopt — very large ints (arbitrary precision)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: big int — {e}")
        failed += 1

    # ── Test 20: Equivalence — specialised vs operator module ────────────

    import operator

    def cmp_equiv_20(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_equiv_20(1, 2)

    check_jit_compiled(cmp_equiv_20, "cmp_equiv_20")

    try:
        test_pairs = [
            (1, 2), (2, 1), (1, 1),
            (-1, 1), (0, 0), (-1, 0),
            (1000000, 1000001), (-1000000, -999999),
            (sys.maxsize - 1, sys.maxsize),
            (-sys.maxsize, sys.maxsize),
            (0, 1),
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
