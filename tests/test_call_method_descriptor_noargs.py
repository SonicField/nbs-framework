#!/usr/bin/env python3
"""
test_call_method_descriptor_noargs.py — Correctness and deopt tests for
CALL_METHOD_DESCRIPTOR_NOARGS specialisation.

Targets: CALL_METHOD_DESCRIPTOR_NOARGS.

CALL_METHOD_DESCRIPTOR_NOARGS specialises calls to C-implemented method
descriptors that take no arguments (only self). CPython's adaptive
interpreter replaces the generic CALL opcode with this specialisation when
it detects repeated calls to methods like list.copy(), dict.keys(),
str.upper(), etc.

These are PyMethodDescrObject descriptors pointing to C functions with
METH_NOARGS calling convention. The JIT can emit a direct C function call
instead of going through the generic vectorcall protocol.

Mechanism:
1. Adaptive interpreter detects CALL to a method_descriptor with METH_NOARGS
2. Replaces CALL with CALL_METHOD_DESCRIPTOR_NOARGS
3. CinderX JIT emits GuardType on the receiver + direct C function pointer call
4. Skips argument parsing, tuple packing, and generic dispatch

Deopt triggers:
  - Receiver type changes (different type with same method name)
  - Method is overridden on the instance or subclass
  - Method descriptor replaced on the type

Tests cover:
  - list.copy()
  - list.clear() — mutating noargs method
  - dict.keys()
  - dict.values()
  - dict.items()
  - dict.copy()
  - str.upper()
  - str.lower()
  - str.strip() with no args
  - str.title()
  - bytes.upper()
  - set.copy()
  - Deopt: different type with same method name
  - Deopt: subclass overriding method
  - Rapid noargs method calls (1000 cycles)
  - Stability — 10000 calls
  - Method on freshly created objects
  - Chained noargs methods (e.g. str.upper().lower())
  - frozenset.copy()
  - Equivalence: obj.method() vs type.method(obj)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_method_descriptor_noargs.py
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
    # Test 1: list.copy()
    # ------------------------------------------------------------------
    try:
        def call_list_copy(obj):
            return obj.copy()

        data = [1, 2, 3]
        for _ in range(WARMUP):
            call_list_copy(data)
        check_jit_compiled(call_list_copy, "call_list_copy")

        result = call_list_copy([1, 2, 3])
        assert result == [1, 2, 3]
        # Verify it is a copy, not the same object
        original = [4, 5, 6]
        copy = call_list_copy(original)
        assert copy == original
        assert copy is not original
        copy.append(7)
        assert len(original) == 3  # original unmodified
        print("  PASS: test_list_copy")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_copy — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: list.clear()
    # ------------------------------------------------------------------
    try:
        def call_list_clear(obj):
            obj.clear()

        data = [1, 2, 3]
        for _ in range(WARMUP):
            data.extend([1, 2, 3])
            call_list_clear(data)
        check_jit_compiled(call_list_clear, "call_list_clear")

        test_list = [10, 20, 30, 40]
        call_list_clear(test_list)
        assert test_list == []
        assert len(test_list) == 0

        # Clear already empty list
        call_list_clear(test_list)
        assert test_list == []
        print("  PASS: test_list_clear")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_clear — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: dict.keys()
    # ------------------------------------------------------------------
    try:
        def call_dict_keys(obj):
            return obj.keys()

        d = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            call_dict_keys(d)
        check_jit_compiled(call_dict_keys, "call_dict_keys")

        keys = call_dict_keys({"x": 10, "y": 20, "z": 30})
        assert set(keys) == {"x", "y", "z"}

        keys_empty = call_dict_keys({})
        assert len(keys_empty) == 0
        print("  PASS: test_dict_keys")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_keys — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: dict.values()
    # ------------------------------------------------------------------
    try:
        def call_dict_values(obj):
            return obj.values()

        d = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            call_dict_values(d)
        check_jit_compiled(call_dict_values, "call_dict_values")

        vals = call_dict_values({"a": 10, "b": 20})
        assert sorted(vals) == [10, 20]

        vals_empty = call_dict_values({})
        assert len(vals_empty) == 0
        print("  PASS: test_dict_values")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_values — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: dict.items()
    # ------------------------------------------------------------------
    try:
        def call_dict_items(obj):
            return obj.items()

        d = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            call_dict_items(d)
        check_jit_compiled(call_dict_items, "call_dict_items")

        items = call_dict_items({"x": 1, "y": 2})
        assert set(items) == {("x", 1), ("y", 2)}

        items_empty = call_dict_items({})
        assert len(items_empty) == 0
        print("  PASS: test_dict_items")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_items — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: dict.copy()
    # ------------------------------------------------------------------
    try:
        def call_dict_copy(obj):
            return obj.copy()

        d = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            call_dict_copy(d)
        check_jit_compiled(call_dict_copy, "call_dict_copy")

        original = {"x": 10, "y": [1, 2]}
        copy = call_dict_copy(original)
        assert copy == original
        assert copy is not original
        copy["z"] = 30
        assert "z" not in original
        print("  PASS: test_dict_copy")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_copy — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: str.upper()
    # ------------------------------------------------------------------
    try:
        def call_str_upper(obj):
            return obj.upper()

        for _ in range(WARMUP):
            call_str_upper("hello")
        check_jit_compiled(call_str_upper, "call_str_upper")

        assert call_str_upper("hello") == "HELLO"
        assert call_str_upper("Hello World") == "HELLO WORLD"
        assert call_str_upper("ALREADY") == "ALREADY"
        assert call_str_upper("") == ""
        assert call_str_upper("123abc") == "123ABC"
        print("  PASS: test_str_upper")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_upper — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: str.lower()
    # ------------------------------------------------------------------
    try:
        def call_str_lower(obj):
            return obj.lower()

        for _ in range(WARMUP):
            call_str_lower("HELLO")
        check_jit_compiled(call_str_lower, "call_str_lower")

        assert call_str_lower("HELLO") == "hello"
        assert call_str_lower("Hello World") == "hello world"
        assert call_str_lower("already") == "already"
        assert call_str_lower("") == ""
        assert call_str_lower("123ABC") == "123abc"
        print("  PASS: test_str_lower")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_lower — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: str.strip() with no args
    # ------------------------------------------------------------------
    try:
        def call_str_strip(obj):
            return obj.strip()

        for _ in range(WARMUP):
            call_str_strip("  hello  ")
        check_jit_compiled(call_str_strip, "call_str_strip")

        assert call_str_strip("  hello  ") == "hello"
        assert call_str_strip("\t\n hello \n\t") == "hello"
        assert call_str_strip("hello") == "hello"
        assert call_str_strip("") == ""
        assert call_str_strip("   ") == ""
        print("  PASS: test_str_strip_noargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_strip_noargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: str.title()
    # ------------------------------------------------------------------
    try:
        def call_str_title(obj):
            return obj.title()

        for _ in range(WARMUP):
            call_str_title("hello world")
        check_jit_compiled(call_str_title, "call_str_title")

        assert call_str_title("hello world") == "Hello World"
        assert call_str_title("HELLO WORLD") == "Hello World"
        assert call_str_title("hello") == "Hello"
        assert call_str_title("") == ""
        assert call_str_title("a b c") == "A B C"
        print("  PASS: test_str_title")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_str_title — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: bytes.upper()
    # ------------------------------------------------------------------
    try:
        def call_bytes_upper(obj):
            return obj.upper()

        for _ in range(WARMUP):
            call_bytes_upper(b"hello")
        check_jit_compiled(call_bytes_upper, "call_bytes_upper")

        assert call_bytes_upper(b"hello") == b"HELLO"
        assert call_bytes_upper(b"ALREADY") == b"ALREADY"
        assert call_bytes_upper(b"") == b""
        assert call_bytes_upper(b"abc123") == b"ABC123"
        print("  PASS: test_bytes_upper")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bytes_upper — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: set.copy()
    # ------------------------------------------------------------------
    try:
        def call_set_copy(obj):
            return obj.copy()

        s = {1, 2, 3}
        for _ in range(WARMUP):
            call_set_copy(s)
        check_jit_compiled(call_set_copy, "call_set_copy")

        original = {10, 20, 30}
        copy = call_set_copy(original)
        assert copy == original
        assert copy is not original
        copy.add(40)
        assert 40 not in original

        assert call_set_copy(set()) == set()
        print("  PASS: test_set_copy")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_set_copy — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Deopt — different type with same method name
    # ------------------------------------------------------------------
    try:
        def call_copy(obj):
            return obj.copy()

        # Warm up on list
        data = [1, 2, 3]
        for _ in range(WARMUP):
            call_copy(data)
        check_jit_compiled(call_copy, "call_copy")

        # list.copy()
        assert call_copy([1, 2]) == [1, 2]

        # Deopt: dict.copy() — different type, same method name
        assert call_copy({"a": 1}) == {"a": 1}

        # Deopt: set.copy()
        assert call_copy({1, 2, 3}) == {1, 2, 3}

        # Back to list
        assert call_copy([4, 5, 6]) == [4, 5, 6]
        print("  PASS: test_deopt_different_type_same_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_type_same_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Deopt — subclass overriding method
    # ------------------------------------------------------------------
    try:
        class MyList(list):
            def copy(self):
                return ["overridden"]

        def call_copy_sub(obj):
            return obj.copy()

        # Warm up on plain list
        plain = [1, 2, 3]
        for _ in range(WARMUP):
            call_copy_sub(plain)
        check_jit_compiled(call_copy_sub, "call_copy_sub")

        assert call_copy_sub([1, 2, 3]) == [1, 2, 3]

        # Deopt: subclass with overridden method
        ml = MyList([1, 2, 3])
        result = call_copy_sub(ml)
        assert result == ["overridden"], f"Expected ['overridden'], got {result}"

        # Back to plain list still works
        assert call_copy_sub([4, 5]) == [4, 5]
        print("  PASS: test_deopt_subclass_override")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_subclass_override — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Rapid noargs method calls (1000 cycles)
    # ------------------------------------------------------------------
    try:
        def rapid_upper(obj):
            return obj.upper()

        for _ in range(WARMUP):
            rapid_upper("test")
        check_jit_compiled(rapid_upper, "rapid_upper")

        for i in range(1000):
            result = rapid_upper("hello")
            assert result == "HELLO", f"cycle {i}: expected 'HELLO', got '{result}'"
        print("  PASS: test_rapid_noargs_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_noargs_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Stability — 10000 calls
    # ------------------------------------------------------------------
    try:
        def stable_keys(obj):
            return obj.keys()

        d = {"a": 1, "b": 2, "c": 3}
        for _ in range(WARMUP):
            stable_keys(d)
        check_jit_compiled(stable_keys, "stable_keys")

        for i in range(10000):
            keys = stable_keys(d)
            assert set(keys) == {"a", "b", "c"}, f"iteration {i}: keys mismatch"
        print("  PASS: test_stability_10000_noargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_noargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Method on freshly created objects
    # ------------------------------------------------------------------
    try:
        def copy_fresh_list():
            return [1, 2, 3].copy()

        for _ in range(WARMUP):
            copy_fresh_list()
        check_jit_compiled(copy_fresh_list, "copy_fresh_list")

        for i in range(100):
            result = copy_fresh_list()
            assert result == [1, 2, 3], f"cycle {i}: got {result}"

        def upper_fresh_str():
            return "hello".upper()

        for _ in range(WARMUP):
            upper_fresh_str()
        check_jit_compiled(upper_fresh_str, "upper_fresh_str")

        assert upper_fresh_str() == "HELLO"
        print("  PASS: test_method_on_fresh_objects")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_on_fresh_objects — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained noargs methods
    # ------------------------------------------------------------------
    try:
        def chain_upper_lower(obj):
            return obj.upper().lower()

        def chain_strip_upper(obj):
            return obj.strip().upper()

        for _ in range(WARMUP):
            chain_upper_lower("Hello")
            chain_strip_upper("  hello  ")
        check_jit_compiled(chain_upper_lower, "chain_upper_lower")
        check_jit_compiled(chain_strip_upper, "chain_strip_upper")

        assert chain_upper_lower("Hello") == "hello"
        assert chain_upper_lower("ABC") == "abc"
        assert chain_upper_lower("") == ""

        assert chain_strip_upper("  hello  ") == "HELLO"
        assert chain_strip_upper("\n test \t") == "TEST"
        assert chain_strip_upper("no_space") == "NO_SPACE"
        print("  PASS: test_chained_noargs_methods")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_noargs_methods — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: frozenset.copy()
    # ------------------------------------------------------------------
    try:
        def call_frozenset_copy(obj):
            return obj.copy()

        fs = frozenset({1, 2, 3})
        for _ in range(WARMUP):
            call_frozenset_copy(fs)
        check_jit_compiled(call_frozenset_copy, "call_frozenset_copy")

        original = frozenset({10, 20, 30})
        copy = call_frozenset_copy(original)
        assert copy == original
        # frozenset.copy() may return the same object (immutable optimisation)
        assert copy == frozenset({10, 20, 30})

        assert call_frozenset_copy(frozenset()) == frozenset()
        print("  PASS: test_frozenset_copy")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_frozenset_copy — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — obj.method() vs type.method(obj)
    # ------------------------------------------------------------------
    try:
        def via_instance(obj):
            return obj.upper()

        def via_type(obj):
            return str.upper(obj)

        for _ in range(WARMUP):
            via_instance("hello")
            via_type("hello")
        check_jit_compiled(via_instance, "via_instance")
        check_jit_compiled(via_type, "via_type")

        test_strings = ["hello", "HELLO", "Hello World", "", "123abc", " spaces "]
        for s in test_strings:
            r_instance = via_instance(s)
            r_type = via_type(s)
            assert r_instance == r_type, (
                f"Mismatch for '{s}': instance={r_instance}, type={r_type}"
            )

        # Same for list.copy
        def list_via_instance(obj):
            return obj.copy()

        def list_via_type(obj):
            return list.copy(obj)

        for _ in range(WARMUP):
            list_via_instance([1, 2])
            list_via_type([1, 2])
        check_jit_compiled(list_via_instance, "list_via_instance")
        check_jit_compiled(list_via_type, "list_via_type")

        test_lists = [[], [1], [1, 2, 3], list(range(10))]
        for lst in test_lists:
            r_instance = list_via_instance(lst)
            r_type = list_via_type(lst)
            assert r_instance == r_type, (
                f"Mismatch for {lst}: instance={r_instance}, type={r_type}"
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
    print(f"\nCALL_METHOD_DESCRIPTOR_NOARGS: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
