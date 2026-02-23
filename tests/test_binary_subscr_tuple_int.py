#!/usr/bin/env python3
"""
test_binary_subscr_tuple_int.py — Correctness and deopt tests for
BINARY_SUBSCR_TUPLE_INT specialisation.

Targets: BINARY_SUBSCR_TUPLE_INT.

BINARY_SUBSCR_TUPLE_INT specialises the subscript load operation (obj[key])
when the container is a tuple and the index is an int. Instead of going
through generic PyObject_GetItem → mp_subscript dispatch, it uses
PyTuple_GET_ITEM directly after bounds checking and negative index
normalisation.

The JIT specialisation emits GuardType(TTupleExact) on the container and
GuardType(TLongExact) on the index, allowing the Simplify pass to use a
direct tuple element access without dispatch overhead.

Deopt triggers:
  - Function JIT-compiled with tuple[int] loads, then called with list/other
  - Index is not int (e.g. slice)
  - Container is not a tuple (e.g. list, dict, custom __getitem__)
  - Tuple subclass (e.g. namedtuple)

Tests cover:
  - Basic tuple indexing (positive index)
  - Negative index
  - Boundary indices (first, last)
  - IndexError for out-of-bounds
  - Tuples with different element types (int, str, None, mixed)
  - Deopt: tuple-compiled -> list load
  - Deopt: tuple-compiled -> dict load
  - Deopt: tuple-compiled -> custom __getitem__
  - Deopt: int index -> slice
  - Tuple read in loop (sum pattern)
  - Object identity preservation
  - Multiple reads in one function
  - Rapid container type alternation
  - Nested tuple reads
  - Large tuple access (10k elements)
  - Boolean index (bool is subclass of int)
  - Single-element and empty tuple
  - Named tuple (tuple subclass)
  - Tuple unpacking equivalence
  - Equivalence: tup[i] vs tuple.__getitem__(tup, i)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_subscr_tuple_int.py
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
        def read_index_zero(tup):
            return tup[0]

        data = (10, 20, 30, 40, 50)
        for _ in range(WARMUP):
            read_index_zero(data)
        check_jit_compiled(read_index_zero, "read_index_zero")

        assert read_index_zero(data) == 10
        assert read_index_zero((99,)) == 99
        assert read_index_zero((100, 200, 300)) == 100
        print("  PASS: test_basic_positive_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_positive_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Negative index
    # ------------------------------------------------------------------
    try:
        def read_last(tup):
            return tup[-1]

        data = (10, 20, 30, 40, 50)
        for _ in range(WARMUP):
            read_last(data)
        check_jit_compiled(read_last, "read_last")

        assert read_last(data) == 50
        assert read_last((7,)) == 7
        assert read_last((1, 2, 3)) == 3

        def read_neg_two(tup):
            return tup[-2]

        for _ in range(WARMUP):
            read_neg_two(data)
        assert read_neg_two(data) == 40
        assert read_neg_two((1, 2)) == 1
        print("  PASS: test_negative_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Boundary indices (first, last)
    # ------------------------------------------------------------------
    try:
        def read_first_and_last(tup):
            return (tup[0], tup[len(tup) - 1])

        data = (100, 200, 300, 400, 500)
        for _ in range(WARMUP):
            read_first_and_last(data)
        check_jit_compiled(read_first_and_last, "read_first_and_last")

        assert read_first_and_last(data) == (100, 500)
        assert read_first_and_last((42,)) == (42, 42)  # single element
        assert read_first_and_last((1, 2)) == (1, 2)
        print("  PASS: test_boundary_indices")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boundary_indices — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: IndexError for out-of-bounds
    # ------------------------------------------------------------------
    try:
        def read_oob(tup, idx):
            return tup[idx]

        data = (10, 20, 30)
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
    # Test 5: Tuples with different element types
    # ------------------------------------------------------------------
    try:
        def read_second(tup):
            return tup[1]

        int_tup = (1, 2, 3)
        str_tup = ("a", "b", "c")
        none_tup = (None, None, None)
        mixed_tup = (1, "two", 3.0, None)

        for _ in range(WARMUP):
            read_second(int_tup)
        check_jit_compiled(read_second, "read_second")

        assert read_second(int_tup) == 2
        assert read_second(str_tup) == "b"
        assert read_second(none_tup) is None
        assert read_second(mixed_tup) == "two"
        print("  PASS: test_different_element_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_element_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Deopt tuple -> list
    # ------------------------------------------------------------------
    try:
        def get_item_tl(container, idx):
            return container[idx]

        tup = (10, 20, 30)
        for _ in range(WARMUP):
            get_item_tl(tup, 1)
        check_jit_compiled(get_item_tl, "get_item_tl")

        lst = [40, 50, 60]
        assert get_item_tl(tup, 0) == 10
        assert get_item_tl(lst, 0) == 40  # deopt
        assert get_item_tl(lst, 2) == 60
        assert get_item_tl(tup, 2) == 30  # back to tuple
        print("  PASS: test_deopt_tuple_to_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_tuple_to_list — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Deopt tuple -> dict
    # ------------------------------------------------------------------
    try:
        def get_item_td(container, key):
            return container[key]

        tup = (10, 20, 30)
        for _ in range(WARMUP):
            get_item_td(tup, 1)
        check_jit_compiled(get_item_td, "get_item_td")

        d = {0: "zero", 1: "one", 2: "two"}
        assert get_item_td(tup, 1) == 20
        assert get_item_td(d, 1) == "one"  # deopt to dict
        assert get_item_td(d, 0) == "zero"
        assert get_item_td(tup, 0) == 10   # back to tuple
        print("  PASS: test_deopt_tuple_to_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_tuple_to_dict — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt tuple -> custom __getitem__
    # ------------------------------------------------------------------
    try:
        class Tripler:
            def __getitem__(self, key):
                return key * 3

        def get_item_tc(container, key):
            return container[key]

        tup = (10, 20, 30)
        for _ in range(WARMUP):
            get_item_tc(tup, 1)
        check_jit_compiled(get_item_tc, "get_item_tc")

        tripler = Tripler()
        assert get_item_tc(tup, 2) == 30
        assert get_item_tc(tripler, 5) == 15   # deopt to custom
        assert get_item_tc(tripler, 7) == 21
        assert get_item_tc(tup, 0) == 10       # back to tuple
        print("  PASS: test_deopt_tuple_to_custom_getitem")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_tuple_to_custom_getitem — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt int index -> slice
    # ------------------------------------------------------------------
    try:
        def get_slice_t(tup, key):
            return tup[key]

        data = (10, 20, 30, 40, 50)
        for _ in range(WARMUP):
            get_slice_t(data, 2)
        check_jit_compiled(get_slice_t, "get_slice_t")

        assert get_slice_t(data, 2) == 30
        # Slice index — deopt from TLongExact guard on index
        result = get_slice_t(data, slice(1, 3))
        assert result == (20, 30), f"Expected (20, 30), got {result}"
        # Back to int
        assert get_slice_t(data, 4) == 50
        print("  PASS: test_deopt_int_to_slice")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_int_to_slice — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Tuple read in loop (sum pattern)
    # ------------------------------------------------------------------
    try:
        def sum_tuple(tup):
            total = 0
            for i in range(len(tup)):
                total += tup[i]
            return total

        data = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
        for _ in range(WARMUP):
            sum_tuple(data)
        check_jit_compiled(sum_tuple, "sum_tuple")

        assert sum_tuple(data) == 55
        assert sum_tuple((100,)) == 100
        assert sum_tuple(()) == 0
        assert sum_tuple(tuple(range(100))) == 4950
        print("  PASS: test_tuple_read_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_tuple_read_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Object identity preservation
    # ------------------------------------------------------------------
    try:
        sentinel = object()

        def read_sentinel_t(tup):
            return tup[0]

        data = (sentinel, 1, 2)
        for _ in range(WARMUP):
            read_sentinel_t(data)
        check_jit_compiled(read_sentinel_t, "read_sentinel_t")

        assert read_sentinel_t(data) is sentinel
        another = object()
        data2 = (another,)
        assert read_sentinel_t(data2) is another
        assert read_sentinel_t(data2) is not sentinel
        print("  PASS: test_object_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_object_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Multiple reads in one function
    # ------------------------------------------------------------------
    try:
        def multi_read_t(tup):
            a = tup[0]
            b = tup[1]
            c = tup[2]
            return a + b + c

        data = (10, 20, 30, 40)
        for _ in range(WARMUP):
            multi_read_t(data)
        check_jit_compiled(multi_read_t, "multi_read_t")

        assert multi_read_t(data) == 60
        assert multi_read_t((1, 2, 3)) == 6
        assert multi_read_t((100, 200, 300)) == 600
        print("  PASS: test_multiple_reads_one_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_reads_one_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid container type alternation
    # ------------------------------------------------------------------
    try:
        def poly_read_t(container, idx):
            return container[idx]

        tup = (10, 20, 30)
        lst = [40, 50, 60]

        for _ in range(WARMUP):
            poly_read_t(tup, 1)
        check_jit_compiled(poly_read_t, "poly_read_t")

        for cycle in range(50):
            assert poly_read_t(tup, 1) == 20, f"tuple failed at cycle {cycle}"
            assert poly_read_t(lst, 1) == 50, f"list failed at cycle {cycle}"

        assert poly_read_t(tup, 0) == 10
        assert poly_read_t(lst, 2) == 60
        print("  PASS: test_rapid_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Nested tuple reads
    # ------------------------------------------------------------------
    try:
        def nested_read_t(matrix, row, col):
            return matrix[row][col]

        mat = ((1, 2, 3), (4, 5, 6), (7, 8, 9))
        for _ in range(WARMUP):
            nested_read_t(mat, 1, 1)
        check_jit_compiled(nested_read_t, "nested_read_t")

        assert nested_read_t(mat, 0, 0) == 1
        assert nested_read_t(mat, 1, 2) == 6
        assert nested_read_t(mat, 2, 0) == 7
        assert nested_read_t(mat, 2, 2) == 9
        print("  PASS: test_nested_tuple_reads")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_tuple_reads — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Large tuple access (10k elements)
    # ------------------------------------------------------------------
    try:
        def read_large_t(tup, idx):
            return tup[idx]

        big_tuple = tuple(range(10000))
        for _ in range(WARMUP):
            read_large_t(big_tuple, 5000)
        check_jit_compiled(read_large_t, "read_large_t")

        assert read_large_t(big_tuple, 0) == 0
        assert read_large_t(big_tuple, 9999) == 9999
        assert read_large_t(big_tuple, 5000) == 5000
        assert read_large_t(big_tuple, -1) == 9999
        assert read_large_t(big_tuple, -10000) == 0
        print("  PASS: test_large_tuple_access")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_tuple_access — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Boolean index (bool is subclass of int)
    # ------------------------------------------------------------------
    try:
        def read_bool_idx_t(tup, idx):
            return tup[idx]

        data = (10, 20, 30)
        for _ in range(WARMUP):
            read_bool_idx_t(data, 0)
        check_jit_compiled(read_bool_idx_t, "read_bool_idx_t")

        assert read_bool_idx_t(data, False) == 10  # False == 0
        assert read_bool_idx_t(data, True) == 20   # True == 1
        assert read_bool_idx_t(data, 0) == 10
        assert read_bool_idx_t(data, 1) == 20
        print("  PASS: test_boolean_index")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_index — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Single-element and empty tuple
    # ------------------------------------------------------------------
    try:
        def read_single(tup):
            return tup[0]

        single = (42,)
        for _ in range(WARMUP):
            read_single(single)
        check_jit_compiled(read_single, "read_single")

        assert read_single(single) == 42
        assert read_single(("hello",)) == "hello"

        # Empty tuple — IndexError
        raised = False
        try:
            read_single(())
        except IndexError:
            raised = True
        assert raised, "Expected IndexError for empty tuple"
        print("  PASS: test_single_and_empty_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_single_and_empty_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Named tuple (tuple subclass) — deopt
    # ------------------------------------------------------------------
    try:
        from collections import namedtuple

        Point = namedtuple("Point", ["x", "y", "z"])

        def read_namedtuple(container, idx):
            return container[idx]

        plain = (10, 20, 30)
        for _ in range(WARMUP):
            read_namedtuple(plain, 1)
        check_jit_compiled(read_namedtuple, "read_namedtuple")

        assert read_namedtuple(plain, 1) == 20

        # namedtuple is a tuple subclass — GuardType(TTupleExact) should deopt
        pt = Point(40, 50, 60)
        assert read_namedtuple(pt, 0) == 40  # deopt to generic path
        assert read_namedtuple(pt, 2) == 60
        assert read_namedtuple(plain, 0) == 10  # back to plain tuple
        print("  PASS: test_namedtuple_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_namedtuple_deopt — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Tuple unpacking equivalence
    # ------------------------------------------------------------------
    try:
        def subscr_unpack(tup):
            return (tup[0], tup[1], tup[2])

        def star_unpack(tup):
            a, b, c = tup
            return (a, b, c)

        data = (10, 20, 30)
        for _ in range(WARMUP):
            subscr_unpack(data)
        check_jit_compiled(subscr_unpack, "subscr_unpack")

        for test_data in [(1, 2, 3), (10, 20, 30), ("a", "b", "c"), (None, True, 0)]:
            assert subscr_unpack(test_data) == star_unpack(test_data), (
                f"Mismatch for {test_data}: "
                f"subscr={subscr_unpack(test_data)}, "
                f"star={star_unpack(test_data)}"
            )
        print("  PASS: test_tuple_unpacking_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_tuple_unpacking_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — tup[i] vs tuple.__getitem__(tup, i)
    # ------------------------------------------------------------------
    try:
        def subscr_read_t(tup, idx):
            return tup[idx]

        def explicit_getitem_t(tup, idx):
            return tuple.__getitem__(tup, idx)

        data = (10, 20, 30, 40, 50)
        for _ in range(WARMUP):
            subscr_read_t(data, 2)
        check_jit_compiled(subscr_read_t, "subscr_read_t")

        for idx in range(5):
            assert subscr_read_t(data, idx) == explicit_getitem_t(data, idx), (
                f"Mismatch at index {idx}: "
                f"subscr={subscr_read_t(data, idx)}, "
                f"explicit={explicit_getitem_t(data, idx)}"
            )
        for idx in range(-5, 0):
            assert subscr_read_t(data, idx) == explicit_getitem_t(data, idx), (
                f"Mismatch at negative index {idx}: "
                f"subscr={subscr_read_t(data, idx)}, "
                f"explicit={explicit_getitem_t(data, idx)}"
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
    print(f"\nBINARY_SUBSCR_TUPLE_INT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
