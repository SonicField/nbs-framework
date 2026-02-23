#!/usr/bin/env python3
"""
test_binary_op_inplace_add_unicode.py — Correctness and deopt tests for
BINARY_OP_INPLACE_ADD_UNICODE specialisation.

Targets: BINARY_OP_INPLACE_ADD_UNICODE.

BINARY_OP_INPLACE_ADD_UNICODE specialises the `s += other_str` pattern
when both operands are exact str objects.  CPython's adaptive interpreter
emits this specialised opcode after observing repeated string += operations.

The key optimisation is that when the left operand's refcount is 1 (no
other references), CPython can resize the string in-place rather than
allocating a new string and copying both halves.  This turns O(n^2)
repeated concatenation into amortised O(n) in some cases.

The JIT specialisation emits GuardType(TUnicodeExact) on both operands,
then calls the inplace unicode_concatenate path.

Deopt triggers:
  - Left operand is not a str (int, bytes, list, custom __iadd__)
  - Right operand is not a str
  - Subclass of str
  - Left operand has refcount > 1 (falls back to regular add, still correct)

Tests cover:
  - Basic str += str
  - Empty string concatenation
  - Single character accumulation
  - Multi-word concatenation in loop
  - Unicode (non-ASCII) concatenation
  - Large string building
  - Deopt: int += int (type change)
  - Deopt: bytes += bytes
  - Deopt: list += list
  - Deopt: custom __iadd__
  - Deopt: str subclass
  - Mixed += in one function (str then int)
  - String with special characters (newlines, tabs, nulls)
  - Repeated += on same variable
  - += with string from function return
  - += preserving string content exactly
  - Rapid type alternation
  - += with f-string result
  - Multiple string variables in one function
  - Accumulation correctness (final result matches join)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Result matches equivalent str.join() or manual construction

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_op_inplace_add_unicode.py
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
                f"{name} not JIT-compiled after {WARMUP} warmup calls. "
                "Test cannot verify JIT path — increase WARMUP or check "
                "cinderjit.auto() is enabled."
            )
        print(f"  WARNING: {name} not found in compiled functions — may not test JIT path")
        return False
    except (ImportError, AttributeError):
        return False


def main():
    print("=== BINARY_OP_INPLACE_ADD_UNICODE Correctness & Deopt Tests ===")
    print()

    try:
        import cinderx
        cinderx.init()
        import cinderjit
        cinderjit.auto()
        try:
            cinderjit.enable_specialized_opcodes()
        except AttributeError:
            pass
    except (ImportError, AttributeError):
        print("SKIP — cinderx/cinderjit not available")
        sys.exit(0)

    passed = 0
    failed = 0

    # ── Test 1: Basic str += str ───────────────────────────────────────

    def concat_basic_1(a, b):
        s = a
        s += b
        return s

    for _ in range(WARMUP):
        concat_basic_1("hello", " world")

    check_jit_compiled(concat_basic_1, "concat_basic_1")

    try:
        assert concat_basic_1("hello", " world") == "hello world"
        assert concat_basic_1("", "") == ""
        assert concat_basic_1("a", "b") == "ab"
        assert concat_basic_1("foo", "") == "foo"
        assert concat_basic_1("", "bar") == "bar"
        print("PASS  Test 1: basic str += str")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic concat — {e}")
        failed += 1

    # ── Test 2: Empty string concatenation ─────────────────────────────

    def concat_empty_2():
        s = ""
        s += ""
        s += ""
        s += ""
        return s

    for _ in range(WARMUP):
        concat_empty_2()

    check_jit_compiled(concat_empty_2, "concat_empty_2")

    try:
        assert concat_empty_2() == ""
        print("PASS  Test 2: empty string concatenation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: empty concat — {e}")
        failed += 1

    # ── Test 3: Single character accumulation ──────────────────────────

    def accumulate_chars_3(chars):
        s = ""
        for c in chars:
            s += c
        return s

    for _ in range(WARMUP):
        accumulate_chars_3("abc")

    check_jit_compiled(accumulate_chars_3, "accumulate_chars_3")

    try:
        assert accumulate_chars_3("abc") == "abc"
        assert accumulate_chars_3("") == ""
        assert accumulate_chars_3("x") == "x"
        assert accumulate_chars_3("hello") == "hello"
        # Verify matches join
        test_str = "the quick brown fox"
        assert accumulate_chars_3(test_str) == "".join(test_str)
        print("PASS  Test 3: single character accumulation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: char accumulation — {e}")
        failed += 1

    # ── Test 4: Multi-word concatenation in loop ──────────────────────

    def concat_words_4(words):
        s = ""
        for w in words:
            s += w
            s += " "
        return s.rstrip()

    words = ["hello", "world", "foo", "bar"]

    for _ in range(WARMUP):
        concat_words_4(words)

    check_jit_compiled(concat_words_4, "concat_words_4")

    try:
        assert concat_words_4(words) == "hello world foo bar"
        assert concat_words_4([]) == ""
        assert concat_words_4(["single"]) == "single"
        print("PASS  Test 4: multi-word concatenation in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: word concat — {e}")
        failed += 1

    # ── Test 5: Unicode (non-ASCII) concatenation ─────────────────────

    def concat_unicode_5(a, b):
        s = a
        s += b
        return s

    for _ in range(WARMUP):
        concat_unicode_5("café", " crème")

    check_jit_compiled(concat_unicode_5, "concat_unicode_5")

    try:
        assert concat_unicode_5("café", " crème") == "café crème"
        assert concat_unicode_5("日本", "語") == "日本語"
        assert concat_unicode_5("α", "β") == "αβ"
        assert concat_unicode_5("🐍", "🎉") == "🐍🎉"
        assert concat_unicode_5("", "ñ") == "ñ"
        print("PASS  Test 5: unicode (non-ASCII) concatenation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: unicode concat — {e}")
        failed += 1

    # ── Test 6: Large string building ─────────────────────────────────

    def build_large_6(n):
        s = ""
        for i in range(n):
            s += "x"
        return s

    for _ in range(WARMUP):
        build_large_6(10)

    check_jit_compiled(build_large_6, "build_large_6")

    try:
        result = build_large_6(1000)
        assert len(result) == 1000
        assert result == "x" * 1000
        assert build_large_6(0) == ""
        assert build_large_6(1) == "x"
        print("PASS  Test 6: large string building (1000 chars)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: large string — {e}")
        failed += 1

    # ── Test 7: Deopt — int += int ────────────────────────────────────

    def iadd_deopt_7(a, b):
        x = a
        x += b
        return x

    for _ in range(WARMUP):
        iadd_deopt_7("hello", " world")

    check_jit_compiled(iadd_deopt_7, "iadd_deopt_7")

    try:
        # String path (specialised)
        assert iadd_deopt_7("hello", " world") == "hello world"

        # Int path (deopt)
        assert iadd_deopt_7(10, 20) == 30
        assert iadd_deopt_7(0, 0) == 0

        # String still works after deopt
        assert iadd_deopt_7("a", "b") == "ab"

        print("PASS  Test 7: deopt — int += int")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: int deopt — {e}")
        failed += 1

    # ── Test 8: Deopt — bytes += bytes ────────────────────────────────

    def iadd_bytes_8(a, b):
        x = a
        x += b
        return x

    for _ in range(WARMUP):
        iadd_bytes_8("hello", " world")

    check_jit_compiled(iadd_bytes_8, "iadd_bytes_8")

    try:
        # String path
        assert iadd_bytes_8("hello", " world") == "hello world"

        # Bytes path (deopt)
        assert iadd_bytes_8(b"hello", b" world") == b"hello world"

        # String still works
        assert iadd_bytes_8("x", "y") == "xy"

        print("PASS  Test 8: deopt — bytes += bytes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: bytes deopt — {e}")
        failed += 1

    # ── Test 9: Deopt — list += list ──────────────────────────────────

    def iadd_list_9(a, b):
        x = a
        x += b
        return x

    for _ in range(WARMUP):
        iadd_list_9("hello", " world")

    check_jit_compiled(iadd_list_9, "iadd_list_9")

    try:
        # String path
        assert iadd_list_9("hello", " world") == "hello world"

        # List path (deopt — list += list extends in-place)
        result = iadd_list_9([1, 2], [3, 4])
        assert result == [1, 2, 3, 4]

        # String still works
        assert iadd_list_9("a", "b") == "ab"

        print("PASS  Test 9: deopt — list += list")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: list deopt — {e}")
        failed += 1

    # ── Test 10: Deopt — custom __iadd__ ──────────────────────────────

    class Accum:
        def __init__(self, val):
            self.val = val
        def __iadd__(self, other):
            self.val += other.val
            return self

    def iadd_custom_10(a, b):
        x = a
        x += b
        return x

    for _ in range(WARMUP):
        iadd_custom_10("hello", " world")

    check_jit_compiled(iadd_custom_10, "iadd_custom_10")

    try:
        # String path
        assert iadd_custom_10("hello", " world") == "hello world"

        # Custom __iadd__ (deopt)
        result = iadd_custom_10(Accum(10), Accum(20))
        assert result.val == 30

        # String still works
        assert iadd_custom_10("a", "b") == "ab"

        print("PASS  Test 10: deopt — custom __iadd__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: custom __iadd__ — {e}")
        failed += 1

    # ── Test 11: Deopt — str subclass ─────────────────────────────────

    class MyStr(str):
        pass

    def iadd_subclass_11(a, b):
        s = a
        s += b
        return s

    for _ in range(WARMUP):
        iadd_subclass_11("hello", " world")

    check_jit_compiled(iadd_subclass_11, "iadd_subclass_11")

    try:
        # Builtin str path
        assert iadd_subclass_11("hello", " world") == "hello world"

        # Str subclass (may deopt — depends on guard type)
        result = iadd_subclass_11(MyStr("hello"), MyStr(" world"))
        assert result == "hello world"

        # Builtin str still works
        assert iadd_subclass_11("x", "y") == "xy"

        print("PASS  Test 11: str subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: str subclass — {e}")
        failed += 1

    # ── Test 12: String with special characters ───────────────────────

    def concat_special_12(a, b):
        s = a
        s += b
        return s

    for _ in range(WARMUP):
        concat_special_12("hello", "\nworld")

    check_jit_compiled(concat_special_12, "concat_special_12")

    try:
        assert concat_special_12("hello", "\nworld") == "hello\nworld"
        assert concat_special_12("a\t", "b\t") == "a\tb\t"
        assert concat_special_12("a\0", "b\0") == "a\0b\0"
        assert concat_special_12("line1\r\n", "line2\r\n") == "line1\r\nline2\r\n"
        assert concat_special_12("'quotes'", '"double"') == "'quotes'\"double\""
        print("PASS  Test 12: special characters (newlines, tabs, nulls)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: special chars — {e}")
        failed += 1

    # ── Test 13: Repeated += on same variable ─────────────────────────

    def repeated_concat_13(n):
        s = ""
        for i in range(n):
            s += str(i)
            s += ","
        return s

    for _ in range(WARMUP):
        repeated_concat_13(5)

    check_jit_compiled(repeated_concat_13, "repeated_concat_13")

    try:
        assert repeated_concat_13(5) == "0,1,2,3,4,"
        assert repeated_concat_13(0) == ""
        assert repeated_concat_13(1) == "0,"
        # Verify against join
        expected = "".join(f"{i}," for i in range(10))
        assert repeated_concat_13(10) == expected
        print("PASS  Test 13: repeated += on same variable")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: repeated concat — {e}")
        failed += 1

    # ── Test 14: += with function return value ────────────────────────

    def get_suffix():
        return " suffix"

    def concat_funcret_14(prefix):
        s = prefix
        s += get_suffix()
        return s

    for _ in range(WARMUP):
        concat_funcret_14("hello")

    check_jit_compiled(concat_funcret_14, "concat_funcret_14")

    try:
        assert concat_funcret_14("hello") == "hello suffix"
        assert concat_funcret_14("") == " suffix"
        assert concat_funcret_14("test") == "test suffix"
        print("PASS  Test 14: += with function return value")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: func return concat — {e}")
        failed += 1

    # ── Test 15: Content preservation ─────────────────────────────────

    def preserve_content_15(parts):
        s = ""
        for p in parts:
            s += p
        return s

    for _ in range(WARMUP):
        preserve_content_15(["a", "b", "c"])

    check_jit_compiled(preserve_content_15, "preserve_content_15")

    try:
        parts = ["the", " quick", " brown", " fox"]
        result = preserve_content_15(parts)
        expected = "".join(parts)
        assert result == expected, f"got {result!r}, expected {expected!r}"

        # Verify character-by-character
        for i, ch in enumerate(expected):
            assert result[i] == ch, f"mismatch at index {i}: {result[i]!r} vs {ch!r}"

        print("PASS  Test 15: content preservation matches join()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: content preservation — {e}")
        failed += 1

    # ── Test 16: Rapid type alternation ───────────────────────────────

    def iadd_alternating_16(a, b):
        x = a
        x += b
        return x

    for _ in range(WARMUP):
        iadd_alternating_16("hello", " world")

    check_jit_compiled(iadd_alternating_16, "iadd_alternating_16")

    try:
        for i in range(100):
            if i % 2 == 0:
                assert iadd_alternating_16("a", "b") == "ab"
            else:
                assert iadd_alternating_16(i, 1) == i + 1

        # Final string check
        assert iadd_alternating_16("x", "y") == "xy"

        print("PASS  Test 16: rapid type alternation (str/int)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: type alternation — {e}")
        failed += 1

    # ── Test 17: += with f-string result ──────────────────────────────

    def concat_fstring_17(items):
        s = ""
        for i, item in enumerate(items):
            s += f"[{i}]={item} "
        return s.rstrip()

    for _ in range(WARMUP):
        concat_fstring_17(["a", "b", "c"])

    check_jit_compiled(concat_fstring_17, "concat_fstring_17")

    try:
        assert concat_fstring_17(["a", "b", "c"]) == "[0]=a [1]=b [2]=c"
        assert concat_fstring_17([]) == ""
        assert concat_fstring_17(["x"]) == "[0]=x"
        print("PASS  Test 17: += with f-string result")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: f-string concat — {e}")
        failed += 1

    # ── Test 18: Multiple string variables in one function ────────────

    def multi_var_18(a, b, c):
        s1 = ""
        s2 = ""
        s1 += a
        s1 += b
        s2 += b
        s2 += c
        return s1, s2

    for _ in range(WARMUP):
        multi_var_18("x", "y", "z")

    check_jit_compiled(multi_var_18, "multi_var_18")

    try:
        assert multi_var_18("x", "y", "z") == ("xy", "yz")
        assert multi_var_18("", "", "") == ("", "")
        assert multi_var_18("hello", " ", "world") == ("hello ", " world")
        print("PASS  Test 18: multiple string variables in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: multi var — {e}")
        failed += 1

    # ── Test 19: Refcount > 1 (shared reference) ─────────────────────

    def concat_shared_ref_19(base, suffix):
        s = base
        alias = s   # refcount > 1: forces copy, not in-place
        s += suffix
        return s, alias

    for _ in range(WARMUP):
        concat_shared_ref_19("hello", " world")

    check_jit_compiled(concat_shared_ref_19, "concat_shared_ref_19")

    try:
        result, alias = concat_shared_ref_19("hello", " world")
        assert result == "hello world"
        assert alias == "hello"  # Original must be unchanged
        assert result is not alias

        result, alias = concat_shared_ref_19("", "x")
        assert result == "x"
        assert alias == ""

        print("PASS  Test 19: refcount > 1 (shared reference preserved)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: shared ref — {e}")
        failed += 1

    # ── Test 20: Accumulation correctness vs join ─────────────────────

    def accumulate_20(items):
        s = ""
        for item in items:
            s += item
        return s

    test_items = [str(i) for i in range(50)]

    for _ in range(WARMUP):
        accumulate_20(test_items)

    check_jit_compiled(accumulate_20, "accumulate_20")

    try:
        result = accumulate_20(test_items)
        expected = "".join(test_items)
        assert result == expected, f"accumulate != join: {result!r} vs {expected!r}"
        assert len(result) == len(expected)

        # Edge cases
        assert accumulate_20([]) == ""
        assert accumulate_20(["only"]) == "only"
        assert accumulate_20(["", "", ""]) == ""

        # Unicode accumulation
        unicode_items = ["café", " ", "naïve", " ", "résumé"]
        assert accumulate_20(unicode_items) == "".join(unicode_items)

        print("PASS  Test 20: accumulation correctness matches join()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: accumulation vs join — {e}")
        failed += 1

    # ── Summary ──────────────────────────────────────────────────────

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
