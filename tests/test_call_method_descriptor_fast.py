#!/usr/bin/env python3
"""
test_call_method_descriptor_fast.py — Correctness and deopt tests for
CALL_METHOD_DESCRIPTOR_FAST specialisation.

Targets: CALL_METHOD_DESCRIPTOR_FAST.

CALL_METHOD_DESCRIPTOR_FAST specialises calls to C-implemented method
descriptors that use the METH_FASTCALL calling convention (positional args
passed as a C array, no keyword args). CPython's adaptive interpreter
replaces the generic CALL opcode with this specialisation when it detects
repeated calls to such methods.

Examples of METH_FASTCALL methods include str.join(), str.split(),
list.append(), list.insert(), dict.get(), str.startswith(),
str.endswith(), etc.

Mechanism:
1. Adaptive interpreter detects CALL to a method_descriptor with METH_FASTCALL
2. Replaces CALL with CALL_METHOD_DESCRIPTOR_FAST
3. CinderX JIT emits GuardType on the receiver + direct C function pointer call
4. Args passed as C array — no tuple packing, no kwarg dict

Deopt triggers:
  - Receiver type changes (different type with same method name)
  - Method is overridden on the instance or subclass
  - Method descriptor replaced on the type

Tests cover:
  - str.join()
  - str.split() with separator arg
  - str.startswith()
  - str.endswith()
  - str.replace()
  - list.append()
  - list.insert()
  - list.index()
  - dict.get() with default
  - dict.pop() with default
  - str.encode()
  - bytes.decode()
  - Deopt: different type with same method name
  - Deopt: subclass overriding method
  - Rapid method calls (1000 cycles)
  - Stability — 10000 calls
  - Method with multiple positional args
  - str.count()
  - list.count()
  - Equivalence: obj.method(arg) vs type.method(obj, arg)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_method_descriptor_fast.py
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
    # Test 1: str.join()
    # ------------------------------------------------------------------
    try:
        def call_join(sep, parts):
            return sep.join(parts)

        for _ in range(WARMUP):
            call_join(", ", ["a", "b", "c"])
        check_jit_compiled(call_join, "call_join")

        assert call_join(", ", ["a", "b", "c"]) == "a, b, c"
        assert call_join("-", ["x", "y"]) == "x-y"
        assert call_join("", ["a", "b"]) == "ab"
        assert call_join(", ", []) == ""
        assert call_join(", ", ["only"]) == "only"
        print("  PASS: test_str_join")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_join — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: str.split() with separator arg
    # ------------------------------------------------------------------
    try:
        def call_split(s, sep):
            return s.split(sep)

        for _ in range(WARMUP):
            call_split("a,b,c", ",")
        check_jit_compiled(call_split, "call_split")

        assert call_split("a,b,c", ",") == ["a", "b", "c"]
        assert call_split("hello world", " ") == ["hello", "world"]
        assert call_split("no-sep", ",") == ["no-sep"]
        assert call_split("", ",") == [""]
        assert call_split("a::b::c", "::") == ["a", "b", "c"]
        print("  PASS: test_str_split")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_split — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: str.startswith()
    # ------------------------------------------------------------------
    try:
        def call_startswith(s, prefix):
            return s.startswith(prefix)

        for _ in range(WARMUP):
            call_startswith("hello world", "hello")
        check_jit_compiled(call_startswith, "call_startswith")

        assert call_startswith("hello world", "hello") is True
        assert call_startswith("hello world", "world") is False
        assert call_startswith("hello", "hello") is True
        assert call_startswith("hello", "hello world") is False
        assert call_startswith("", "") is True
        assert call_startswith("hello", "") is True
        print("  PASS: test_str_startswith")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_startswith — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: str.endswith()
    # ------------------------------------------------------------------
    try:
        def call_endswith(s, suffix):
            return s.endswith(suffix)

        for _ in range(WARMUP):
            call_endswith("hello world", "world")
        check_jit_compiled(call_endswith, "call_endswith")

        assert call_endswith("hello world", "world") is True
        assert call_endswith("hello world", "hello") is False
        assert call_endswith("hello", "hello") is True
        assert call_endswith("hello", "lo") is True
        assert call_endswith("", "") is True
        assert call_endswith("hello", "") is True
        print("  PASS: test_str_endswith")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_endswith — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: str.replace()
    # ------------------------------------------------------------------
    try:
        def call_replace(s, old, new):
            return s.replace(old, new)

        for _ in range(WARMUP):
            call_replace("hello world", "world", "python")
        check_jit_compiled(call_replace, "call_replace")

        assert call_replace("hello world", "world", "python") == "hello python"
        assert call_replace("aaa", "a", "b") == "bbb"
        assert call_replace("hello", "x", "y") == "hello"  # no match
        assert call_replace("", "a", "b") == ""
        assert call_replace("abcabc", "abc", "X") == "XX"
        print("  PASS: test_str_replace")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_replace — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: list.append()
    # ------------------------------------------------------------------
    try:
        def call_append(lst, val):
            lst.append(val)

        data = []
        for _ in range(WARMUP):
            call_append(data, 0)
        data.clear()
        check_jit_compiled(call_append, "call_append")

        result = []
        call_append(result, 1)
        call_append(result, 2)
        call_append(result, 3)
        assert result == [1, 2, 3]

        call_append(result, "mixed")
        assert result == [1, 2, 3, "mixed"]

        call_append(result, None)
        assert result[-1] is None
        print("  PASS: test_list_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_append — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: list.insert()
    # ------------------------------------------------------------------
    try:
        def call_insert(lst, idx, val):
            lst.insert(idx, val)

        data = [1, 2, 3]
        for _ in range(WARMUP):
            call_insert(data, 0, 0)
            data.pop(0)
        check_jit_compiled(call_insert, "call_insert")

        result = [1, 2, 3]
        call_insert(result, 0, 0)
        assert result == [0, 1, 2, 3]

        call_insert(result, 2, 99)
        assert result == [0, 1, 99, 2, 3]

        call_insert(result, len(result), 100)
        assert result[-1] == 100
        print("  PASS: test_list_insert")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_insert — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: list.index()
    # ------------------------------------------------------------------
    try:
        def call_index(lst, val):
            return lst.index(val)

        data = [10, 20, 30, 40, 50]
        for _ in range(WARMUP):
            call_index(data, 30)
        check_jit_compiled(call_index, "call_index")

        assert call_index([10, 20, 30], 10) == 0
        assert call_index([10, 20, 30], 20) == 1
        assert call_index([10, 20, 30], 30) == 2

        # Duplicate values — returns first index
        assert call_index([1, 2, 1, 2], 2) == 1

        # ValueError for missing value
        got_error = False
        try:
            call_index([1, 2, 3], 99)
        except ValueError:
            got_error = True
        assert got_error, "Expected ValueError for missing value"
        print("  PASS: test_list_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: dict.get() with default
    # ------------------------------------------------------------------
    try:
        def call_dict_get(d, key, default):
            return d.get(key, default)

        d = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            call_dict_get(d, "a", None)
        check_jit_compiled(call_dict_get, "call_dict_get")

        assert call_dict_get({"a": 1}, "a", -1) == 1
        assert call_dict_get({"a": 1}, "b", -1) == -1
        assert call_dict_get({}, "x", "default") == "default"
        assert call_dict_get({"x": None}, "x", "fallback") is None
        assert call_dict_get({}, "missing", None) is None
        print("  PASS: test_dict_get_with_default")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_get_with_default — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: dict.pop() with default
    # ------------------------------------------------------------------
    try:
        def call_dict_pop(d, key, default):
            return d.pop(key, default)

        for _ in range(WARMUP):
            d = {"a": 1}
            call_dict_pop(d, "a", None)
        check_jit_compiled(call_dict_pop, "call_dict_pop")

        d = {"x": 10, "y": 20}
        assert call_dict_pop(d, "x", -1) == 10
        assert "x" not in d
        assert call_dict_pop(d, "missing", -1) == -1
        assert call_dict_pop(d, "y", -1) == 20
        assert d == {}
        print("  PASS: test_dict_pop_with_default")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_pop_with_default — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: str.encode()
    # ------------------------------------------------------------------
    try:
        def call_encode(s, encoding):
            return s.encode(encoding)

        for _ in range(WARMUP):
            call_encode("hello", "utf-8")
        check_jit_compiled(call_encode, "call_encode")

        assert call_encode("hello", "utf-8") == b"hello"
        assert call_encode("hello", "ascii") == b"hello"
        assert call_encode("", "utf-8") == b""
        assert call_encode("\u00e9", "utf-8") == b"\xc3\xa9"  # e-acute
        print("  PASS: test_str_encode")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_encode — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: bytes.decode()
    # ------------------------------------------------------------------
    try:
        def call_decode(b, encoding):
            return b.decode(encoding)

        for _ in range(WARMUP):
            call_decode(b"hello", "utf-8")
        check_jit_compiled(call_decode, "call_decode")

        assert call_decode(b"hello", "utf-8") == "hello"
        assert call_decode(b"hello", "ascii") == "hello"
        assert call_decode(b"", "utf-8") == ""
        assert call_decode(b"\xc3\xa9", "utf-8") == "\u00e9"
        print("  PASS: test_bytes_decode")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bytes_decode — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Deopt — different type with same method name
    # ------------------------------------------------------------------
    try:
        def call_count(obj, val):
            return obj.count(val)

        # Warm up on list
        data = [1, 2, 3, 2, 1]
        for _ in range(WARMUP):
            call_count(data, 2)
        check_jit_compiled(call_count, "call_count")

        # list.count()
        assert call_count([1, 2, 2, 3], 2) == 2
        assert call_count([1, 2, 3], 99) == 0

        # Deopt: tuple.count() — different type, same method name
        assert call_count((1, 2, 2, 3), 2) == 2
        assert call_count((1, 1, 1), 1) == 3

        # Deopt: str.count()
        assert call_count("hello", "l") == 2
        assert call_count("aaa", "a") == 3

        # Back to list
        assert call_count([5, 5, 5, 5], 5) == 4
        print("  PASS: test_deopt_different_type_same_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_type_same_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Deopt — subclass overriding method
    # ------------------------------------------------------------------
    try:
        class MyStr(str):
            def startswith(self, prefix):
                # Always returns True
                return True

        def call_sw(s, prefix):
            return s.startswith(prefix)

        for _ in range(WARMUP):
            call_sw("hello", "he")
        check_jit_compiled(call_sw, "call_sw")

        assert call_sw("hello", "he") is True
        assert call_sw("hello", "xx") is False

        # Deopt: subclass with overridden method
        ms = MyStr("hello")
        assert call_sw(ms, "xx") is True  # overridden, always True

        # Back to plain str
        assert call_sw("hello", "xx") is False
        print("  PASS: test_deopt_subclass_override")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_subclass_override — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Rapid method calls (1000 cycles)
    # ------------------------------------------------------------------
    try:
        def rapid_join(sep, parts):
            return sep.join(parts)

        for _ in range(WARMUP):
            rapid_join(",", ["a", "b"])
        check_jit_compiled(rapid_join, "rapid_join")

        for i in range(1000):
            result = rapid_join(",", ["x", "y", "z"])
            assert result == "x,y,z", f"cycle {i}: expected 'x,y,z', got '{result}'"
        print("  PASS: test_rapid_fast_method_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_fast_method_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Stability — 10000 calls
    # ------------------------------------------------------------------
    try:
        def stable_get(d, key, default):
            return d.get(key, default)

        d = {"key": 42}
        for _ in range(WARMUP):
            stable_get(d, "key", -1)
        check_jit_compiled(stable_get, "stable_get")

        for i in range(10000):
            result = stable_get(d, "key", -1)
            assert result == 42, f"iteration {i}: got {result}, expected 42"
        print("  PASS: test_stability_10000_fast_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_fast_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Method with multiple positional args
    # ------------------------------------------------------------------
    try:
        def call_replace_multi(s, old, new):
            return s.replace(old, new)

        for _ in range(WARMUP):
            call_replace_multi("aabbcc", "bb", "XX")
        check_jit_compiled(call_replace_multi, "call_replace_multi")

        # 2 positional args
        assert call_replace_multi("aabbcc", "bb", "XX") == "aaXXcc"

        # Multiple replacements
        assert call_replace_multi("abcabcabc", "abc", "X") == "XXX"

        # Empty old string — inserts between every character
        assert call_replace_multi("abc", "", "-") == "-a-b-c-"

        # No match
        assert call_replace_multi("hello", "xyz", "!") == "hello"

        # list.insert has 2 positional args too
        def call_insert_multi(lst, idx, val):
            lst.insert(idx, val)

        for _ in range(WARMUP):
            tmp = [1]
            call_insert_multi(tmp, 0, 0)
        check_jit_compiled(call_insert_multi, "call_insert_multi")

        r = [1, 2, 3]
        call_insert_multi(r, 1, 99)
        assert r == [1, 99, 2, 3]
        print("  PASS: test_multiple_positional_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_positional_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: str.count()
    # ------------------------------------------------------------------
    try:
        def call_str_count(s, sub):
            return s.count(sub)

        for _ in range(WARMUP):
            call_str_count("hello world", "l")
        check_jit_compiled(call_str_count, "call_str_count")

        assert call_str_count("hello world", "l") == 3
        assert call_str_count("hello world", "o") == 2
        assert call_str_count("hello world", "z") == 0
        assert call_str_count("aaa", "a") == 3
        assert call_str_count("aaa", "aa") == 1
        assert call_str_count("", "a") == 0
        assert call_str_count("hello", "") == 6  # len + 1 for empty substr
        print("  PASS: test_str_count")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_count — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: list.count()
    # ------------------------------------------------------------------
    try:
        def call_list_count(lst, val):
            return lst.count(val)

        for _ in range(WARMUP):
            call_list_count([1, 2, 3, 2, 1], 2)
        check_jit_compiled(call_list_count, "call_list_count")

        assert call_list_count([1, 2, 3, 2, 1], 2) == 2
        assert call_list_count([1, 2, 3, 2, 1], 1) == 2
        assert call_list_count([1, 2, 3], 99) == 0
        assert call_list_count([], 1) == 0
        assert call_list_count([None, None, None], None) == 3
        assert call_list_count(["a", "b", "a"], "a") == 2
        print("  PASS: test_list_count")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_count — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — obj.method(arg) vs type.method(obj, arg)
    # ------------------------------------------------------------------
    try:
        def via_instance(s, prefix):
            return s.startswith(prefix)

        def via_type(s, prefix):
            return str.startswith(s, prefix)

        for _ in range(WARMUP):
            via_instance("hello", "he")
            via_type("hello", "he")
        check_jit_compiled(via_instance, "via_instance")
        check_jit_compiled(via_type, "via_type")

        test_cases = [
            ("hello", "he", True),
            ("hello", "xx", False),
            ("hello", "hello", True),
            ("hello", "hello world", False),
            ("", "", True),
            ("hello", "", True),
        ]
        for s, prefix, expected in test_cases:
            r_inst = via_instance(s, prefix)
            r_type = via_type(s, prefix)
            assert r_inst == r_type == expected, (
                f"Mismatch for ('{s}', '{prefix}'): "
                f"instance={r_inst}, type={r_type}, expected={expected}"
            )

        # Same for list.index
        def list_via_instance(lst, val):
            return lst.index(val)

        def list_via_type(lst, val):
            return list.index(lst, val)

        for _ in range(WARMUP):
            list_via_instance([1, 2, 3], 2)
            list_via_type([1, 2, 3], 2)
        check_jit_compiled(list_via_instance, "list_via_instance")
        check_jit_compiled(list_via_type, "list_via_type")

        test_lists = [
            ([1, 2, 3], 1, 0),
            ([1, 2, 3], 2, 1),
            ([1, 2, 3], 3, 2),
            ([10, 20, 10], 10, 0),
        ]
        for lst, val, expected in test_lists:
            r_inst = list_via_instance(lst, val)
            r_type = list_via_type(lst, val)
            assert r_inst == r_type == expected, (
                f"Mismatch for ({lst}, {val}): "
                f"instance={r_inst}, type={r_type}, expected={expected}"
            )
        print("  PASS: test_equivalence_instance_vs_type_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_instance_vs_type_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_METHOD_DESCRIPTOR_FAST: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
