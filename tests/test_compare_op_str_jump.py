#!/usr/bin/env python3
"""
test_compare_op_str_jump.py — Correctness and deopt tests for
COMPARE_OP_STR_JUMP specialisation.

Targets: COMPARE_AND_BRANCH (str path) / COMPARE_OP_STR_JUMP.

In CPython 3.12, the adaptive interpreter fuses comparison + conditional
branch for string operands. When the interpreter detects repeated string
equality/inequality checks followed by a branch (e.g. if s == "foo":),
it specialises to avoid generic comparison dispatch and directly compares
the strings (typically via pointer identity for interned strings, falling
back to value comparison).

The CinderX JIT compiles this by emitting GuardType checks on both
operands to confirm they are str, then performing a direct string
comparison and branch.

Deopt triggers:
  - Either operand is not a str (int, float, custom type, etc.)
  - String subclass with overridden __eq__/__lt__/etc.

Tests cover:
  - Basic equality comparison
  - Basic inequality comparison
  - Less-than (lexicographic)
  - All six comparison operators
  - Empty string comparisons
  - Single character comparisons
  - Interned string identity
  - Deopt: int operand
  - Deopt: float operand
  - Unicode string comparisons
  - Long string comparisons
  - Case sensitivity
  - Deopt: mixed types in loop
  - Rapid comparisons (1000 iterations)
  - Stability (10000 calls)
  - String subclass
  - Prefix/suffix relationships
  - Chained comparisons
  - Deopt: operand type changes
  - Equivalence: specialised vs operator module

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_compare_op_str_jump.py
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

    print("=== COMPARE_OP_STR_JUMP Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Basic equality comparison ───────────────────────────────

    def cmp_eq_1(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_eq_1("hello", "hello")

    check_jit_compiled(cmp_eq_1, "cmp_eq_1")

    try:
        assert cmp_eq_1("hello", "hello") is True
        assert cmp_eq_1("hello", "world") is False
        assert cmp_eq_1("", "") is True
        assert cmp_eq_1("a", "a") is True
        print("PASS  Test 1: basic equality comparison")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic equality — {e}")
        failed += 1

    # ── Test 2: Basic inequality comparison ──────────────────────────────

    def cmp_ne_2(a, b):
        if a != b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_ne_2("hello", "world")

    check_jit_compiled(cmp_ne_2, "cmp_ne_2")

    try:
        assert cmp_ne_2("hello", "world") is True
        assert cmp_ne_2("hello", "hello") is False
        assert cmp_ne_2("", "a") is True
        assert cmp_ne_2("a", "") is True
        assert cmp_ne_2("", "") is False
        print("PASS  Test 2: basic inequality comparison")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: basic inequality — {e}")
        failed += 1

    # ── Test 3: Less-than (lexicographic) ────────────────────────────────

    def cmp_lt_3(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_lt_3("a", "b")

    check_jit_compiled(cmp_lt_3, "cmp_lt_3")

    try:
        assert cmp_lt_3("a", "b") is True
        assert cmp_lt_3("b", "a") is False
        assert cmp_lt_3("a", "a") is False
        assert cmp_lt_3("abc", "abd") is True
        assert cmp_lt_3("abc", "abc") is False
        assert cmp_lt_3("ab", "abc") is True  # Prefix is less
        print("PASS  Test 3: less-than (lexicographic)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: less-than — {e}")
        failed += 1

    # ── Test 4: All six comparison operators ─────────────────────────────

    def cmp_le_4(a, b):
        if a <= b:
            return True
        return False

    def cmp_gt_4(a, b):
        if a > b:
            return True
        return False

    def cmp_ge_4(a, b):
        if a >= b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_le_4("a", "b")
        cmp_gt_4("b", "a")
        cmp_ge_4("b", "a")

    check_jit_compiled(cmp_le_4, "cmp_le_4")
    check_jit_compiled(cmp_gt_4, "cmp_gt_4")
    check_jit_compiled(cmp_ge_4, "cmp_ge_4")

    try:
        assert cmp_le_4("a", "b") is True
        assert cmp_le_4("a", "a") is True
        assert cmp_le_4("b", "a") is False
        assert cmp_gt_4("b", "a") is True
        assert cmp_gt_4("a", "b") is False
        assert cmp_gt_4("a", "a") is False
        assert cmp_ge_4("b", "a") is True
        assert cmp_ge_4("a", "a") is True
        assert cmp_ge_4("a", "b") is False
        print("PASS  Test 4: all six comparison operators")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: six operators — {e}")
        failed += 1

    # ── Test 5: Empty string comparisons ─────────────────────────────────

    def cmp_empty_5(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_empty_5("", "")

    check_jit_compiled(cmp_empty_5, "cmp_empty_5")

    try:
        assert cmp_empty_5("", "") is True
        assert cmp_empty_5("", "a") is False
        assert cmp_empty_5("a", "") is False
        assert cmp_empty_5("", " ") is False
        print("PASS  Test 5: empty string comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: empty string — {e}")
        failed += 1

    # ── Test 6: Single character comparisons ─────────────────────────────

    def cmp_char_6(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_char_6("a", "b")

    check_jit_compiled(cmp_char_6, "cmp_char_6")

    try:
        assert cmp_char_6("a", "b") is True
        assert cmp_char_6("z", "a") is False
        assert cmp_char_6("A", "a") is True  # 'A' (65) < 'a' (97)
        assert cmp_char_6("0", "9") is True
        assert cmp_char_6("9", "a") is True  # '9' (57) < 'a' (97)
        assert cmp_char_6(" ", "!") is True  # ' ' (32) < '!' (33)
        print("PASS  Test 6: single character comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: single char — {e}")
        failed += 1

    # ── Test 7: Interned string identity ─────────────────────────────────

    def cmp_intern_7(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_intern_7("hello", "hello")

    check_jit_compiled(cmp_intern_7, "cmp_intern_7")

    try:
        # Interned strings (same object)
        s1 = "hello"
        s2 = "hello"
        assert cmp_intern_7(s1, s2) is True
        assert s1 is s2  # Verify interning

        # Non-interned strings (different objects, same value)
        s3 = "".join(["h", "e", "l", "l", "o"])
        assert cmp_intern_7(s1, s3) is True
        # s3 may or may not be interned — value equality must still work

        # Different strings
        assert cmp_intern_7("hello", "world") is False

        print("PASS  Test 7: interned string identity")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: interned — {e}")
        failed += 1

    # ── Test 8: Deopt — int operand ──────────────────────────────────────

    def cmp_deopt_int_8(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_deopt_int_8("hello", "hello")

    check_jit_compiled(cmp_deopt_int_8, "cmp_deopt_int_8")

    try:
        # String path
        assert cmp_deopt_int_8("hello", "hello") is True

        # Int operand (deopt)
        assert cmp_deopt_int_8(1, 1) is True
        assert cmp_deopt_int_8(1, 2) is False

        # String path still works
        assert cmp_deopt_int_8("foo", "bar") is False

        print("PASS  Test 8: deopt — int operand")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: int deopt — {e}")
        failed += 1

    # ── Test 9: Deopt — float operand ────────────────────────────────────

    def cmp_deopt_float_9(a, b):
        if a < b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_deopt_float_9("a", "b")

    check_jit_compiled(cmp_deopt_float_9, "cmp_deopt_float_9")

    try:
        # String path
        assert cmp_deopt_float_9("a", "b") is True

        # Float operand (deopt)
        assert cmp_deopt_float_9(1.0, 2.0) is True
        assert cmp_deopt_float_9(2.0, 1.0) is False

        # String path still works
        assert cmp_deopt_float_9("z", "a") is False

        print("PASS  Test 9: deopt — float operand")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: float deopt — {e}")
        failed += 1

    # ── Test 10: Unicode string comparisons ──────────────────────────────

    def cmp_unicode_10(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_unicode_10("hello", "hello")

    check_jit_compiled(cmp_unicode_10, "cmp_unicode_10")

    try:
        assert cmp_unicode_10("\u00e9", "\u00e9") is True  # é == é
        assert cmp_unicode_10("\u00e9", "e") is False
        assert cmp_unicode_10("\u4e16\u754c", "\u4e16\u754c") is True  # 世界
        assert cmp_unicode_10("\u4e16\u754c", "\u4e16") is False
        assert cmp_unicode_10("\U0001f600", "\U0001f600") is True  # 😀
        assert cmp_unicode_10("\U0001f600", "\U0001f601") is False
        print("PASS  Test 10: unicode string comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: unicode — {e}")
        failed += 1

    # ── Test 11: Long string comparisons ─────────────────────────────────

    def cmp_long_11(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_long_11("x", "x")

    check_jit_compiled(cmp_long_11, "cmp_long_11")

    try:
        long1 = "a" * 10000
        long2 = "a" * 10000
        long3 = "a" * 9999 + "b"
        assert cmp_long_11(long1, long2) is True
        assert cmp_long_11(long1, long3) is False
        assert cmp_long_11(long1, long1[:9999]) is False  # Different length
        print("PASS  Test 11: long string comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: long strings — {e}")
        failed += 1

    # ── Test 12: Case sensitivity ────────────────────────────────────────

    def cmp_case_12(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_case_12("Hello", "Hello")

    check_jit_compiled(cmp_case_12, "cmp_case_12")

    try:
        assert cmp_case_12("Hello", "Hello") is True
        assert cmp_case_12("Hello", "hello") is False
        assert cmp_case_12("HELLO", "hello") is False
        assert cmp_case_12("ABC", "abc") is False
        assert cmp_case_12("abc", "ABC") is False
        print("PASS  Test 12: case sensitivity")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: case — {e}")
        failed += 1

    # ── Test 13: Deopt — mixed types in loop ─────────────────────────────

    def cmp_mixed_13(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_mixed_13("a", "a")

    check_jit_compiled(cmp_mixed_13, "cmp_mixed_13")

    try:
        results = []
        for i in range(100):
            if i % 3 == 0:
                results.append(cmp_mixed_13("x", "x"))  # str
            elif i % 3 == 1:
                results.append(cmp_mixed_13(1, 1))  # int deopt
            else:
                results.append(cmp_mixed_13(1.0, 1.0))  # float deopt
        assert all(results), "all comparisons should be True (a == a)"

        # String path still works
        assert cmp_mixed_13("hello", "hello") is True
        assert cmp_mixed_13("hello", "world") is False

        print("PASS  Test 13: deopt — mixed types in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: mixed loop — {e}")
        failed += 1

    # ── Test 14: Rapid comparisons (1000 iterations) ─────────────────────

    def cmp_rapid_14(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_rapid_14("test", "test")

    check_jit_compiled(cmp_rapid_14, "cmp_rapid_14")

    try:
        for i in range(1000):
            s = str(i)
            assert cmp_rapid_14(s, s) is True
            assert cmp_rapid_14(s, s + "x") is False
        print("PASS  Test 14: rapid comparisons (1000 iterations)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: rapid — {e}")
        failed += 1

    # ── Test 15: Stability (10000 calls) ─────────────────────────────────

    def cmp_stable_15(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_stable_15("stable", "stable")

    check_jit_compiled(cmp_stable_15, "cmp_stable_15")

    try:
        for i in range(10000):
            assert cmp_stable_15("stable", "stable") is True
        assert cmp_stable_15("stable", "unstable") is False
        print("PASS  Test 15: stability (10000 calls)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: stability — {e}")
        failed += 1

    # ── Test 16: String subclass ─────────────────────────────────────────

    class MyStr(str):
        pass

    def cmp_subclass_16(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_subclass_16("hello", "hello")

    check_jit_compiled(cmp_subclass_16, "cmp_subclass_16")

    try:
        # String path
        assert cmp_subclass_16("hello", "hello") is True

        # String subclass (deopt)
        ms1 = MyStr("hello")
        ms2 = MyStr("hello")
        assert cmp_subclass_16(ms1, ms2) is True
        assert cmp_subclass_16(ms1, MyStr("world")) is False

        # Mixed
        assert cmp_subclass_16(ms1, "hello") is True
        assert cmp_subclass_16("hello", ms1) is True

        # String path still works
        assert cmp_subclass_16("foo", "bar") is False

        print("PASS  Test 16: string subclass comparison")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: str subclass — {e}")
        failed += 1

    # ── Test 17: Prefix/suffix relationships ─────────────────────────────

    def cmp_prefix_lt_17(a, b):
        if a < b:
            return True
        return False

    def cmp_prefix_eq_17(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_prefix_lt_17("abc", "abcd")
        cmp_prefix_eq_17("abc", "abc")

    check_jit_compiled(cmp_prefix_lt_17, "cmp_prefix_lt_17")

    try:
        # Prefix is less than the full string
        assert cmp_prefix_lt_17("abc", "abcd") is True
        assert cmp_prefix_lt_17("abcd", "abc") is False
        assert cmp_prefix_eq_17("abc", "abcd") is False
        assert cmp_prefix_eq_17("abc", "abc") is True
        # Empty string is prefix of everything
        assert cmp_prefix_lt_17("", "a") is True
        assert cmp_prefix_lt_17("a", "") is False
        print("PASS  Test 17: prefix/suffix relationships")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: prefix — {e}")
        failed += 1

    # ── Test 18: Chained comparisons ─────────────────────────────────────

    def cmp_chain_18(a, b, c):
        if a < b < c:
            return True
        return False

    for _ in range(WARMUP):
        cmp_chain_18("a", "b", "c")

    check_jit_compiled(cmp_chain_18, "cmp_chain_18")

    try:
        assert cmp_chain_18("a", "b", "c") is True
        assert cmp_chain_18("a", "c", "b") is False
        assert cmp_chain_18("c", "b", "a") is False
        assert cmp_chain_18("a", "a", "b") is False  # Not strictly less
        assert cmp_chain_18("x", "y", "z") is True
        assert cmp_chain_18("", "a", "b") is True
        print("PASS  Test 18: chained comparisons")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: chained — {e}")
        failed += 1

    # ── Test 19: Deopt — operand type changes ────────────────────────────

    def cmp_typechange_19(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_typechange_19("x", "x")

    check_jit_compiled(cmp_typechange_19, "cmp_typechange_19")

    try:
        # Start with strings
        assert cmp_typechange_19("x", "x") is True
        assert cmp_typechange_19("x", "y") is False

        # Switch to ints (deopt)
        assert cmp_typechange_19(1, 1) is True
        assert cmp_typechange_19(1, 2) is False

        # Switch to floats (deopt)
        assert cmp_typechange_19(1.0, 1.0) is True
        assert cmp_typechange_19(1.0, 2.0) is False

        # Switch to tuples (deopt)
        assert cmp_typechange_19((1,), (1,)) is True
        assert cmp_typechange_19((1,), (2,)) is False

        # Back to strings
        assert cmp_typechange_19("hello", "hello") is True
        assert cmp_typechange_19("hello", "world") is False

        print("PASS  Test 19: deopt — operand type changes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: type change — {e}")
        failed += 1

    # ── Test 20: Equivalence — specialised vs operator module ────────────

    import operator

    def cmp_equiv_20(a, b):
        if a == b:
            return True
        return False

    for _ in range(WARMUP):
        cmp_equiv_20("test", "test")

    check_jit_compiled(cmp_equiv_20, "cmp_equiv_20")

    try:
        test_pairs = [
            ("hello", "hello"), ("hello", "world"),
            ("", ""), ("", "a"), ("a", ""),
            ("abc", "abd"), ("abc", "abc"),
            ("ABC", "abc"), ("abc", "ABC"),
            ("a" * 100, "a" * 100), ("a" * 100, "a" * 99 + "b"),
            ("\u00e9", "\u00e9"), ("\u00e9", "e"),
        ]
        for a, b in test_pairs:
            specialised = cmp_equiv_20(a, b)
            reference = operator.eq(a, b)
            assert specialised == reference, (
                f"mismatch for ({a!r}, {b!r}): specialised={specialised}, "
                f"operator.eq={reference}"
            )
        print("PASS  Test 20: equivalence — specialised vs operator.eq")
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
