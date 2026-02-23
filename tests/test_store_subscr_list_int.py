#!/usr/bin/env python3
"""
test_store_subscr_list_int.py — Correctness and deopt tests for
STORE_SUBSCR_LIST_INT specialisation.

Targets: STORE_SUBSCR_LIST_INT.

STORE_SUBSCR_LIST_INT specialises the subscript store operation (obj[key] = val)
when the container is a list and the index is an int. Instead of going through
generic PyObject_SetItem → mp_ass_subscript dispatch, it uses PyList_SET_ITEM
directly after bounds checking.

The JIT specialisation emits GuardType(TListExact) on the container and
GuardType(TLongExact) on the index, allowing the Simplify pass to use a
direct list store without dispatch overhead.

Deopt triggers:
  - Function JIT-compiled with list[int] stores, then called with dict/other
  - Index is not int (e.g. slice)
  - Container is not a list (e.g. dict, custom __setitem__)
  - List subclass

Tests cover:
  - Basic list store (positive index)
  - Negative index
  - Boundary indices (first, last)
  - IndexError for out-of-bounds
  - Store different value types (int, str, None, list, object)
  - Deopt: list-compiled → dict store
  - Deopt: list-compiled → custom __setitem__
  - Deopt: int index → slice assignment
  - List mutation via store in loop
  - Store preserving list identity (same object)
  - Multiple stores in one function
  - Store after append (growing list)
  - Rapid container type alternation
  - Nested list stores

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_store_subscr_list_int.py
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
    print("=== STORE_SUBSCR_LIST_INT Correctness & Deopt Tests ===")
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

    # ── Test 1: Basic list store (positive index) ────────────────────────

    def store_basic_1(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_basic_1(tmp, 1, 42)

    check_jit_compiled(store_basic_1, "store_basic_1")

    try:
        lst = [1, 2, 3]
        store_basic_1(lst, 0, 10)
        assert lst == [10, 2, 3]
        store_basic_1(lst, 1, 20)
        assert lst == [10, 20, 3]
        store_basic_1(lst, 2, 30)
        assert lst == [10, 20, 30]
        print("PASS  Test 1: basic list store (positive index)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic list store — {e}")
        failed += 1

    # ── Test 2: Negative index ───────────────────────────────────────────

    def store_neg_2(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_neg_2(tmp, -1, 99)

    check_jit_compiled(store_neg_2, "store_neg_2")

    try:
        lst = [1, 2, 3, 4, 5]
        store_neg_2(lst, -1, 50)
        assert lst == [1, 2, 3, 4, 50]
        store_neg_2(lst, -2, 40)
        assert lst == [1, 2, 3, 40, 50]
        store_neg_2(lst, -5, 10)
        assert lst == [10, 2, 3, 40, 50]
        print("PASS  Test 2: negative index")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: negative index — {e}")
        failed += 1

    # ── Test 3: Boundary indices ─────────────────────────────────────────

    def store_boundary_3(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0]
        store_boundary_3(tmp, 0, 1)

    check_jit_compiled(store_boundary_3, "store_boundary_3")

    try:
        # Single element list
        lst = [99]
        store_boundary_3(lst, 0, 1)
        assert lst == [1]
        store_boundary_3(lst, -1, 2)
        assert lst == [2]

        # First and last of longer list
        lst = [0, 0, 0, 0, 0]
        store_boundary_3(lst, 0, "first")
        store_boundary_3(lst, 4, "last")
        assert lst == ["first", 0, 0, 0, "last"]

        print("PASS  Test 3: boundary indices (first, last)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: boundary indices — {e}")
        failed += 1

    # ── Test 4: IndexError for out-of-bounds ─────────────────────────────

    def store_oob_4(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_oob_4(tmp, 1, 42)

    check_jit_compiled(store_oob_4, "store_oob_4")

    try:
        lst = [1, 2, 3]

        try:
            store_oob_4(lst, 3, 99)
            assert False, "expected IndexError for index 3"
        except IndexError:
            pass

        try:
            store_oob_4(lst, -4, 99)
            assert False, "expected IndexError for index -4"
        except IndexError:
            pass

        try:
            store_oob_4(lst, 100, 99)
            assert False, "expected IndexError for index 100"
        except IndexError:
            pass

        # List unchanged after errors
        assert lst == [1, 2, 3]

        # Normal store still works after errors
        store_oob_4(lst, 0, 10)
        assert lst == [10, 2, 3]

        print("PASS  Test 4: IndexError for out-of-bounds")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: out-of-bounds — {e}")
        failed += 1

    # ── Test 5: Store different value types ──────────────────────────────

    def store_types_5(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_types_5(tmp, 0, 42)

    check_jit_compiled(store_types_5, "store_types_5")

    try:
        lst = [None, None, None, None, None]
        store_types_5(lst, 0, 42)
        store_types_5(lst, 1, "hello")
        store_types_5(lst, 2, None)
        store_types_5(lst, 3, [1, 2])
        store_types_5(lst, 4, {"a": 1})
        assert lst == [42, "hello", None, [1, 2], {"a": 1}]
        print("PASS  Test 5: store different value types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: different value types — {e}")
        failed += 1

    # ── Test 6: Store preserves list identity ────────────────────────────

    def store_identity_6(lst, idx, val):
        lst[idx] = val
        return lst

    for _ in range(WARMUP):
        tmp = [0, 0]
        store_identity_6(tmp, 0, 1)

    check_jit_compiled(store_identity_6, "store_identity_6")

    try:
        original = [1, 2, 3]
        original_id = id(original)
        result = store_identity_6(original, 1, 99)
        assert result is original
        assert id(original) == original_id
        assert original == [1, 99, 3]
        print("PASS  Test 6: store preserves list identity")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: list identity — {e}")
        failed += 1

    # ── Test 7: Deopt list → dict store ──────────────────────────────────

    def store_deopt_7(container, key, val):
        container[key] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_deopt_7(tmp, 1, 42)

    check_jit_compiled(store_deopt_7, "store_deopt_7")

    try:
        lst = [1, 2, 3]
        store_deopt_7(lst, 0, 10)
        assert lst == [10, 2, 3]

        # Deopt: dict container
        d = {"a": 1}
        store_deopt_7(d, "b", 2)
        assert d == {"a": 1, "b": 2}

        # List still works after deopt
        store_deopt_7(lst, 2, 30)
        assert lst == [10, 2, 30]

        print("PASS  Test 7: deopt list → dict store")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: deopt list → dict — {e}")
        failed += 1

    # ── Test 8: Deopt list → custom __setitem__ ──────────────────────────

    class CustomStore:
        def __init__(self):
            self.data = {}
        def __setitem__(self, key, val):
            self.data[key] = val

    def store_deopt_8(container, key, val):
        container[key] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_deopt_8(tmp, 1, 42)

    check_jit_compiled(store_deopt_8, "store_deopt_8")

    try:
        lst = [1, 2, 3]
        store_deopt_8(lst, 0, 10)
        assert lst == [10, 2, 3]

        # Deopt: custom __setitem__
        cs = CustomStore()
        store_deopt_8(cs, "key", "value")
        assert cs.data == {"key": "value"}

        # List still works
        store_deopt_8(lst, 1, 20)
        assert lst == [10, 20, 3]

        print("PASS  Test 8: deopt list → custom __setitem__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt custom __setitem__ — {e}")
        failed += 1

    # ── Test 9: Deopt int index → slice assignment ───────────────────────

    def store_deopt_9(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_deopt_9(tmp, 1, 42)

    check_jit_compiled(store_deopt_9, "store_deopt_9")

    try:
        lst = [1, 2, 3, 4, 5]
        store_deopt_9(lst, 0, 10)
        assert lst == [10, 2, 3, 4, 5]

        # Deopt: slice index (replaces a range)
        store_deopt_9(lst, slice(1, 3), [20, 30])
        assert lst == [10, 20, 30, 4, 5]

        # Int index still works
        store_deopt_9(lst, 4, 50)
        assert lst == [10, 20, 30, 4, 50]

        print("PASS  Test 9: deopt int index → slice assignment")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt slice — {e}")
        failed += 1

    # ── Test 10: List mutation via store in loop ─────────────────────────

    def fill_list_10(lst, n):
        for i in range(n):
            lst[i] = i * i
        return lst

    for _ in range(WARMUP):
        fill_list_10([0, 0, 0, 0, 0], 5)

    check_jit_compiled(fill_list_10, "fill_list_10")

    try:
        lst = [0] * 10
        fill_list_10(lst, 10)
        assert lst == [0, 1, 4, 9, 16, 25, 36, 49, 64, 81]

        lst = [0] * 5
        fill_list_10(lst, 5)
        assert lst == [0, 1, 4, 9, 16]

        print("PASS  Test 10: list store in loop (fill with squares)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: store in loop — {e}")
        failed += 1

    # ── Test 11: Multiple stores in one function ─────────────────────────

    def swap_12(lst, i, j):
        tmp = lst[i]
        lst[i] = lst[j]
        lst[j] = tmp

    for _ in range(WARMUP):
        tmp = [1, 2, 3]
        swap_12(tmp, 0, 2)

    check_jit_compiled(swap_12, "swap_12")

    try:
        lst = [1, 2, 3, 4, 5]
        swap_12(lst, 0, 4)
        assert lst == [5, 2, 3, 4, 1]
        swap_12(lst, 1, 3)
        assert lst == [5, 4, 3, 2, 1]
        swap_12(lst, 2, 2)  # swap with self
        assert lst == [5, 4, 3, 2, 1]
        print("PASS  Test 11: multiple stores (swap)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: swap — {e}")
        failed += 1

    # ── Test 12: Store after append (growing list) ───────────────────────

    def append_and_store_12(lst, val):
        lst.append(0)  # Grow list
        lst[-1] = val   # Store at new position

    for _ in range(WARMUP):
        tmp = []
        append_and_store_12(tmp, 42)

    check_jit_compiled(append_and_store_12, "append_and_store_12")

    try:
        lst = [1, 2, 3]
        append_and_store_12(lst, 4)
        assert lst == [1, 2, 3, 4]
        append_and_store_12(lst, 5)
        assert lst == [1, 2, 3, 4, 5]

        lst2 = []
        for i in range(5):
            append_and_store_12(lst2, i * 10)
        assert lst2 == [0, 10, 20, 30, 40]

        print("PASS  Test 12: store after append")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: store after append — {e}")
        failed += 1

    # ── Test 13: Store with bool index (bool is int subclass) ────────────

    def store_bool_13(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_bool_13(tmp, 1, 42)

    check_jit_compiled(store_bool_13, "store_bool_13")

    try:
        lst = [1, 2, 3]
        # True == 1, False == 0
        store_bool_13(lst, True, 20)
        assert lst == [1, 20, 3]
        store_bool_13(lst, False, 10)
        assert lst == [10, 20, 3]
        print("PASS  Test 13: bool index (True=1, False=0)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: bool index — {e}")
        failed += 1

    # ── Test 14: Empty list IndexError ───────────────────────────────────

    def store_empty_14(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_empty_14(tmp, 0, 1)

    check_jit_compiled(store_empty_14, "store_empty_14")

    try:
        try:
            store_empty_14([], 0, 99)
            assert False, "expected IndexError for empty list"
        except IndexError:
            pass

        # Normal store still works
        lst = [1]
        store_empty_14(lst, 0, 42)
        assert lst == [42]

        print("PASS  Test 14: empty list IndexError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: empty list — {e}")
        failed += 1

    # ── Test 15: Rapid container type alternation ────────────────────────

    def store_rapid_15(container, key, val):
        container[key] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_rapid_15(tmp, 1, 42)

    check_jit_compiled(store_rapid_15, "store_rapid_15")

    try:
        for i in range(10):
            # List store
            lst = [0, 0, 0]
            store_rapid_15(lst, 1, 99)
            assert lst[1] == 99, f"cycle {i}: list store failed"

            # Dict store (deopt)
            d = {}
            store_rapid_15(d, "k", 42)
            assert d["k"] == 42, f"cycle {i}: dict store failed"

            # Bytearray store (deopt)
            ba = bytearray(b"\x00\x00\x00")
            store_rapid_15(ba, 0, 65)
            assert ba[0] == 65, f"cycle {i}: bytearray store failed"

        print("PASS  Test 15: rapid container type alternation (10 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: rapid type alternation — {e}")
        failed += 1

    # ── Test 16: Nested list store ───────────────────────────────────────

    def store_nested_16(lst, i, j, val):
        lst[i][j] = val

    for _ in range(WARMUP):
        tmp = [[0, 0], [0, 0]]
        store_nested_16(tmp, 0, 1, 42)

    check_jit_compiled(store_nested_16, "store_nested_16")

    try:
        matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
        store_nested_16(matrix, 0, 0, 10)
        assert matrix[0][0] == 10
        store_nested_16(matrix, 2, 2, 90)
        assert matrix[2][2] == 90
        store_nested_16(matrix, 1, 1, 50)
        assert matrix == [[10, 2, 3], [4, 50, 6], [7, 8, 90]]
        print("PASS  Test 16: nested list store (matrix)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: nested list store — {e}")
        failed += 1

    # ── Test 17: Overwrite same index repeatedly ─────────────────────────

    def overwrite_17(lst, idx, val):
        lst[idx] = val

    for _ in range(WARMUP):
        tmp = [0]
        overwrite_17(tmp, 0, 1)

    check_jit_compiled(overwrite_17, "overwrite_17")

    try:
        lst = [0]
        for i in range(100):
            overwrite_17(lst, 0, i)
        assert lst == [99]
        print("PASS  Test 17: overwrite same index 100 times")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: overwrite — {e}")
        failed += 1

    # ── Test 18: TypeError for non-subscriptable ─────────────────────────

    def store_type_err_18(container, key, val):
        container[key] = val

    for _ in range(WARMUP):
        tmp = [0, 0, 0]
        store_type_err_18(tmp, 1, 42)

    check_jit_compiled(store_type_err_18, "store_type_err_18")

    try:
        lst = [1, 2, 3]
        store_type_err_18(lst, 0, 10)
        assert lst == [10, 2, 3]

        # TypeError: int is not subscriptable
        try:
            store_type_err_18(42, 0, 1)
            assert False, "expected TypeError for int"
        except TypeError:
            pass

        # TypeError: str doesn't support item assignment
        try:
            store_type_err_18("abc", 0, "x")
            assert False, "expected TypeError for str"
        except TypeError:
            pass

        # TypeError: tuple doesn't support item assignment
        try:
            store_type_err_18((1, 2, 3), 0, 99)
            assert False, "expected TypeError for tuple"
        except TypeError:
            pass

        # List still works after errors
        store_type_err_18(lst, 2, 30)
        assert lst == [10, 2, 30]

        print("PASS  Test 18: TypeError for non-subscriptable")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: TypeError — {e}")
        failed += 1

    # ── Test 19: Reverse list in-place via stores ────────────────────────

    def reverse_inplace_19(lst):
        n = len(lst)
        for i in range(n // 2):
            tmp = lst[i]
            lst[i] = lst[n - 1 - i]
            lst[n - 1 - i] = tmp

    for _ in range(WARMUP):
        reverse_inplace_19([1, 2, 3, 4])

    check_jit_compiled(reverse_inplace_19, "reverse_inplace_19")

    try:
        lst = [1, 2, 3, 4, 5]
        reverse_inplace_19(lst)
        assert lst == [5, 4, 3, 2, 1]

        lst = [1]
        reverse_inplace_19(lst)
        assert lst == [1]

        lst = [1, 2]
        reverse_inplace_19(lst)
        assert lst == [2, 1]

        lst = []
        reverse_inplace_19(lst)
        assert lst == []

        print("PASS  Test 19: reverse list in-place via stores")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: reverse in-place — {e}")
        failed += 1

    # ── Test 20: Store with large index ──────────────────────────────────

    def store_large_20(lst, idx, val):
        lst[idx] = val

    large_list = [0] * 100000

    for _ in range(WARMUP):
        store_large_20(large_list, 50000, 1)

    check_jit_compiled(store_large_20, "store_large_20")

    try:
        store_large_20(large_list, 0, "first")
        store_large_20(large_list, 99999, "last")
        store_large_20(large_list, 50000, "mid")
        assert large_list[0] == "first"
        assert large_list[99999] == "last"
        assert large_list[50000] == "mid"
        assert large_list[1] == 0  # Unmodified
        assert large_list[99998] == 0  # Unmodified

        # Negative on large list
        store_large_20(large_list, -1, "neg_last")
        assert large_list[99999] == "neg_last"

        print("PASS  Test 20: large list (100k elements)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: large list — {e}")
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
