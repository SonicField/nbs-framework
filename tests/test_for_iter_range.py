#!/usr/bin/env python3
"""
test_for_iter_range.py — Correctness and deopt tests for
FOR_ITER_RANGE specialisation.

Targets: FOR_ITER_RANGE.

FOR_ITER_RANGE specialises the for-loop iteration (for x in range(...))
when the iterator is a range_iterator. Instead of going through the
generic FOR_ITER path (which calls tp_iternext via the iterator protocol),
the specialisation directly uses the range_iterator's internal int state,
incrementing by the step value and checking against the stop bound,
avoiding the overhead of creating intermediate Python int objects on
each iteration.

The adaptive specialiser emits FOR_ITER_RANGE after observing repeated
iteration over range objects.

Deopt triggers:
  - Iterator is not a range_iterator (list, tuple, generator, custom iterable)
  - Any non-range iterable passed to the specialised callsite

Tests cover:
  - Basic range iteration
  - Empty range (range(0))
  - Single-element range (range(1))
  - Large range iteration (10000 elements)
  - Range with start (range(5, 10))
  - Range with step (range(0, 20, 3))
  - Negative step (range(10, 0, -1))
  - Negative range (range(-5, 5))
  - Deopt: switch to list iteration
  - Deopt: switch to tuple iteration
  - Deopt: switch to generator iteration
  - Deopt: switch to custom iterable
  - Sum accumulator pattern
  - Break in range loop
  - Continue in range loop
  - Nested range loops (cartesian product sum)
  - Range in list comprehension
  - Enumerate over range
  - Rapid alternation (range vs list)
  - Equivalence: range for-loop vs manual iter()/next()

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. All elements visited in correct order

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_for_iter_range.py
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
    print("=== FOR_ITER_RANGE Correctness & Deopt Tests ===")
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
    # Test 1: Basic range iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_basic(n):
            total = 0
            for x in range(n):
                total += x
            return total

        for _ in range(WARMUP):
            iterate_range_basic(5)

        check_jit_compiled(iterate_range_basic, "iterate_range_basic")
        result = iterate_range_basic(5)
        # sum(0..4) = 0+1+2+3+4 = 10
        assert result == 10, f"Expected 10, got {result}"
        print("  PASS: test_basic_range_iteration")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_range_iteration — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Empty range
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_empty(n):
            count = 0
            for x in range(n):
                count += 1
            return count

        for _ in range(WARMUP):
            iterate_range_empty(0)

        check_jit_compiled(iterate_range_empty, "iterate_range_empty")
        result = iterate_range_empty(0)
        assert result == 0, f"Expected 0 iterations, got {result}"
        print("  PASS: test_empty_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_range — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Single-element range
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_single(n):
            result = []
            for x in range(n):
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_range_single(1)

        check_jit_compiled(iterate_range_single, "iterate_range_single")
        result = iterate_range_single(1)
        assert result == [0], f"Expected [0], got {result}"
        print("  PASS: test_single_element_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_single_element_range — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Large range
    # ------------------------------------------------------------------ #
    try:
        def sum_large_range(n):
            total = 0
            for x in range(n):
                total += x
            return total

        # Warmup with smaller range to avoid excessive time
        for _ in range(WARMUP):
            sum_large_range(10)

        check_jit_compiled(sum_large_range, "sum_large_range")
        result = sum_large_range(10000)
        # sum(0..9999) = 9999*10000/2 = 49995000
        assert result == 49995000, f"Expected 49995000, got {result}"
        print("  PASS: test_large_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_range — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: Range with start
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_start(start, stop):
            result = []
            for x in range(start, stop):
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_range_start(5, 10)

        check_jit_compiled(iterate_range_start, "iterate_range_start")
        result = iterate_range_start(5, 10)
        assert result == [5, 6, 7, 8, 9], f"Expected [5,6,7,8,9], got {result}"
        print("  PASS: test_range_with_start")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_range_with_start — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: Range with step
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_step(start, stop, step):
            result = []
            for x in range(start, stop, step):
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_range_step(0, 20, 3)

        check_jit_compiled(iterate_range_step, "iterate_range_step")
        result = iterate_range_step(0, 20, 3)
        assert result == [0, 3, 6, 9, 12, 15, 18], (
            f"Expected [0,3,6,9,12,15,18], got {result}"
        )
        print("  PASS: test_range_with_step")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_range_with_step — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Negative step
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_neg_step(start, stop, step):
            result = []
            for x in range(start, stop, step):
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_range_neg_step(10, 0, -1)

        check_jit_compiled(iterate_range_neg_step, "iterate_range_neg_step")
        result = iterate_range_neg_step(10, 0, -1)
        assert result == [10, 9, 8, 7, 6, 5, 4, 3, 2, 1], (
            f"Expected [10,9,8,...,1], got {result}"
        )
        print("  PASS: test_negative_step")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_step — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: Negative range
    # ------------------------------------------------------------------ #
    try:
        def iterate_range_negative(start, stop):
            result = []
            for x in range(start, stop):
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_range_negative(-5, 5)

        check_jit_compiled(iterate_range_negative, "iterate_range_negative")
        result = iterate_range_negative(-5, 5)
        assert result == [-5, -4, -3, -2, -1, 0, 1, 2, 3, 4], (
            f"Expected [-5,-4,...,4], got {result}"
        )
        print("  PASS: test_negative_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_negative_range — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Deopt — warm with range, switch to list iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_list(seq):
            total = 0
            for x in seq:
                total += x
            return total

        for _ in range(WARMUP):
            iterate_deopt_list(range(5))

        check_jit_compiled(iterate_deopt_list, "iterate_deopt_list")
        # Verify range still works after specialisation
        assert iterate_deopt_list(range(5)) == 10

        # Switch to list — should deopt
        result = iterate_deopt_list([10, 20, 30])
        assert result == 60, f"List deopt: expected 60, got {result}"
        # Verify range still works after deopt
        assert iterate_deopt_list(range(5)) == 10, "Range broken after list deopt"
        print("  PASS: test_deopt_to_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_list — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 10: Deopt — warm with range, switch to tuple iteration
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_tuple(seq):
            total = 0
            for x in seq:
                total += x
            return total

        for _ in range(WARMUP):
            iterate_deopt_tuple(range(5))

        check_jit_compiled(iterate_deopt_tuple, "iterate_deopt_tuple")
        assert iterate_deopt_tuple(range(5)) == 10

        # Switch to tuple — should deopt
        result = iterate_deopt_tuple((10, 20, 30))
        assert result == 60, f"Tuple deopt: expected 60, got {result}"
        # Verify range still works after deopt
        assert iterate_deopt_tuple(range(5)) == 10, "Range broken after tuple deopt"
        print("  PASS: test_deopt_to_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: Deopt — warm with range, switch to generator
    # ------------------------------------------------------------------ #
    try:
        def iterate_deopt_gen(seq):
            result = []
            for x in seq:
                result.append(x)
            return result

        for _ in range(WARMUP):
            iterate_deopt_gen(range(3))

        check_jit_compiled(iterate_deopt_gen, "iterate_deopt_gen")

        def gen():
            yield 7
            yield 8
            yield 9

        result = iterate_deopt_gen(gen())
        assert result == [7, 8, 9], f"Generator deopt: got {result}"
        # Verify range still works after deopt
        assert iterate_deopt_gen(range(3)) == [0, 1, 2], "Range broken after gen deopt"
        print("  PASS: test_deopt_to_generator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_generator — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: Deopt — warm with range, switch to custom iterable
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

        for _ in range(WARMUP):
            iterate_deopt_custom(range(3))

        check_jit_compiled(iterate_deopt_custom, "iterate_deopt_custom")

        ci = CustomIter(100, 200, 300)
        result = iterate_deopt_custom(ci)
        assert result == [100, 200, 300], f"Custom deopt: got {result}"
        # Verify range still works after deopt
        assert iterate_deopt_custom(range(3)) == [0, 1, 2], (
            "Range broken after custom deopt"
        )
        print("  PASS: test_deopt_to_custom_iterable")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_custom_iterable — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: Sum accumulator with range
    # ------------------------------------------------------------------ #
    try:
        def sum_accum_range(n):
            total = 0
            for x in range(n):
                total += x
            return total

        for _ in range(WARMUP):
            sum_accum_range(10)

        check_jit_compiled(sum_accum_range, "sum_accum_range")
        # sum(0..99) = 4950
        assert sum_accum_range(100) == 4950, f"Expected 4950"
        assert sum_accum_range(0) == 0
        assert sum_accum_range(1) == 0
        assert sum_accum_range(2) == 1
        print("  PASS: test_sum_accumulator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_sum_accumulator — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: Break in range loop
    # ------------------------------------------------------------------ #
    try:
        def find_first_gt5(n):
            for x in range(n):
                if x > 5:
                    return x
            return None

        for _ in range(WARMUP):
            find_first_gt5(10)

        check_jit_compiled(find_first_gt5, "find_first_gt5")
        assert find_first_gt5(10) == 6, f"Expected 6, got {find_first_gt5(10)}"
        assert find_first_gt5(5) is None, "range(5) has no element > 5"
        assert find_first_gt5(0) is None, "range(0) is empty"
        print("  PASS: test_break_in_range_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_break_in_range_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Continue in range loop — skip odds
    # ------------------------------------------------------------------ #
    try:
        def sum_even_range(n):
            total = 0
            for x in range(n):
                if x % 2 != 0:
                    continue
                total += x
            return total

        for _ in range(WARMUP):
            sum_even_range(10)

        check_jit_compiled(sum_even_range, "sum_even_range")
        # Even numbers in range(10): 0,2,4,6,8 => sum = 20
        result = sum_even_range(10)
        assert result == 20, f"Expected 20, got {result}"
        assert sum_even_range(0) == 0
        assert sum_even_range(1) == 0  # Only 0, which is even
        assert sum_even_range(3) == 2  # 0 + 2
        print("  PASS: test_continue_skip_odds")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_continue_skip_odds — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: Nested range loops — cartesian product sum
    # ------------------------------------------------------------------ #
    try:
        def cartesian_sum_range(na, nb):
            total = 0
            for a in range(na):
                for b in range(nb):
                    total += a * b
            return total

        for _ in range(WARMUP):
            cartesian_sum_range(3, 4)

        check_jit_compiled(cartesian_sum_range, "cartesian_sum_range")
        # sum(a*b for a in range(3) for b in range(4))
        # = sum over a in {0,1,2}: a * sum(b in {0,1,2,3}) = a * 6
        # = 0*6 + 1*6 + 2*6 = 18
        result = cartesian_sum_range(3, 4)
        assert result == 18, f"Expected 18, got {result}"
        assert cartesian_sum_range(0, 5) == 0, "Empty outer range"
        assert cartesian_sum_range(5, 0) == 0, "Empty inner range"
        print("  PASS: test_nested_range_loops")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_range_loops — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Range in list comprehension
    # ------------------------------------------------------------------ #
    try:
        def squares_range(n):
            return [x * x for x in range(n)]

        for _ in range(WARMUP):
            squares_range(5)

        check_jit_compiled(squares_range, "squares_range")
        result = squares_range(5)
        assert result == [0, 1, 4, 9, 16], f"Expected [0,1,4,9,16], got {result}"
        assert squares_range(0) == []
        assert squares_range(1) == [0]
        print("  PASS: test_range_list_comprehension")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_range_list_comprehension — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Enumerate over range
    # ------------------------------------------------------------------ #
    try:
        def indexed_sum_range(n):
            total = 0
            for i, x in enumerate(range(n)):
                total += i * x
            return total

        for _ in range(WARMUP):
            indexed_sum_range(5)

        check_jit_compiled(indexed_sum_range, "indexed_sum_range")
        # enumerate(range(5)) yields (0,0),(1,1),(2,2),(3,3),(4,4)
        # sum = 0*0 + 1*1 + 2*2 + 3*3 + 4*4 = 0+1+4+9+16 = 30
        result = indexed_sum_range(5)
        assert result == 30, f"Expected 30, got {result}"
        assert indexed_sum_range(0) == 0
        assert indexed_sum_range(1) == 0  # 0*0
        print("  PASS: test_enumerate_over_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_enumerate_over_range — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: Rapid alternation — range vs list (50 cycles)
    # ------------------------------------------------------------------ #
    try:
        def sum_iter_alt(seq):
            total = 0
            for x in seq:
                total += x
            return total

        for _ in range(WARMUP):
            sum_iter_alt(range(4))

        check_jit_compiled(sum_iter_alt, "sum_iter_alt")

        lst = [10, 20, 30]
        ok = True
        for i in range(50):
            rr = sum_iter_alt(range(4))
            rl = sum_iter_alt(lst)
            if rr != 6:
                print(f"  FAIL: range iteration {i}: expected 6, got {rr}")
                ok = False
                break
            if rl != 60:
                print(f"  FAIL: list iteration {i}: expected 60, got {rl}")
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
    # Test 20: Equivalence — range for-loop vs manual iter()/next()
    # ------------------------------------------------------------------ #
    try:
        def via_for_range(n):
            result = []
            for x in range(n):
                result.append(x)
            return result

        def via_manual_range(n):
            result = []
            it = iter(range(n))
            while True:
                try:
                    result.append(next(it))
                except StopIteration:
                    break
            return result

        for _ in range(WARMUP):
            via_for_range(5)

        check_jit_compiled(via_for_range, "via_for_range")

        for test_n in [0, 1, 5, 20, 100]:
            fr = via_for_range(test_n)
            mr = via_manual_range(test_n)
            assert fr == mr, (
                f"Mismatch for range({test_n}): for={fr}, manual={mr}"
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
    print(f"FOR_ITER_RANGE: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
