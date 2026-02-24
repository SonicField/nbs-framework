#!/usr/bin/env python3
"""
test_store_subscr_dict.py -- Correctness and deopt tests for
STORE_SUBSCR_DICT specialisation.

Targets: STORE_SUBSCR_DICT.

STORE_SUBSCR_DICT specialises the subscript store operation
(dict_obj[key] = value) when the container is a dict. Instead of going
through generic PyObject_SetItem -> mp_ass_subscript dispatch, it uses
PyDict_SetItem directly.

The JIT specialisation emits GuardType(TDictExact) on the container,
allowing the Simplify pass to use a direct dict store without dispatch
overhead.

Deopt triggers:
  - Container is not a dict (e.g. list, custom __setitem__)
  - Dict subclass

Tests cover:
  1.  Basic dict store with string key
  2.  Integer key store
  3.  Tuple key store (hashable)
  4.  Overwrite existing key
  5.  Store None as key
  6.  Store None as value
  7.  Store different value types (int, str, list, dict, None)
  8.  Store preserves dict identity (same object)
  9.  Deopt: dict-compiled -> list store
  10. Deopt: dict-compiled -> custom __setitem__
  11. Deopt: dict-compiled -> bytearray store
  12. Dict mutation via store in loop (build dict from range)
  13. Multiple stores in one function
  14. Store after del (key deleted then re-added)
  15. Rapid container type alternation (dict vs list, 10 cycles)
  16. Nested dict store: d[k1][k2] = val
  17. Overwrite same key 100 times
  18. Large dict store (10000 keys)
  19. Bool key (True/False as dict keys, hash collision with ints)
  20. Dict comprehension equivalence vs manual store loop

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_store_subscr_dict.py
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
    print("=== STORE_SUBSCR_DICT Correctness & Deopt Tests ===")
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

    # ------------------------------------------------------------------
    # Test 1: Basic dict store with string key
    # ------------------------------------------------------------------
    try:
        def store_str_key_1(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_str_key_1(warmup_d, "x", 1)

        check_jit_compiled(store_str_key_1, "store_str_key_1")

        d = {}
        store_str_key_1(d, "hello", 42)
        assert d == {"hello": 42}, f"Expected {{'hello': 42}}, got {d}"
        store_str_key_1(d, "world", 99)
        assert d == {"hello": 42, "world": 99}, f"Unexpected dict: {d}"
        print("  PASS: test_basic_string_key_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_string_key_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Integer key store
    # ------------------------------------------------------------------
    try:
        def store_int_key_2(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_int_key_2(warmup_d, 0, "a")

        check_jit_compiled(store_int_key_2, "store_int_key_2")

        d = {}
        store_int_key_2(d, 0, "zero")
        store_int_key_2(d, 1, "one")
        store_int_key_2(d, -1, "neg_one")
        assert d == {0: "zero", 1: "one", -1: "neg_one"}, f"Unexpected dict: {d}"
        print("  PASS: test_integer_key_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_integer_key_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Tuple key store (hashable)
    # ------------------------------------------------------------------
    try:
        def store_tuple_key_3(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_tuple_key_3(warmup_d, (0, 0), "origin")

        check_jit_compiled(store_tuple_key_3, "store_tuple_key_3")

        d = {}
        store_tuple_key_3(d, (1, 2), "point_a")
        store_tuple_key_3(d, (3, 4), "point_b")
        store_tuple_key_3(d, (), "empty_tuple")
        assert d == {(1, 2): "point_a", (3, 4): "point_b", (): "empty_tuple"}, \
            f"Unexpected dict: {d}"
        print("  PASS: test_tuple_key_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_tuple_key_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Overwrite existing key
    # ------------------------------------------------------------------
    try:
        def store_overwrite_4(d, k, v):
            d[k] = v

        warmup_d = {"x": 0}
        for _ in range(WARMUP):
            store_overwrite_4(warmup_d, "x", 1)

        check_jit_compiled(store_overwrite_4, "store_overwrite_4")

        d = {"key": "original"}
        store_overwrite_4(d, "key", "updated")
        assert d["key"] == "updated", f"Expected 'updated', got {d['key']}"
        assert len(d) == 1, f"Expected length 1 after overwrite, got {len(d)}"
        store_overwrite_4(d, "key", 42)
        assert d["key"] == 42, f"Expected 42, got {d['key']}"
        assert len(d) == 1, f"Expected length 1 after second overwrite, got {len(d)}"
        print("  PASS: test_overwrite_existing_key")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overwrite_existing_key — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Store None as key
    # ------------------------------------------------------------------
    try:
        def store_none_key_5(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_none_key_5(warmup_d, None, 0)

        check_jit_compiled(store_none_key_5, "store_none_key_5")

        d = {}
        store_none_key_5(d, None, "none_val")
        assert d == {None: "none_val"}, f"Unexpected dict: {d}"
        assert d[None] == "none_val"
        # Overwrite None key
        store_none_key_5(d, None, "replaced")
        assert d[None] == "replaced"
        assert len(d) == 1
        print("  PASS: test_store_none_as_key")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_none_as_key — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Store None as value
    # ------------------------------------------------------------------
    try:
        def store_none_val_6(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_none_val_6(warmup_d, "x", None)

        check_jit_compiled(store_none_val_6, "store_none_val_6")

        d = {"a": 1, "b": 2}
        store_none_val_6(d, "a", None)
        assert d["a"] is None, f"Expected None, got {d['a']}"
        store_none_val_6(d, "c", None)
        assert d["c"] is None
        assert len(d) == 3
        # Verify the key is present even though value is None
        assert "a" in d
        assert "c" in d
        print("  PASS: test_store_none_as_value")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_none_as_value — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Store different value types (int, str, list, dict, None)
    # ------------------------------------------------------------------
    try:
        def store_varied_7(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_varied_7(warmup_d, "w", 0)

        check_jit_compiled(store_varied_7, "store_varied_7")

        d = {}
        store_varied_7(d, "int", 42)
        store_varied_7(d, "str", "hello")
        store_varied_7(d, "list", [1, 2, 3])
        store_varied_7(d, "dict", {"nested": True})
        store_varied_7(d, "none", None)

        assert d["int"] == 42
        assert d["str"] == "hello"
        assert d["list"] == [1, 2, 3]
        assert d["dict"] == {"nested": True}
        assert d["none"] is None
        assert len(d) == 5
        print("  PASS: test_store_different_value_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_different_value_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Store preserves dict identity (same object)
    # ------------------------------------------------------------------
    try:
        def store_identity_8(d, k, v):
            d[k] = v
            return d

        warmup_d = {}
        for _ in range(WARMUP):
            store_identity_8(warmup_d, "x", 1)

        check_jit_compiled(store_identity_8, "store_identity_8")

        original = {"a": 1}
        original_id = id(original)
        result = store_identity_8(original, "b", 2)
        assert result is original, "Returned dict is not the same object"
        assert id(original) == original_id, "Dict id changed after store"
        assert original == {"a": 1, "b": 2}
        print("  PASS: test_store_preserves_dict_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_preserves_dict_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt: dict-compiled -> list store
    # ------------------------------------------------------------------
    try:
        def store_deopt_list_9(container, key, val):
            container[key] = val

        warmup_d = {}
        for _ in range(WARMUP):
            store_deopt_list_9(warmup_d, "k", 1)

        check_jit_compiled(store_deopt_list_9, "store_deopt_list_9")

        # Dict still works after warmup
        d = {"a": 1}
        store_deopt_list_9(d, "b", 2)
        assert d == {"a": 1, "b": 2}, f"Dict store failed: {d}"

        # Deopt to list with integer index
        lst = [10, 20, 30]
        store_deopt_list_9(lst, 0, 99)
        assert lst == [99, 20, 30], f"List store failed: {lst}"

        # Dict still works after deopt
        d2 = {}
        store_deopt_list_9(d2, "x", 42)
        assert d2 == {"x": 42}, f"Dict store after deopt failed: {d2}"

        print("  PASS: test_deopt_dict_to_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_to_list — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt: dict-compiled -> custom __setitem__
    # ------------------------------------------------------------------
    try:
        class TrackedStore:
            """Custom container that records all stores."""
            def __init__(self):
                self.data = {}
                self.store_count = 0

            def __setitem__(self, key, val):
                self.data[key] = val
                self.store_count += 1

        def store_deopt_custom_10(container, key, val):
            container[key] = val

        warmup_d = {}
        for _ in range(WARMUP):
            store_deopt_custom_10(warmup_d, "k", 1)

        check_jit_compiled(store_deopt_custom_10, "store_deopt_custom_10")

        # Dict works
        d = {}
        store_deopt_custom_10(d, "a", 10)
        assert d == {"a": 10}

        # Deopt to custom __setitem__
        cs = TrackedStore()
        store_deopt_custom_10(cs, "x", 42)
        store_deopt_custom_10(cs, "y", 99)
        assert cs.data == {"x": 42, "y": 99}, f"Custom store data: {cs.data}"
        assert cs.store_count == 2, f"Expected 2 stores, got {cs.store_count}"

        # Dict still works after deopt
        d2 = {"p": 1}
        store_deopt_custom_10(d2, "q", 2)
        assert d2 == {"p": 1, "q": 2}

        print("  PASS: test_deopt_dict_to_custom_setitem")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_to_custom_setitem — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt: dict-compiled -> bytearray store
    # ------------------------------------------------------------------
    try:
        def store_deopt_ba_11(container, key, val):
            container[key] = val

        warmup_d = {}
        for _ in range(WARMUP):
            store_deopt_ba_11(warmup_d, "k", 1)

        check_jit_compiled(store_deopt_ba_11, "store_deopt_ba_11")

        # Dict works
        d = {}
        store_deopt_ba_11(d, "a", 10)
        assert d == {"a": 10}

        # Deopt to bytearray (indexed by int, value must be 0-255)
        ba = bytearray(b"\x00\x01\x02\x03")
        store_deopt_ba_11(ba, 0, 65)
        store_deopt_ba_11(ba, 3, 90)
        assert ba[0] == 65, f"Expected 65 at index 0, got {ba[0]}"
        assert ba[3] == 90, f"Expected 90 at index 3, got {ba[3]}"
        assert ba == bytearray(b"A\x01\x02Z")

        # Dict still works after deopt
        d2 = {}
        store_deopt_ba_11(d2, "z", 999)
        assert d2 == {"z": 999}

        print("  PASS: test_deopt_dict_to_bytearray")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_to_bytearray — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Dict mutation via store in loop (build dict from range)
    # ------------------------------------------------------------------
    try:
        def build_dict_12(n):
            d = {}
            for i in range(n):
                d[i] = i * i
            return d

        for _ in range(WARMUP):
            build_dict_12(5)

        check_jit_compiled(build_dict_12, "build_dict_12")

        result = build_dict_12(10)
        expected = {i: i * i for i in range(10)}
        assert result == expected, f"Expected {expected}, got {result}"

        result_empty = build_dict_12(0)
        assert result_empty == {}, f"Expected empty dict, got {result_empty}"

        result_one = build_dict_12(1)
        assert result_one == {0: 0}, f"Expected {{0: 0}}, got {result_one}"

        print("  PASS: test_dict_store_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_store_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Multiple stores in one function
    # ------------------------------------------------------------------
    try:
        def multi_store_13(d, a, b, c):
            d["a"] = a
            d["b"] = b
            d["c"] = c

        warmup_d = {}
        for _ in range(WARMUP):
            multi_store_13(warmup_d, 1, 2, 3)

        check_jit_compiled(multi_store_13, "multi_store_13")

        d = {}
        multi_store_13(d, 10, 20, 30)
        assert d == {"a": 10, "b": 20, "c": 30}, f"Unexpected dict: {d}"

        # Overwrite with different values
        multi_store_13(d, "x", "y", "z")
        assert d == {"a": "x", "b": "y", "c": "z"}, f"Overwrite failed: {d}"

        print("  PASS: test_multiple_stores_one_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_stores_one_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Store after del (key deleted then re-added)
    # ------------------------------------------------------------------
    try:
        def store_after_del_14(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_after_del_14(warmup_d, "x", 1)

        check_jit_compiled(store_after_del_14, "store_after_del_14")

        d = {"key": "original", "other": "keep"}
        del d["key"]
        assert "key" not in d, "Key should be deleted"

        store_after_del_14(d, "key", "re-added")
        assert d["key"] == "re-added", f"Expected 're-added', got {d['key']}"
        assert d["other"] == "keep", "Other key should be untouched"
        assert len(d) == 2

        # Delete and re-add multiple times
        for i in range(10):
            del d["key"]
            store_after_del_14(d, "key", i)
            assert d["key"] == i, f"Re-add iteration {i} failed"

        print("  PASS: test_store_after_del")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_after_del — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Rapid container type alternation (dict vs list, 10 cycles)
    # ------------------------------------------------------------------
    try:
        def store_rapid_15(container, key, val):
            container[key] = val

        warmup_d = {}
        for _ in range(WARMUP):
            store_rapid_15(warmup_d, "k", 1)

        check_jit_compiled(store_rapid_15, "store_rapid_15")

        for cycle in range(10):
            # Dict store
            d = {}
            store_rapid_15(d, "x", cycle)
            assert d["x"] == cycle, f"cycle {cycle}: dict store failed"

            # List store (deopt)
            lst = [0, 0, 0]
            store_rapid_15(lst, 1, cycle * 10)
            assert lst[1] == cycle * 10, f"cycle {cycle}: list store failed"

        # Final dict store to confirm dict path still works
        final_d = {}
        store_rapid_15(final_d, "final", 999)
        assert final_d == {"final": 999}

        print("  PASS: test_rapid_container_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_container_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Nested dict store: d[k1][k2] = val
    # ------------------------------------------------------------------
    try:
        def nested_store_16(d, k1, k2, val):
            d[k1][k2] = val

        warmup_d = {"outer": {"inner": 0}}
        for _ in range(WARMUP):
            nested_store_16(warmup_d, "outer", "inner", 1)

        check_jit_compiled(nested_store_16, "nested_store_16")

        d = {
            "server": {"host": "localhost", "port": 8080},
            "db": {"host": "db.local", "port": 5432},
        }
        nested_store_16(d, "server", "port", 9090)
        assert d["server"]["port"] == 9090, f"Expected 9090, got {d['server']['port']}"
        assert d["server"]["host"] == "localhost", "Host should be untouched"

        nested_store_16(d, "db", "host", "newdb.local")
        assert d["db"]["host"] == "newdb.local"
        assert d["db"]["port"] == 5432, "DB port should be untouched"

        # Add new key to nested dict
        nested_store_16(d, "server", "ssl", True)
        assert d["server"]["ssl"] is True

        print("  PASS: test_nested_dict_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_dict_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Overwrite same key 100 times
    # ------------------------------------------------------------------
    try:
        def overwrite_17(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            overwrite_17(warmup_d, "x", 0)

        check_jit_compiled(overwrite_17, "overwrite_17")

        d = {}
        for i in range(100):
            overwrite_17(d, "key", i)
        assert d["key"] == 99, f"Expected 99 after 100 overwrites, got {d['key']}"
        assert len(d) == 1, f"Expected length 1, got {len(d)}"

        print("  PASS: test_overwrite_same_key_100_times")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overwrite_same_key_100_times — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Large dict store (10000 keys)
    # ------------------------------------------------------------------
    try:
        def store_large_18(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_large_18(warmup_d, "x", 0)

        check_jit_compiled(store_large_18, "store_large_18")

        d = {}
        for i in range(10000):
            store_large_18(d, f"key_{i}", i)

        assert len(d) == 10000, f"Expected 10000 keys, got {len(d)}"
        assert d["key_0"] == 0, f"First key: expected 0, got {d['key_0']}"
        assert d["key_9999"] == 9999, f"Last key: expected 9999, got {d['key_9999']}"
        assert d["key_5000"] == 5000, f"Mid key: expected 5000, got {d['key_5000']}"

        # Verify no spurious keys
        for i in range(0, 10000, 1000):
            assert d[f"key_{i}"] == i, f"Spot check failed at key_{i}"

        print("  PASS: test_large_dict_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_dict_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Bool key (True/False as dict keys, hash collision with ints)
    # ------------------------------------------------------------------
    try:
        def store_bool_key_19(d, k, v):
            d[k] = v

        warmup_d = {}
        for _ in range(WARMUP):
            store_bool_key_19(warmup_d, True, 0)

        check_jit_compiled(store_bool_key_19, "store_bool_key_19")

        # In Python, True == 1 and False == 0, and they hash the same.
        # Storing True then 1 overwrites; the key stays as whichever was
        # inserted first.
        d = {}
        store_bool_key_19(d, True, "bool_true")
        assert d[True] == "bool_true"
        assert d[1] == "bool_true", "True and 1 should share the same slot"

        store_bool_key_19(d, False, "bool_false")
        assert d[False] == "bool_false"
        assert d[0] == "bool_false", "False and 0 should share the same slot"

        # Overwrite via int key — should update the same slot
        store_bool_key_19(d, 1, "int_one")
        assert d[True] == "int_one", "Overwrite via 1 should affect True's slot"
        assert d[1] == "int_one"

        store_bool_key_19(d, 0, "int_zero")
        assert d[False] == "int_zero", "Overwrite via 0 should affect False's slot"
        assert d[0] == "int_zero"

        assert len(d) == 2, f"Expected 2 entries (True/1 and False/0), got {len(d)}"

        print("  PASS: test_bool_key_hash_collision")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bool_key_hash_collision — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Dict comprehension equivalence vs manual store loop
    # ------------------------------------------------------------------
    try:
        def manual_store_20(pairs):
            d = {}
            for k, v in pairs:
                d[k] = v
            return d

        pairs = [(f"k{i}", i * 3) for i in range(20)]

        for _ in range(WARMUP):
            manual_store_20(pairs)

        check_jit_compiled(manual_store_20, "manual_store_20")

        manual_result = manual_store_20(pairs)
        comp_result = {k: v for k, v in pairs}

        assert manual_result == comp_result, (
            f"Manual store and comprehension differ: "
            f"manual={manual_result}, comp={comp_result}"
        )

        # Verify both have the same keys and values
        assert set(manual_result.keys()) == set(comp_result.keys())
        for k in manual_result:
            assert manual_result[k] == comp_result[k], (
                f"Value mismatch at key {k!r}: "
                f"manual={manual_result[k]}, comp={comp_result[k]}"
            )

        # Edge case: empty pairs
        assert manual_store_20([]) == {}

        # Edge case: duplicate keys — last value wins
        dup_pairs = [("a", 1), ("b", 2), ("a", 3)]
        manual_dup = manual_store_20(dup_pairs)
        comp_dup = {k: v for k, v in dup_pairs}
        assert manual_dup == comp_dup, (
            f"Duplicate key handling differs: manual={manual_dup}, comp={comp_dup}"
        )
        assert manual_dup["a"] == 3, "Last value for duplicate key should win"

        print("  PASS: test_dict_comprehension_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_comprehension_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print()
    print(f"STORE_SUBSCR_DICT: {passed}/{passed + failed} passed, {failed}/{passed + failed} failed")
    if failed > 0:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
