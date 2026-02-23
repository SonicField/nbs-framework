#!/usr/bin/env python3
"""
test_compare_op_str.py — Correctness and deopt tests for
COMPARE_OP_STR specialisation.

Targets: COMPARE_OP_STR.

COMPARE_OP_STR specialises comparison operations (== and !=, primarily)
when both operands are strings. Instead of going through the generic
COMPARE_OP path (which must check types, look up __eq__/__ne__, and
dispatch through the rich comparison protocol), the specialisation
compares strings directly using pointer equality (interned strings)
and then unicode_compare if needed.

The adaptive specialiser emits COMPARE_OP_STR after observing repeated
comparisons of string operands. CPython 3.12 primarily specialises
== and != for strings (not ordering comparisons).

Deopt triggers:
  - One or both operands are not str (int, bytes, custom __eq__)
  - Operand type changes between calls

Tests cover:
  - Basic equality (==)
  - Basic inequality (!=)
  - Interned string identity (same object)
  - Non-interned string comparison
  - Empty string comparisons
  - Single character strings
  - Long string comparisons
  - Unicode strings (non-ASCII)
  - Deopt: switch to int operand
  - Deopt: switch to bytes operand
  - Deopt: switch to custom __eq__
  - Deopt: cross-type comparison (str == int)
  - Loop with string comparison
  - Rapid str-vs-int alternation
  - String ordering (<, >, <=, >=) — lexicographic
  - Case sensitivity
  - String concatenation then compare
  - Interned vs non-interned equivalence
  - None comparison (str == None is False)
  - Equivalence: (a == b) vs str.__eq__(a, b)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Semantic equivalence with known-good reference implementations

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_compare_op_str.py
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
        def str_eq(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq("hello", "hello")
        check_jit_compiled(str_eq, "str_eq")

        assert str_eq("hello", "hello") is True
        assert str_eq("hello", "world") is False
        assert str_eq("abc", "abc") is True
        assert str_eq("abc", "abd") is False
        assert str_eq("", "") is True
        print("  PASS: test_basic_equality")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_equality — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Basic inequality (!=)
    # ------------------------------------------------------------------
    try:
        def str_ne(a, b):
            return a != b

        for _ in range(WARMUP):
            str_ne("hello", "world")
        check_jit_compiled(str_ne, "str_ne")

        assert str_ne("hello", "world") is True
        assert str_ne("hello", "hello") is False
        assert str_ne("abc", "abd") is True
        assert str_ne("", "") is False
        assert str_ne("a", "b") is True
        print("  PASS: test_basic_inequality")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_inequality — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Interned string identity (same object)
    # ------------------------------------------------------------------
    try:
        def str_eq_interned(a, b):
            return a == b

        # String literals are interned by CPython — same object
        s1 = "hello"
        s2 = "hello"
        assert s1 is s2, "Expected interned strings to be same object"

        for _ in range(WARMUP):
            str_eq_interned(s1, s2)
        check_jit_compiled(str_eq_interned, "str_eq_interned")

        assert str_eq_interned(s1, s2) is True

        # sys.intern() explicitly
        s3 = sys.intern("dynamic_" + "string")
        s4 = sys.intern("dynamic_" + "string")
        assert s3 is s4
        assert str_eq_interned(s3, s4) is True
        print("  PASS: test_interned_string_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_interned_string_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Non-interned string comparison
    # ------------------------------------------------------------------
    try:
        def str_eq_non_intern(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_non_intern("test", "test")
        check_jit_compiled(str_eq_non_intern, "str_eq_non_intern")

        # Construct strings that won't be interned
        s1 = "".join(["h", "e", "l", "l", "o"])
        s2 = "".join(["h", "e", "l", "l", "o"])
        # They should be equal but not necessarily the same object
        assert str_eq_non_intern(s1, s2) is True
        assert str_eq_non_intern(s1, "hello") is True

        s3 = "".join(["w", "o", "r", "l", "d"])
        assert str_eq_non_intern(s1, s3) is False
        print("  PASS: test_non_interned_comparison")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_non_interned_comparison — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Empty string comparisons
    # ------------------------------------------------------------------
    try:
        def str_eq_empty(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_empty("", "")
        check_jit_compiled(str_eq_empty, "str_eq_empty")

        assert str_eq_empty("", "") is True
        assert str_eq_empty("", "a") is False
        assert str_eq_empty("a", "") is False
        assert str_eq_empty("", "hello") is False

        def str_ne_empty(a, b):
            return a != b
        for _ in range(WARMUP):
            str_ne_empty("", "x")
        assert str_ne_empty("", "") is False
        assert str_ne_empty("", "x") is True
        print("  PASS: test_empty_string_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_string_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Single character strings
    # ------------------------------------------------------------------
    try:
        def str_eq_char(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_char("a", "a")
        check_jit_compiled(str_eq_char, "str_eq_char")

        assert str_eq_char("a", "a") is True
        assert str_eq_char("a", "b") is False
        assert str_eq_char("z", "z") is True
        assert str_eq_char("A", "a") is False  # case sensitive
        assert str_eq_char("0", "0") is True
        assert str_eq_char(" ", " ") is True
        assert str_eq_char("\n", "\n") is True
        assert str_eq_char("\t", " ") is False
        print("  PASS: test_single_char_strings")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_single_char_strings — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Long string comparisons
    # ------------------------------------------------------------------
    try:
        def str_eq_long(a, b):
            return a == b

        long_a = "x" * 10000
        long_b = "x" * 10000
        long_c = "x" * 9999 + "y"

        for _ in range(WARMUP):
            str_eq_long(long_a, long_b)
        check_jit_compiled(str_eq_long, "str_eq_long")

        assert str_eq_long(long_a, long_b) is True
        assert str_eq_long(long_a, long_c) is False  # differ at last char
        assert str_eq_long(long_a, "x" * 9999) is False  # differ in length

        # Very long equal strings
        mega_a = "abc" * 5000
        mega_b = "abc" * 5000
        assert str_eq_long(mega_a, mega_b) is True
        print("  PASS: test_long_string_comparisons")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_long_string_comparisons — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Unicode strings (non-ASCII)
    # ------------------------------------------------------------------
    try:
        def str_eq_unicode(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_unicode("caf\u00e9", "caf\u00e9")
        check_jit_compiled(str_eq_unicode, "str_eq_unicode")

        assert str_eq_unicode("caf\u00e9", "caf\u00e9") is True
        assert str_eq_unicode("caf\u00e9", "cafe") is False
        assert str_eq_unicode("\u20ac", "\u20ac") is True  # Euro sign
        assert str_eq_unicode("\u20ac", "$") is False
        assert str_eq_unicode("\U0001f600", "\U0001f600") is True  # emoji
        assert str_eq_unicode("\U0001f600", "\U0001f601") is False
        assert str_eq_unicode("\u00e9", "\u0065\u0301") is False  # NFC vs NFD
        print("  PASS: test_unicode_strings")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_unicode_strings — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt str -> int
    # ------------------------------------------------------------------
    try:
        def cmp_deopt_int(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_deopt_int("hello", "hello")
        check_jit_compiled(cmp_deopt_int, "cmp_deopt_int")

        assert cmp_deopt_int("hello", "hello") is True
        # Deopt: int operands
        assert cmp_deopt_int(42, 42) is True
        assert cmp_deopt_int(42, 43) is False
        # Cross-type
        assert cmp_deopt_int("42", 42) is False
        # Back to string
        assert cmp_deopt_int("hello", "hello") is True
        print("  PASS: test_deopt_str_to_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_str_to_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt str -> bytes
    # ------------------------------------------------------------------
    try:
        def cmp_deopt_bytes(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_deopt_bytes("hello", "hello")
        check_jit_compiled(cmp_deopt_bytes, "cmp_deopt_bytes")

        assert cmp_deopt_bytes("hello", "hello") is True
        # Deopt: bytes operands
        assert cmp_deopt_bytes(b"hello", b"hello") is True
        assert cmp_deopt_bytes(b"hello", b"world") is False
        # Cross-type (str == bytes is always False, no TypeError for ==)
        assert cmp_deopt_bytes("hello", b"hello") is False
        # Back to string
        assert cmp_deopt_bytes("test", "test") is True
        print("  PASS: test_deopt_str_to_bytes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_str_to_bytes — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt str -> custom __eq__
    # ------------------------------------------------------------------
    try:
        class MatchAll:
            def __eq__(self, other):
                return True

        class MatchNone:
            def __eq__(self, other):
                return False

        def cmp_deopt_custom(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_deopt_custom("x", "x")
        check_jit_compiled(cmp_deopt_custom, "cmp_deopt_custom")

        assert cmp_deopt_custom("x", "x") is True
        # Deopt: custom __eq__
        assert cmp_deopt_custom(MatchAll(), "anything") is True
        assert cmp_deopt_custom(MatchNone(), "anything") is False
        # Back to string
        assert cmp_deopt_custom("abc", "abc") is True
        assert cmp_deopt_custom("abc", "def") is False
        print("  PASS: test_deopt_custom_eq")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_custom_eq — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Cross-type comparison (str == int is False)
    # ------------------------------------------------------------------
    try:
        def cmp_cross(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_cross("hello", "hello")
        check_jit_compiled(cmp_cross, "cmp_cross")

        # str == non-str is always False (no TypeError for ==)
        assert cmp_cross("42", 42) is False
        assert cmp_cross("3.14", 3.14) is False
        assert cmp_cross("True", True) is False
        assert cmp_cross("None", None) is False
        assert cmp_cross("[]", []) is False
        # str == str still works
        assert cmp_cross("42", "42") is True
        print("  PASS: test_cross_type_comparison")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_cross_type_comparison — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Loop with string comparison
    # ------------------------------------------------------------------
    try:
        def count_matches(lst, target):
            count = 0
            for s in lst:
                if s == target:
                    count += 1
            return count

        data = ["apple", "banana", "apple", "cherry", "apple", "banana"]
        for _ in range(WARMUP):
            count_matches(data, "apple")
        check_jit_compiled(count_matches, "count_matches")

        assert count_matches(data, "apple") == 3
        assert count_matches(data, "banana") == 2
        assert count_matches(data, "cherry") == 1
        assert count_matches(data, "grape") == 0
        assert count_matches([], "apple") == 0

        # Large list
        big_data = ["match" if i % 10 == 0 else "other" for i in range(1000)]
        assert count_matches(big_data, "match") == 100
        print("  PASS: test_loop_string_comparison")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_loop_string_comparison — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid str-vs-int alternation
    # ------------------------------------------------------------------
    try:
        def cmp_poly(a, b):
            return a == b

        for _ in range(WARMUP):
            cmp_poly("x", "x")
        check_jit_compiled(cmp_poly, "cmp_poly")

        for cycle in range(50):
            r_str = cmp_poly("hello", "hello")
            r_int = cmp_poly(42, 42)
            assert r_str is True, f"str compare failed at cycle {cycle}"
            assert r_int is True, f"int compare failed at cycle {cycle}"

            r_str2 = cmp_poly("hello", "world")
            r_int2 = cmp_poly(42, 43)
            assert r_str2 is False, f"str ne failed at cycle {cycle}"
            assert r_int2 is False, f"int ne failed at cycle {cycle}"
        print("  PASS: test_rapid_str_int_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_str_int_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: String ordering (<, >, <=, >=) — lexicographic
    # ------------------------------------------------------------------
    try:
        def str_lt(a, b):
            return a < b

        def str_le(a, b):
            return a <= b

        for _ in range(WARMUP):
            str_lt("apple", "banana")
        for _ in range(WARMUP):
            str_le("apple", "banana")
        check_jit_compiled(str_lt, "str_lt")
        check_jit_compiled(str_le, "str_le")

        assert str_lt("apple", "banana") is True
        assert str_lt("banana", "apple") is False
        assert str_lt("apple", "apple") is False
        assert str_lt("abc", "abd") is True
        assert str_lt("a", "aa") is True  # shorter string is "less"
        assert str_lt("", "a") is True

        assert str_le("apple", "apple") is True
        assert str_le("apple", "banana") is True
        assert str_le("banana", "apple") is False

        def str_gt(a, b):
            return a > b
        for _ in range(WARMUP):
            str_gt("banana", "apple")
        assert str_gt("banana", "apple") is True
        assert str_gt("apple", "banana") is False
        assert str_gt("apple", "apple") is False
        print("  PASS: test_string_ordering")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_string_ordering — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Case sensitivity
    # ------------------------------------------------------------------
    try:
        def str_eq_case(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_case("Hello", "Hello")
        check_jit_compiled(str_eq_case, "str_eq_case")

        assert str_eq_case("Hello", "Hello") is True
        assert str_eq_case("Hello", "hello") is False
        assert str_eq_case("HELLO", "hello") is False
        assert str_eq_case("ABC", "abc") is False
        assert str_eq_case("a", "A") is False

        # Ordering: uppercase < lowercase in ASCII
        def str_lt_case(a, b):
            return a < b
        for _ in range(WARMUP):
            str_lt_case("A", "a")
        assert str_lt_case("A", "a") is True  # ord('A')=65 < ord('a')=97
        assert str_lt_case("Z", "a") is True  # ord('Z')=90 < ord('a')=97
        print("  PASS: test_case_sensitivity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_case_sensitivity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: String concatenation then compare
    # ------------------------------------------------------------------
    try:
        def concat_and_compare(a, b, target):
            return (a + b) == target

        for _ in range(WARMUP):
            concat_and_compare("hel", "lo", "hello")
        check_jit_compiled(concat_and_compare, "concat_and_compare")

        assert concat_and_compare("hel", "lo", "hello") is True
        assert concat_and_compare("hel", "lo", "world") is False
        assert concat_and_compare("", "hello", "hello") is True
        assert concat_and_compare("hello", "", "hello") is True
        assert concat_and_compare("", "", "") is True
        assert concat_and_compare("a", "b", "ab") is True
        assert concat_and_compare("a", "b", "ba") is False
        print("  PASS: test_concat_then_compare")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_concat_then_compare — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Interned vs non-interned equivalence
    # ------------------------------------------------------------------
    try:
        def str_eq_intern_test(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_intern_test("test", "test")
        check_jit_compiled(str_eq_intern_test, "str_eq_intern_test")

        # Interned (literal)
        interned = "hello_world"
        # Non-interned (constructed)
        non_interned = "".join(["hello", "_", "world"])

        # Must produce same == result regardless of interning
        assert str_eq_intern_test(interned, non_interned) is True
        assert str_eq_intern_test(non_interned, interned) is True
        assert str_eq_intern_test(non_interned, non_interned) is True

        # Explicitly intern and compare
        force_interned = sys.intern(non_interned)
        assert str_eq_intern_test(interned, force_interned) is True
        assert str_eq_intern_test(force_interned, non_interned) is True
        print("  PASS: test_interned_vs_non_interned")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_interned_vs_non_interned — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: None comparison (str == None is False)
    # ------------------------------------------------------------------
    try:
        def str_eq_none(a, b):
            return a == b

        for _ in range(WARMUP):
            str_eq_none("hello", "hello")
        check_jit_compiled(str_eq_none, "str_eq_none")

        assert str_eq_none("hello", "hello") is True
        # str == None is False (not TypeError)
        assert str_eq_none("hello", None) is False
        assert str_eq_none(None, "hello") is False
        assert str_eq_none(None, None) is True
        # Back to string
        assert str_eq_none("test", "test") is True
        print("  PASS: test_none_comparison")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_none_comparison — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — (a == b) vs str.__eq__(a, b)
    # ------------------------------------------------------------------
    try:
        def str_eq_op(a, b):
            return a == b

        def str_eq_dunder(a, b):
            return str.__eq__(a, b)

        for _ in range(WARMUP):
            str_eq_op("test", "test")
        check_jit_compiled(str_eq_op, "str_eq_op")

        cases = [
            ("hello", "hello"),
            ("hello", "world"),
            ("", ""),
            ("", "x"),
            ("abc", "abd"),
            ("x" * 1000, "x" * 1000),
            ("x" * 1000, "x" * 999 + "y"),
            ("caf\u00e9", "caf\u00e9"),
            ("caf\u00e9", "cafe"),
            ("\U0001f600", "\U0001f600"),
        ]
        for a, b in cases:
            r_op = str_eq_op(a, b)
            r_du = str_eq_dunder(a, b)
            assert r_op == r_du, (
                f"Mismatch for ({a!r}, {b!r}): "
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
    print(f"\nCOMPARE_OP_STR: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
