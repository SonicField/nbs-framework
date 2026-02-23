#!/usr/bin/env python3
"""
test_binary_subscr_list_int.py — Correctness and deopt tests for
BINARY_SUBSCR_LIST_INT specialisation.

Targets: BINARY_SUBSCR_LIST_INT.

BINARY_SUBSCR_LIST_INT specialises the subscript load operation (obj[key])
when the container is a list and the index is an int. Instead of going through
generic PyObject_GetItem → mp_subscript dispatch, it uses PyList_GET_ITEM
directly after bounds checking and negative index normalisation.

The JIT specialisation emits GuardType(TListExact) on the container and
GuardType(TLongExact) on the index, allowing the Simplify pass to use a
direct list load without dispatch overhead.

Deopt triggers:
  - Function JIT-compiled with list[int] loads, then called with dict/other
  - Index is not int (e.g. slice)
  - Container is not a list (e.g. tuple, dict, custom __getitem__)
  - List subclass

Tests cover:
  - Basic list indexing (positive index)
  - Negative index
  - Boundary indices (first, last)
  - IndexError for out-of-bounds
  - Lists with different element types (int, str, None, mixed)
  - Deopt: list-compiled -> tuple load
  - Deopt: list-compiled -> dict load
  - Deopt: list-compiled -> custom __getitem__
  - Deopt: int index -> slice
  - List read in loop
  - List identity (same object across reads)
  - Multiple reads in one function
  - Read after mutation (append, pop, insert)
  - Rapid container type alternation
  - Nested list reads
  - Large list access
  - Boolean index (bool is subclass of int)
  - Empty list IndexError

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_subscr_list_int.py
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
    # Test 1: Basic positive index
    # ------------------------------------------------------------------
    try:
        def read_index_zero(lst):
            return lst[0]

        data = [10, 20, 30, 40, 50]
        for _ in range(WARMUP):
            read_index_zero(data)
        check_jit_compiled(read_index_zero, "read_index_zero")

        assert read_index_zero(data) == 10
        assert read_index_zero([99]) == 99
        assert read_index_zero([100, 200, 300]) == 100
        print("  PASS: test_basic_positive_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_positive_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Negative index
    # ------------------------------------------------------------------
    try:
        def read_last(lst):
            return lst[-1]

        data = [10, 20, 30, 40, 50]
        for _ in range(WARMUP):
            read_last(data)
        check_jit_compiled(read_last, "read_last")

        assert read_last(data) == 50
        assert read_last([7]) == 7
        assert read_last([1, 2, 3]) == 3

        def read_neg_two(lst):
            return lst[-2]

        for _ in range(WARMUP):
            read_neg_two(data)
        assert read_neg_two(data) == 40
        assert read_neg_two([1, 2]) == 1
        print("  PASS: test_negative_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Boundary indices (first, last)
    # ------------------------------------------------------------------
    try:
        def read_first_and_last(lst):
            return (lst[0], lst[len(lst) - 1])

        data = [100, 200, 300, 400, 500]
        for _ in range(WARMUP):
            read_first_and_last(data)
        check_jit_compiled(read_first_and_last, "read_first_and_last")

        assert read_first_and_last(data) == (100, 500)
        assert read_first_and_last([42]) == (42, 42)  # single element
        assert read_first_and_last([1, 2]) == (1, 2)
        print("  PASS: test_boundary_indices")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boundary_indices — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: IndexError for out-of-bounds
    # ------------------------------------------------------------------
    try:
        def read_oob(lst, idx):
            return lst[idx]

        data = [10, 20, 30]
        for _ in range(WARMUP):
            read_oob(data, 1)
        check_jit_compiled(read_oob, "read_oob")

        # Positive out-of-bounds
        raised = False
        try:
            read_oob(data, 5)
        except IndexError:
            raised = True
        assert raised, "Expected IndexError for positive out-of-bounds"

        # Negative out-of-bounds
        raised = False
        try:
            read_oob(data, -4)
        except IndexError:
            raised = True
        assert raised, "Expected IndexError for negative out-of-bounds"

        # Exactly at boundary (should work)
        assert read_oob(data, 2) == 30
        assert read_oob(data, -3) == 10
        print("  PASS: test_index_error_oob")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_index_error_oob — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Lists with different element types
    # ------------------------------------------------------------------
    try:
        def read_second(lst):
            return lst[1]

        int_list = [1, 2, 3]
        str_list = ["a", "b", "c"]
        none_list = [None, None, None]
        mixed_list = [1, "two", 3.0, None]

        for _ in range(WARMUP):
            read_second(int_list)
        check_jit_compiled(read_second, "read_second")

        assert read_second(int_list) == 2
        assert read_second(str_list) == "b"
        assert read_second(none_list) is None
        assert read_second(mixed_list) == "two"
        print("  PASS: test_different_element_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_element_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Deopt list -> tuple
    # ------------------------------------------------------------------
    try:
        def get_item(container, idx):
            return container[idx]

        lst = [10, 20, 30]
        for _ in range(WARMUP):
            get_item(lst, 1)
        check_jit_compiled(get_item, "get_item")

        # After JIT with list, switch to tuple
        tup = (40, 50, 60)
        assert get_item(lst, 0) == 10
        assert get_item(tup, 0) == 40  # deopt
        assert get_item(tup, 2) == 60
        assert get_item(lst, 2) == 30  # back to list
        print("  PASS: test_deopt_list_to_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_list_to_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Deopt list -> dict
    # ------------------------------------------------------------------
    try:
        def get_item_d(container, key):
            return container[key]

        lst = [10, 20, 30]
        for _ in range(WARMUP):
            get_item_d(lst, 1)
        check_jit_compiled(get_item_d, "get_item_d")

        d = {0: "zero", 1: "one", 2: "two"}
        assert get_item_d(lst, 1) == 20
        assert get_item_d(d, 1) == "one"  # deopt to dict
        assert get_item_d(d, 0) == "zero"
        assert get_item_d(lst, 0) == 10   # back to list
        print("  PASS: test_deopt_list_to_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_list_to_dict — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt list -> custom __getitem__
    # ------------------------------------------------------------------
    try:
        class Doubler:
            def __getitem__(self, key):
                return key * 2

        def get_item_c(container, key):
            return container[key]

        lst = [10, 20, 30]
        for _ in range(WARMUP):
            get_item_c(lst, 1)
        check_jit_compiled(get_item_c, "get_item_c")

        doubler = Doubler()
        assert get_item_c(lst, 2) == 30
        assert get_item_c(doubler, 5) == 10   # deopt to custom __getitem__
        assert get_item_c(doubler, 7) == 14
        assert get_item_c(lst, 0) == 10       # back to list
        print("  PASS: test_deopt_list_to_custom_getitem")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_list_to_custom_getitem — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt int index -> slice
    # ------------------------------------------------------------------
    try:
        def get_slice(lst, key):
            return lst[key]

        data = [10, 20, 30, 40, 50]
        for _ in range(WARMUP):
            get_slice(data, 2)
        check_jit_compiled(get_slice, "get_slice")

        assert get_slice(data, 2) == 30
        # Slice index — deopt from TLongExact guard on index
        result = get_slice(data, slice(1, 3))
        assert result == [20, 30], f"Expected [20, 30], got {result}"
        # Back to int
        assert get_slice(data, 4) == 50
        print("  PASS: test_deopt_int_to_slice")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_slice — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: List read in loop (accumulator pattern)
    # ------------------------------------------------------------------
    try:
        def sum_list(lst):
            total = 0
            for i in range(len(lst)):
                total += lst[i]
            return total

        data = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        for _ in range(WARMUP):
            sum_list(data)
        check_jit_compiled(sum_list, "sum_list")

        assert sum_list(data) == 55
        assert sum_list([100]) == 100
        assert sum_list([]) == 0
        assert sum_list(list(range(100))) == 4950
        print("  PASS: test_list_read_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_read_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Object identity preservation
    # ------------------------------------------------------------------
    try:
        sentinel = object()

        def read_sentinel(lst):
            return lst[0]

        data = [sentinel, 1, 2]
        for _ in range(WARMUP):
            read_sentinel(data)
        check_jit_compiled(read_sentinel, "read_sentinel")

        assert read_sentinel(data) is sentinel
        # Same object, not just equal
        another = object()
        data2 = [another]
        assert read_sentinel(data2) is another
        assert read_sentinel(data2) is not sentinel
        print("  PASS: test_object_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_object_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Multiple reads in one function
    # ------------------------------------------------------------------
    try:
        def multi_read(lst):
            a = lst[0]
            b = lst[1]
            c = lst[2]
            return a + b + c

        data = [10, 20, 30, 40]
        for _ in range(WARMUP):
            multi_read(data)
        check_jit_compiled(multi_read, "multi_read")

        assert multi_read(data) == 60
        assert multi_read([1, 2, 3]) == 6
        assert multi_read([100, 200, 300]) == 600
        print("  PASS: test_multiple_reads_one_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_reads_one_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Read after mutation (append, pop, insert)
    # ------------------------------------------------------------------
    try:
        def read_after_mutate(lst):
            return lst[0]

        data = [10, 20, 30]
        for _ in range(WARMUP):
            read_after_mutate(data)
        check_jit_compiled(read_after_mutate, "read_after_mutate")

        assert read_after_mutate(data) == 10

        # Append doesn't change index 0
        data.append(40)
        assert read_after_mutate(data) == 10
        assert len(data) == 4

        # Pop from front changes index 0
        data.pop(0)
        assert read_after_mutate(data) == 20

        # Insert at front changes index 0
        data.insert(0, 99)
        assert read_after_mutate(data) == 99

        # Clear and rebuild
        data.clear()
        data.append(777)
        assert read_after_mutate(data) == 777
        print("  PASS: test_read_after_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_read_after_mutation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid container type alternation
    # ------------------------------------------------------------------
    try:
        def poly_read(container, idx):
            return container[idx]

        lst = [10, 20, 30]
        tup = (40, 50, 60)

        for _ in range(WARMUP):
            poly_read(lst, 1)
        check_jit_compiled(poly_read, "poly_read")

        # Alternate rapidly between list and tuple
        for cycle in range(50):
            assert poly_read(lst, 1) == 20, f"list failed at cycle {cycle}"
            assert poly_read(tup, 1) == 50, f"tuple failed at cycle {cycle}"

        # Final check both still work
        assert poly_read(lst, 0) == 10
        assert poly_read(tup, 2) == 60
        print("  PASS: test_rapid_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Nested list reads
    # ------------------------------------------------------------------
    try:
        def nested_read(matrix, row, col):
            return matrix[row][col]

        mat = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
        for _ in range(WARMUP):
            nested_read(mat, 1, 1)
        check_jit_compiled(nested_read, "nested_read")

        assert nested_read(mat, 0, 0) == 1
        assert nested_read(mat, 1, 2) == 6
        assert nested_read(mat, 2, 0) == 7
        assert nested_read(mat, 2, 2) == 9
        print("  PASS: test_nested_list_reads")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_list_reads — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Large list access
    # ------------------------------------------------------------------
    try:
        def read_large(lst, idx):
            return lst[idx]

        big_list = list(range(10000))
        for _ in range(WARMUP):
            read_large(big_list, 5000)
        check_jit_compiled(read_large, "read_large")

        assert read_large(big_list, 0) == 0
        assert read_large(big_list, 9999) == 9999
        assert read_large(big_list, 5000) == 5000
        assert read_large(big_list, -1) == 9999
        assert read_large(big_list, -10000) == 0
        print("  PASS: test_large_list_access")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_list_access — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Boolean index (bool is subclass of int)
    # ------------------------------------------------------------------
    try:
        def read_bool_idx(lst, idx):
            return lst[idx]

        data = [10, 20, 30]
        for _ in range(WARMUP):
            read_bool_idx(data, 0)
        check_jit_compiled(read_bool_idx, "read_bool_idx")

        # bool is a subclass of int: True == 1, False == 0
        # This may or may not trigger deopt depending on whether
        # GuardType checks for exact int vs int subclass
        assert read_bool_idx(data, False) == 10  # False == 0
        assert read_bool_idx(data, True) == 20   # True == 1
        assert read_bool_idx(data, 0) == 10
        assert read_bool_idx(data, 1) == 20
        print("  PASS: test_boolean_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Empty list IndexError
    # ------------------------------------------------------------------
    try:
        def read_empty(lst):
            return lst[0]

        data = [42]
        for _ in range(WARMUP):
            read_empty(data)
        check_jit_compiled(read_empty, "read_empty")

        assert read_empty(data) == 42

        raised = False
        try:
            read_empty([])
        except IndexError:
            raised = True
        assert raised, "Expected IndexError for empty list"
        print("  PASS: test_empty_list_index_error")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_list_index_error — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: List subclass
    # ------------------------------------------------------------------
    try:
        class MyList(list):
            pass

        def read_subclass(lst, idx):
            return lst[idx]

        plain = [10, 20, 30]
        for _ in range(WARMUP):
            read_subclass(plain, 1)
        check_jit_compiled(read_subclass, "read_subclass")

        assert read_subclass(plain, 1) == 20

        # MyList is a subclass of list, GuardType(TListExact) should deopt
        ml = MyList([40, 50, 60])
        assert read_subclass(ml, 0) == 40  # deopt to generic path
        assert read_subclass(ml, 2) == 60
        assert read_subclass(plain, 0) == 10  # back to list
        print("  PASS: test_list_subclass_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_subclass_deopt — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — lst[i] vs list.__getitem__(lst, i)
    # ------------------------------------------------------------------
    try:
        def subscr_read(lst, idx):
            return lst[idx]

        def explicit_getitem(lst, idx):
            return list.__getitem__(lst, idx)

        data = [10, 20, 30, 40, 50]
        for _ in range(WARMUP):
            subscr_read(data, 2)
        check_jit_compiled(subscr_read, "subscr_read")

        for idx in range(5):
            assert subscr_read(data, idx) == explicit_getitem(data, idx), (
                f"Mismatch at index {idx}: "
                f"subscr={subscr_read(data, idx)}, "
                f"explicit={explicit_getitem(data, idx)}"
            )
        for idx in range(-5, 0):
            assert subscr_read(data, idx) == explicit_getitem(data, idx), (
                f"Mismatch at negative index {idx}: "
                f"subscr={subscr_read(data, idx)}, "
                f"explicit={explicit_getitem(data, idx)}"
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
    print(f"\nBINARY_SUBSCR_LIST_INT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
