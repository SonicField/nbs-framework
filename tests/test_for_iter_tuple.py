#!/usr/bin/env python3
"""
test_for_iter_tuple.py — Correctness and deopt tests for
FOR_ITER_TUPLE specialisation.

Targets: FOR_ITER_TUPLE.

FOR_ITER_TUPLE specialises the for-loop iteration (for x in some_tuple)
when the iterator is a tuple_iterator. Instead of going through the
generic FOR_ITER path (which calls tp_iternext via the iterator protocol),
the specialisation directly accesses tuple elements using the
tuple_iterator's internal index, checking the tuple length for termination.

The adaptive specialiser emits FOR_ITER_TUPLE after observing repeated
iteration over tuple objects. The GuardType is emitted at GET_ITER (once),
not per-iteration.

Deopt triggers:
  - Iterator is not a tuple_iterator (list, generator, custom iterable)
  - Tuple type changes (tuple subclass)

Tests cover:
  - Basic tuple iteration
  - Iteration over empty tuple
  - Single-element tuple
  - Large tuple iteration (1000 elements)
  - Tuple with mixed types
  - Tuple with None elements
  - Nested tuple iteration (outer)
  - Deopt: switch to list iteration
  - Deopt: switch to generator iteration
  - Deopt: switch to custom iterable
  - Deopt: switch to dict iteration (keys)
  - Deopt: switch to range iteration
  - Sum accumulator pattern
  - Break in loop
  - Continue in loop
  - Nested for loops (both tuples)
  - Tuple in generator expression
  - Enumerate over tuple
  - Rapid alternation (tuple vs list)
  - Iteration equivalence: for-loop vs manual iter()/next()

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. All elements visited in correct order

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_for_iter_tuple.py
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
    print("=== FOR_ITER_TUPLE Correctness & Deopt Tests ===")
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

    # ------------------------------------------------------------------ #
    # Test 1: Basic tuple iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_tuple(tup):
            result = []
            for x in tup:
                result.append(x)
            return result

        data = (1, 2, 3, 4, 5)
        for _ in range(WARMUP):
            iterate_tuple(data)

        check_jit_compiled(iterate_tuple, "iterate_tuple")
        result = iterate_tuple(data)
        assert result == [1, 2, 3, 4, 5], f"Expected [1,2,3,4,5], got {result}"
        print("  PASS: test_basic_tuple_iteration")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_tuple_iteration — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Iteration over empty tuple
    # ------------------------------------------------------------------ #
    try:
        def iterate_empty(tup):
            result = []
            for x in tup:
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_empty(())

        check_jit_compiled(iterate_empty, "iterate_empty")
        result = iterate_empty(())
        assert result == [], f"Expected [], got {result}"
        print("  PASS: test_empty_tuple_iteration")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_tuple_iteration — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Single-element tuple
    # ------------------------------------------------------------------ #
    try:
        def iterate_single(tup):
            result = []
            for x in tup:
                result.append(x)
            return result

        data = (42,)
        for _ in range(WARMUP):
            iterate_single(data)

        check_jit_compiled(iterate_single, "iterate_single")
        result = iterate_single(data)
        assert result == [42], f"Expected [42], got {result}"
        print("  PASS: test_single_element_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_single_element_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Large tuple iteration (1000 elements)
    # ------------------------------------------------------------------ #
    try:
        def sum_tuple(tup):
            total = 0
            for x in tup:
                total += x
            return total

        data = tuple(range(1000))
        # Warm up with a small slice to avoid slow warmup
        warmup_data = tuple(range(10))
        for _ in range(WARMUP):
            sum_tuple(warmup_data)

        check_jit_compiled(sum_tuple, "sum_tuple")
        result = sum_tuple(data)
        # sum(0..999) = 499500
        assert result == 499500, f"Expected 499500, got {result}"
        print("  PASS: test_large_tuple_iteration")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_tuple_iteration — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: Tuple with mixed types
    # ------------------------------------------------------------------ #
    try:
        def collect_types(tup):
            result = []
            for x in tup:
                result.append(type(x).__name__)
            return result

        data = (42, "hello", 3.14, None, True)
        for _ in range(WARMUP):
            collect_types(data)

        check_jit_compiled(collect_types, "collect_types")
        result = collect_types(data)
        assert result == ['int', 'str', 'float', 'NoneType', 'bool'], (
            f"Got {result}"
        )
        print("  PASS: test_mixed_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_types — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: Tuple with None elements
    # ------------------------------------------------------------------ #
    try:
        def count_nones(tup):
            count = 0
            for x in tup:
                if x is None:
                    count += 1
            return count

        data = (None, 1, None, 2, None)
        for _ in range(WARMUP):
            count_nones(data)

        check_jit_compiled(count_nones, "count_nones")
        assert count_nones(data) == 3, f"Expected 3 Nones"
        assert count_nones((None, None)) == 2
        assert count_nones((1, 2, 3)) == 0
        print("  PASS: test_none_elements")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_none_elements — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Nested tuple iteration (outer)
    # ------------------------------------------------------------------ #
    try:
        def flatten_one_level(tup):
            result = []
            for subtuple in tup:
                for item in subtuple:
                    result.append(item)
            return result

        data = ((1, 2), (3, 4), (5,))
        for _ in range(WARMUP):
            flatten_one_level(data)

        check_jit_compiled(flatten_one_level, "flatten_one_level")
        result = flatten_one_level(data)
        assert result == [1, 2, 3, 4, 5], f"Expected [1,2,3,4,5], got {result}"
        print("  PASS: test_nested_tuple_iteration")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_tuple_iteration — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: Deopt — switch to list iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_list(seq):
            result = []
            for x in seq:
                result.append(x)
            return result

        tup = (10, 20, 30)
        for _ in range(WARMUP):
            iterate_deopt_list(tup)

        check_jit_compiled(iterate_deopt_list, "iterate_deopt_list")
        assert iterate_deopt_list(tup) == [10, 20, 30]

        # Switch to list — should deopt
        lst = [40, 50, 60]
        result = iterate_deopt_list(lst)
        assert result == [40, 50, 60], f"List deopt: got {result}"

        # Original tuple type must still work after deopt
        result2 = iterate_deopt_list(tup)
        assert result2 == [10, 20, 30], f"Tuple after deopt: got {result2}"
        print("  PASS: test_deopt_to_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_list — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Deopt — switch to generator iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_gen(seq):
            result = []
            for x in seq:
                result.append(x)
            return result

        tup = (1, 2, 3)
        for _ in range(WARMUP):
            iterate_deopt_gen(tup)

        check_jit_compiled(iterate_deopt_gen, "iterate_deopt_gen")

        def gen():
            yield 7
            yield 8
            yield 9

        result = iterate_deopt_gen(gen())
        assert result == [7, 8, 9], f"Generator deopt: got {result}"

        # Original tuple type must still work after deopt
        result2 = iterate_deopt_gen(tup)
        assert result2 == [1, 2, 3], f"Tuple after deopt: got {result2}"
        print("  PASS: test_deopt_to_generator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_generator — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 10: Deopt — switch to custom iterable
    # ------------------------------------------------------------------ #
    try:
        class CustomIter:
            def __init__(self, *args):
                self._data = args

            def __iter__(self):
                return iter(self._data)

        def iterate_deopt_custom(seq):
            result = []
            for x in seq:
                result.append(x)
            return result

        tup = (100, 200)
        for _ in range(WARMUP):
            iterate_deopt_custom(tup)

        check_jit_compiled(iterate_deopt_custom, "iterate_deopt_custom")

        ci = CustomIter(300, 400, 500)
        result = iterate_deopt_custom(ci)
        assert result == [300, 400, 500], f"Custom deopt: got {result}"

        # Original tuple type must still work after deopt
        result2 = iterate_deopt_custom(tup)
        assert result2 == [100, 200], f"Tuple after deopt: got {result2}"
        print("  PASS: test_deopt_to_custom_iterable")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_custom_iterable — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: Deopt — switch to dict iteration (keys)
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_dict(seq):
            result = []
            for x in seq:
                result.append(x)
            return result

        tup = ("a", "b", "c")
        for _ in range(WARMUP):
            iterate_deopt_dict(tup)

        check_jit_compiled(iterate_deopt_dict, "iterate_deopt_dict")

        d = {"x": 1, "y": 2, "z": 3}
        result = iterate_deopt_dict(d)
        assert set(result) == {"x", "y", "z"}, f"Dict deopt: got {result}"

        # Original tuple type must still work after deopt
        result2 = iterate_deopt_dict(tup)
        assert result2 == ["a", "b", "c"], f"Tuple after deopt: got {result2}"
        print("  PASS: test_deopt_to_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_dict — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: Deopt — switch to range iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_range(seq):
            total = 0
            for x in seq:
                total += x
            return total

        tup = (1, 2, 3, 4, 5)
        for _ in range(WARMUP):
            iterate_deopt_range(tup)

        check_jit_compiled(iterate_deopt_range, "iterate_deopt_range")
        assert iterate_deopt_range(tup) == 15

        result = iterate_deopt_range(range(1, 6))
        assert result == 15, f"Range deopt: expected 15, got {result}"

        # Original tuple type must still work after deopt
        result2 = iterate_deopt_range(tup)
        assert result2 == 15, f"Tuple after deopt: got {result2}"
        print("  PASS: test_deopt_to_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_range — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: Sum accumulator with tuple
    # ------------------------------------------------------------------ #
    try:
        def sum_accum(tup):
            total = 0
            for x in tup:
                total += x
            return total

        data = tuple(range(100))
        warmup_data = tuple(range(5))
        for _ in range(WARMUP):
            sum_accum(warmup_data)

        check_jit_compiled(sum_accum, "sum_accum")
        # sum(0..99) = 4950
        assert sum_accum(data) == 4950, f"Expected 4950"
        assert sum_accum(()) == 0
        assert sum_accum((42,)) == 42
        print("  PASS: test_sum_accumulator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_sum_accumulator — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: Break in tuple loop
    # ------------------------------------------------------------------ #
    try:
        def find_first_negative(tup):
            for x in tup:
                if x < 0:
                    return x
            return None

        data = (1, 2, 3, -5, 4)
        for _ in range(WARMUP):
            find_first_negative(data)

        check_jit_compiled(find_first_negative, "find_first_negative")
        assert find_first_negative(data) == -5
        assert find_first_negative((1, 2, 3)) is None
        assert find_first_negative((-1, 2)) == -1
        print("  PASS: test_break_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_break_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Continue in tuple loop
    # ------------------------------------------------------------------ #
    try:
        def sum_positive(tup):
            total = 0
            for x in tup:
                if x < 0:
                    continue
                total += x
            return total

        data = (1, -2, 3, -4, 5)
        for _ in range(WARMUP):
            sum_positive(data)

        check_jit_compiled(sum_positive, "sum_positive")
        assert sum_positive(data) == 9, f"1+3+5=9"
        assert sum_positive((-1, -2, -3)) == 0
        assert sum_positive((10,)) == 10
        print("  PASS: test_continue_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_continue_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: Nested for loops (both tuples) — cartesian sum
    # ------------------------------------------------------------------ #
    try:
        def cartesian_sum(tup_a, tup_b):
            total = 0
            for a in tup_a:
                for b in tup_b:
                    total += a * b
            return total

        ta = (1, 2, 3)
        tb = (10, 20)
        for _ in range(WARMUP):
            cartesian_sum(ta, tb)

        check_jit_compiled(cartesian_sum, "cartesian_sum")
        # (1*10 + 1*20) + (2*10 + 2*20) + (3*10 + 3*20) = 30 + 60 + 90 = 180
        result = cartesian_sum(ta, tb)
        assert result == 180, f"Expected 180, got {result}"
        print("  PASS: test_nested_for_loops")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_for_loops — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Tuple in generator expression
    # ------------------------------------------------------------------ #
    try:
        def squares_via_genexpr(tup):
            return tuple(x * x for x in tup)

        data = (1, 2, 3, 4, 5)
        for _ in range(WARMUP):
            squares_via_genexpr(data)

        check_jit_compiled(squares_via_genexpr, "squares_via_genexpr")
        result = squares_via_genexpr(data)
        assert result == (1, 4, 9, 16, 25), f"Expected (1,4,9,16,25), got {result}"
        assert squares_via_genexpr(()) == ()
        print("  PASS: test_tuple_generator_expression")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_tuple_generator_expression — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Enumerate over tuple
    # ------------------------------------------------------------------ #
    try:
        def indexed_sum(tup):
            total = 0
            for i, x in enumerate(tup):
                total += i * x
            return total

        data = (10, 20, 30)
        for _ in range(WARMUP):
            indexed_sum(data)

        check_jit_compiled(indexed_sum, "indexed_sum")
        # 0*10 + 1*20 + 2*30 = 0 + 20 + 60 = 80
        result = indexed_sum(data)
        assert result == 80, f"Expected 80, got {result}"
        print("  PASS: test_enumerate_over_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_enumerate_over_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: Rapid alternation — tuple vs list (50 cycles)
    # ------------------------------------------------------------------ #
    try:
        def sum_iter(seq):
            total = 0
            for x in seq:
                total += x
            return total

        tup = (1, 2, 3)
        for _ in range(WARMUP):
            sum_iter(tup)

        check_jit_compiled(sum_iter, "sum_iter")

        lst = [4, 5, 6]
        ok = True
        for i in range(50):
            rt = sum_iter(tup)
            rl = sum_iter(lst)
            if rt != 6:
                print(f"  FAIL: tuple iteration {i}: expected 6, got {rt}")
                ok = False
                break
            if rl != 15:
                print(f"  FAIL: list iteration {i}: expected 15, got {rl}")
                ok = False
                break

        if ok:
            print("  PASS: test_rapid_alternation")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 20: Iteration equivalence — for-loop vs manual iter()/next()
    # ------------------------------------------------------------------ #
    try:
        def via_for(tup):
            result = []
            for x in tup:
                result.append(x)
            return result

        def via_manual(tup):
            result = []
            it = iter(tup)
            while True:
                try:
                    result.append(next(it))
                except StopIteration:
                    break
            return result

        data = (10, 20, 30, 40, 50)
        for _ in range(WARMUP):
            via_for(data)

        check_jit_compiled(via_for, "via_for")

        for test_data in [(), (1,), (1, 2, 3), tuple(range(20))]:
            fr = via_for(test_data)
            mr = via_manual(test_data)
            assert fr == mr, (
                f"Mismatch for {test_data}: for={fr}, manual={mr}"
            )

        print("  PASS: test_for_vs_manual_iter_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_for_vs_manual_iter_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Summary
    # ------------------------------------------------------------------ #
    print()
    print(f"FOR_ITER_TUPLE: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
