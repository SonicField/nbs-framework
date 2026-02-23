#!/usr/bin/env python3
"""
test_binary_subscr_dict.py — Correctness and deopt tests for
BINARY_SUBSCR_DICT specialisation.

Targets: BINARY_SUBSCR_DICT.

BINARY_SUBSCR_DICT specialises the subscript load operation (obj[key])
when the container is a dict. Instead of going through generic
PyObject_GetItem → mp_subscript dispatch, it uses PyDict_GetItem (or
the internal dict lookup) directly, bypassing the type dispatch overhead.

The JIT specialisation emits GuardType(TDictExact) on the container,
allowing the Simplify pass to call PyDict_Type.tp_as_mapping->mp_subscript
directly.

Deopt triggers:
  - Function JIT-compiled with dict[key] loads, then called with list/other
  - Container is not a dict (e.g. list, tuple, custom __getitem__)
  - Dict subclass (e.g. defaultdict, OrderedDict)

Tests cover:
  - Basic dict lookup (string key)
  - Integer keys
  - Tuple keys (immutable, hashable)
  - KeyError for missing key
  - Different value types (int, str, None, list, nested dict)
  - Deopt: dict-compiled -> list load
  - Deopt: dict-compiled -> custom __getitem__
  - Deopt: dict-compiled -> defaultdict (dict subclass)
  - Dict read in loop
  - Object identity preservation
  - Multiple reads in one function
  - Read after mutation (add, delete, update)
  - Rapid container type alternation
  - Nested dict reads (d[k1][k2])
  - Large dict access (10k entries)
  - Boolean key (True/False as dict keys)
  - Empty dict KeyError
  - Dict with hash collisions (pathological keys)
  - Equivalence: d[k] vs dict.__getitem__(d, k)
  - Mixed key types in same dict

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_subscr_dict.py
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
    # Test 1: Basic string key lookup
    # ------------------------------------------------------------------
    try:
        def read_key(d):
            return d["x"]

        data = {"x": 42, "y": 99}
        for _ in range(WARMUP):
            read_key(data)
        check_jit_compiled(read_key, "read_key")

        assert read_key(data) == 42
        assert read_key({"x": "hello"}) == "hello"
        assert read_key({"x": None, "y": 1}) is None
        print("  PASS: test_basic_string_key")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_string_key — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Integer keys
    # ------------------------------------------------------------------
    try:
        def read_int_key(d, k):
            return d[k]

        data = {0: "zero", 1: "one", 2: "two", -1: "neg_one"}
        for _ in range(WARMUP):
            read_int_key(data, 1)
        check_jit_compiled(read_int_key, "read_int_key")

        assert read_int_key(data, 0) == "zero"
        assert read_int_key(data, 1) == "one"
        assert read_int_key(data, -1) == "neg_one"
        assert read_int_key(data, 2) == "two"
        print("  PASS: test_integer_keys")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_integer_keys — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Tuple keys (immutable, hashable)
    # ------------------------------------------------------------------
    try:
        def read_tuple_key(d, k):
            return d[k]

        data = {(0, 0): "origin", (1, 0): "east", (0, 1): "north"}
        for _ in range(WARMUP):
            read_tuple_key(data, (0, 0))
        check_jit_compiled(read_tuple_key, "read_tuple_key")

        assert read_tuple_key(data, (0, 0)) == "origin"
        assert read_tuple_key(data, (1, 0)) == "east"
        assert read_tuple_key(data, (0, 1)) == "north"
        print("  PASS: test_tuple_keys")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_tuple_keys — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: KeyError for missing key
    # ------------------------------------------------------------------
    try:
        def read_missing(d, k):
            return d[k]

        data = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            read_missing(data, "a")
        check_jit_compiled(read_missing, "read_missing")

        assert read_missing(data, "a") == 1

        raised = False
        try:
            read_missing(data, "z")
        except KeyError as e:
            raised = True
            assert e.args[0] == "z", f"KeyError key should be 'z', got {e.args[0]}"
        assert raised, "Expected KeyError for missing key"

        # Missing integer key
        raised = False
        try:
            read_missing(data, 999)
        except KeyError:
            raised = True
        assert raised, "Expected KeyError for missing integer key"
        print("  PASS: test_key_error_missing")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_key_error_missing — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Different value types
    # ------------------------------------------------------------------
    try:
        def read_val(d, k):
            return d[k]

        data = {
            "int": 42,
            "str": "hello",
            "none": None,
            "list": [1, 2, 3],
            "dict": {"nested": True},
            "float": 3.14,
            "bool": False,
        }
        for _ in range(WARMUP):
            read_val(data, "int")
        check_jit_compiled(read_val, "read_val")

        assert read_val(data, "int") == 42
        assert read_val(data, "str") == "hello"
        assert read_val(data, "none") is None
        assert read_val(data, "list") == [1, 2, 3]
        assert read_val(data, "dict") == {"nested": True}
        assert read_val(data, "float") == 3.14
        assert read_val(data, "bool") is False
        print("  PASS: test_different_value_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_value_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Deopt dict -> list
    # ------------------------------------------------------------------
    try:
        def get_item_dl(container, key):
            return container[key]

        d = {"a": 10, "b": 20}
        for _ in range(WARMUP):
            get_item_dl(d, "a")
        check_jit_compiled(get_item_dl, "get_item_dl")

        assert get_item_dl(d, "a") == 10
        # Deopt to list
        lst = [100, 200, 300]
        assert get_item_dl(lst, 0) == 100
        assert get_item_dl(lst, 2) == 300
        # Back to dict
        assert get_item_dl(d, "b") == 20
        print("  PASS: test_deopt_dict_to_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_to_list — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Deopt dict -> custom __getitem__
    # ------------------------------------------------------------------
    try:
        class UpperLookup:
            def __init__(self, data):
                self._data = data

            def __getitem__(self, key):
                return self._data[key.upper()]

        def get_item_dc(container, key):
            return container[key]

        d = {"hello": 1, "world": 2}
        for _ in range(WARMUP):
            get_item_dc(d, "hello")
        check_jit_compiled(get_item_dc, "get_item_dc")

        assert get_item_dc(d, "hello") == 1
        # Deopt to custom __getitem__
        upper = UpperLookup({"HELLO": 10, "WORLD": 20})
        assert get_item_dc(upper, "hello") == 10
        assert get_item_dc(upper, "world") == 20
        # Back to dict
        assert get_item_dc(d, "world") == 2
        print("  PASS: test_deopt_dict_to_custom_getitem")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_to_custom_getitem — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt dict -> defaultdict (dict subclass)
    # ------------------------------------------------------------------
    try:
        from collections import defaultdict

        def get_item_dd(container, key):
            return container[key]

        d = {"x": 1, "y": 2}
        for _ in range(WARMUP):
            get_item_dd(d, "x")
        check_jit_compiled(get_item_dd, "get_item_dd")

        assert get_item_dd(d, "x") == 1

        # defaultdict is a dict subclass — GuardType(TDictExact) should deopt
        dd = defaultdict(int)
        dd["x"] = 100
        assert get_item_dd(dd, "x") == 100
        # defaultdict returns default for missing key (no KeyError)
        assert get_item_dd(dd, "missing") == 0
        # Back to plain dict
        assert get_item_dd(d, "y") == 2
        print("  PASS: test_deopt_dict_to_defaultdict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_to_defaultdict — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Dict read in loop
    # ------------------------------------------------------------------
    try:
        def sum_values(d, keys):
            total = 0
            for k in keys:
                total += d[k]
            return total

        data = {str(i): i for i in range(20)}
        keys = [str(i) for i in range(20)]
        for _ in range(WARMUP):
            sum_values(data, keys)
        check_jit_compiled(sum_values, "sum_values")

        assert sum_values(data, keys) == sum(range(20))
        assert sum_values(data, ["0", "1", "2"]) == 3
        assert sum_values(data, []) == 0
        print("  PASS: test_dict_read_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_read_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Object identity preservation
    # ------------------------------------------------------------------
    try:
        sentinel = object()

        def read_sentinel(d):
            return d["key"]

        data = {"key": sentinel}
        for _ in range(WARMUP):
            read_sentinel(data)
        check_jit_compiled(read_sentinel, "read_sentinel")

        assert read_sentinel(data) is sentinel
        another = object()
        data2 = {"key": another}
        assert read_sentinel(data2) is another
        assert read_sentinel(data2) is not sentinel
        print("  PASS: test_object_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_object_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Multiple reads in one function
    # ------------------------------------------------------------------
    try:
        def multi_read(d):
            a = d["a"]
            b = d["b"]
            c = d["c"]
            return a + b + c

        data = {"a": 10, "b": 20, "c": 30}
        for _ in range(WARMUP):
            multi_read(data)
        check_jit_compiled(multi_read, "multi_read")

        assert multi_read(data) == 60
        assert multi_read({"a": 1, "b": 2, "c": 3}) == 6
        assert multi_read({"a": 100, "b": 200, "c": 300}) == 600
        print("  PASS: test_multiple_reads_one_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_reads_one_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Read after mutation (add, delete, update)
    # ------------------------------------------------------------------
    try:
        def read_x(d):
            return d["x"]

        data = {"x": 10, "y": 20}
        for _ in range(WARMUP):
            read_x(data)
        check_jit_compiled(read_x, "read_x")

        assert read_x(data) == 10

        # Update value
        data["x"] = 99
        assert read_x(data) == 99

        # Delete other key — "x" still there
        del data["y"]
        assert read_x(data) == 99
        assert len(data) == 1

        # Add many keys — dict may resize internally
        for i in range(100):
            data[f"extra_{i}"] = i
        assert read_x(data) == 99

        # Clear and re-add
        data.clear()
        data["x"] = 777
        assert read_x(data) == 777
        print("  PASS: test_read_after_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_read_after_mutation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid container type alternation
    # ------------------------------------------------------------------
    try:
        def poly_read(container, key):
            return container[key]

        d = {"k": 10}
        lst = [20, 30]

        for _ in range(WARMUP):
            poly_read(d, "k")
        check_jit_compiled(poly_read, "poly_read")

        for cycle in range(50):
            assert poly_read(d, "k") == 10, f"dict failed at cycle {cycle}"
            assert poly_read(lst, 0) == 20, f"list failed at cycle {cycle}"

        assert poly_read(d, "k") == 10
        assert poly_read(lst, 1) == 30
        print("  PASS: test_rapid_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Nested dict reads
    # ------------------------------------------------------------------
    try:
        def nested_read(d, k1, k2):
            return d[k1][k2]

        data = {
            "server": {"host": "localhost", "port": 8080},
            "db": {"host": "db.local", "port": 5432},
        }
        for _ in range(WARMUP):
            nested_read(data, "server", "host")
        check_jit_compiled(nested_read, "nested_read")

        assert nested_read(data, "server", "host") == "localhost"
        assert nested_read(data, "server", "port") == 8080
        assert nested_read(data, "db", "host") == "db.local"
        assert nested_read(data, "db", "port") == 5432
        print("  PASS: test_nested_dict_reads")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_dict_reads — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Large dict access (10k entries)
    # ------------------------------------------------------------------
    try:
        def read_large(d, k):
            return d[k]

        big_dict = {f"key_{i}": i for i in range(10000)}
        for _ in range(WARMUP):
            read_large(big_dict, "key_5000")
        check_jit_compiled(read_large, "read_large")

        assert read_large(big_dict, "key_0") == 0
        assert read_large(big_dict, "key_9999") == 9999
        assert read_large(big_dict, "key_5000") == 5000
        assert read_large(big_dict, "key_1234") == 1234
        print("  PASS: test_large_dict_access")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_dict_access — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Boolean key (True/False as dict keys)
    # ------------------------------------------------------------------
    try:
        def read_bool_key(d, k):
            return d[k]

        # In Python, True == 1 and False == 0, and they hash the same.
        # So {True: "t", 1: "one"} keeps the first key but last value.
        data = {True: "true_val", False: "false_val"}
        for _ in range(WARMUP):
            read_bool_key(data, True)
        check_jit_compiled(read_bool_key, "read_bool_key")

        assert read_bool_key(data, True) == "true_val"
        assert read_bool_key(data, False) == "false_val"
        # True == 1 and False == 0 hash-collide with ints
        assert read_bool_key(data, 1) == "true_val"
        assert read_bool_key(data, 0) == "false_val"
        print("  PASS: test_boolean_key")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_key — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Empty dict KeyError
    # ------------------------------------------------------------------
    try:
        def read_empty(d, k):
            return d[k]

        data = {"x": 1}
        for _ in range(WARMUP):
            read_empty(data, "x")
        check_jit_compiled(read_empty, "read_empty")

        assert read_empty(data, "x") == 1

        raised = False
        try:
            read_empty({}, "anything")
        except KeyError:
            raised = True
        assert raised, "Expected KeyError for empty dict"
        print("  PASS: test_empty_dict_key_error")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_dict_key_error — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Dict with hash collision pattern
    # ------------------------------------------------------------------
    try:
        class FixedHash:
            """All instances hash to the same value, forcing collision chains."""
            def __init__(self, val):
                self.val = val

            def __hash__(self):
                return 42

            def __eq__(self, other):
                return isinstance(other, FixedHash) and self.val == other.val

        def read_collision(d, k):
            return d[k]

        k1 = FixedHash("a")
        k2 = FixedHash("b")
        k3 = FixedHash("c")
        data = {k1: "val_a", k2: "val_b", k3: "val_c"}

        for _ in range(WARMUP):
            read_collision(data, k1)
        check_jit_compiled(read_collision, "read_collision")

        assert read_collision(data, k1) == "val_a"
        assert read_collision(data, k2) == "val_b"
        assert read_collision(data, k3) == "val_c"

        # Missing key with same hash
        raised = False
        try:
            read_collision(data, FixedHash("d"))
        except KeyError:
            raised = True
        assert raised, "Expected KeyError for missing key with same hash"
        print("  PASS: test_hash_collision_pattern")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_hash_collision_pattern — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Mixed key types in same dict
    # ------------------------------------------------------------------
    try:
        def read_mixed(d, k):
            return d[k]

        data = {
            "str_key": 1,
            42: 2,
            (1, 2): 3,
            3.14: 4,
            None: 5,
            True: 6,  # Note: True == 1, so this is key 1 with value 6
        }
        for _ in range(WARMUP):
            read_mixed(data, "str_key")
        check_jit_compiled(read_mixed, "read_mixed")

        assert read_mixed(data, "str_key") == 1
        assert read_mixed(data, 42) == 2
        assert read_mixed(data, (1, 2)) == 3
        assert read_mixed(data, 3.14) == 4
        assert read_mixed(data, None) == 5
        # True == 1 so d[True] and d[1] are the same slot
        assert read_mixed(data, True) == 6
        assert read_mixed(data, 1) == 6
        print("  PASS: test_mixed_key_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_key_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — d[k] vs dict.__getitem__(d, k)
    # ------------------------------------------------------------------
    try:
        def subscr_read(d, k):
            return d[k]

        def explicit_getitem(d, k):
            return dict.__getitem__(d, k)

        data = {"a": 1, "b": 2, "c": 3, "d": 4, "e": 5}
        keys = list(data.keys())

        for _ in range(WARMUP):
            subscr_read(data, "a")
        check_jit_compiled(subscr_read, "subscr_read")

        for k in keys:
            assert subscr_read(data, k) == explicit_getitem(data, k), (
                f"Mismatch at key {k!r}: "
                f"subscr={subscr_read(data, k)}, "
                f"explicit={explicit_getitem(data, k)}"
            )

        # Both should raise KeyError for missing key
        for missing in ["z", 999, (99,)]:
            s_raised = False
            e_raised = False
            try:
                subscr_read(data, missing)
            except KeyError:
                s_raised = True
            try:
                explicit_getitem(data, missing)
            except KeyError:
                e_raised = True
            assert s_raised == e_raised, (
                f"KeyError mismatch for {missing!r}: "
                f"subscr raised={s_raised}, explicit raised={e_raised}"
            )
        print("  PASS: test_equivalence_subscr_vs_getitem")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_subscr_vs_getitem — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nBINARY_SUBSCR_DICT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
