#!/usr/bin/env python3
"""
test_unpack_sequence_list.py — Correctness and deopt tests for
UNPACK_SEQUENCE_LIST specialisation.

Targets: UNPACK_SEQUENCE_LIST.

UNPACK_SEQUENCE_LIST specialises unpacking operations (a, b, c = some_list)
when the right-hand side is a list. Instead of going through the generic
UNPACK_SEQUENCE path (which must handle any iterable), the specialisation
directly reads list elements by index, avoiding iterator creation overhead.

The adaptive specialiser emits UNPACK_SEQUENCE_LIST after observing repeated
unpacking of list objects with a consistent element count.

Deopt triggers:
  - Right-hand side is not a list (tuple, generator, custom iterable)
  - List length does not match the number of unpack targets
  - Right-hand side is a list subclass

Tests cover:
  - Basic 2-element list unpack
  - Basic 3-element list unpack
  - Single-element list unpack
  - Large list unpack (5 elements)
  - List with different element types
  - List with None elements
  - Nested list unpack (outer only)
  - Deopt: switch to tuple
  - Deopt: switch to generator
  - Deopt: switch to custom iterable
  - List unpack in loop
  - List unpack with mutation before unpack
  - ValueError: too many values
  - ValueError: not enough values
  - Identity preservation through unpack
  - List unpack with starred assignment
  - Deopt: switch to list subclass
  - Rapid type alternation (list vs tuple)
  - List unpack from function return
  - Unpack equivalence: manual index vs unpack

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (ValueError for wrong length)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_unpack_sequence_list.py
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
    print("=== UNPACK_SEQUENCE_LIST Correctness & Deopt Tests ===")
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
    # Test 1: Basic 2-element list unpack
    # ------------------------------------------------------------------ #
    try:
        def unpack_two(lst):
            a, b = lst
            return (a, b)

        data = [10, 20]
        for _ in range(WARMUP):
            unpack_two(data)

        check_jit_compiled(unpack_two, "unpack_two")
        result = unpack_two(data)
        assert result == (10, 20), f"Expected (10, 20), got {result}"
        assert unpack_two([99, 88]) == (99, 88)
        print("  PASS: test_basic_2_element_unpack")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_2_element_unpack — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Basic 3-element list unpack
    # ------------------------------------------------------------------ #
    try:
        def unpack_three(lst):
            a, b, c = lst
            return (a, b, c)

        data = [1, 2, 3]
        for _ in range(WARMUP):
            unpack_three(data)

        check_jit_compiled(unpack_three, "unpack_three")
        result = unpack_three(data)
        assert result == (1, 2, 3), f"Expected (1, 2, 3), got {result}"
        print("  PASS: test_basic_3_element_unpack")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_3_element_unpack — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Single-element list unpack
    # ------------------------------------------------------------------ #
    try:
        def unpack_one(lst):
            a, = lst
            return a

        data = [42]
        for _ in range(WARMUP):
            unpack_one(data)

        check_jit_compiled(unpack_one, "unpack_one")
        result = unpack_one(data)
        assert result == 42, f"Expected 42, got {result}"
        print("  PASS: test_single_element_unpack")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_single_element_unpack — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Large list unpack (5 elements)
    # ------------------------------------------------------------------ #
    try:
        def unpack_five(lst):
            a, b, c, d, e = lst
            return (a, b, c, d, e)

        data = [10, 20, 30, 40, 50]
        for _ in range(WARMUP):
            unpack_five(data)

        check_jit_compiled(unpack_five, "unpack_five")
        result = unpack_five(data)
        assert result == (10, 20, 30, 40, 50), f"Expected (10,20,30,40,50), got {result}"
        print("  PASS: test_large_list_unpack")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_list_unpack — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: List with different element types
    # ------------------------------------------------------------------ #
    try:
        def unpack_mixed(lst):
            a, b, c = lst
            return (a, b, c)

        data = [42, "hello", 3.14]
        for _ in range(WARMUP):
            unpack_mixed(data)

        check_jit_compiled(unpack_mixed, "unpack_mixed")
        a, b, c = unpack_mixed(data)
        assert a == 42
        assert b == "hello"
        assert c == 3.14
        print("  PASS: test_different_element_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_element_types — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: List with None elements
    # ------------------------------------------------------------------ #
    try:
        def unpack_nones(lst):
            a, b, c = lst
            return (a, b, c)

        data = [None, None, None]
        for _ in range(WARMUP):
            unpack_nones(data)

        check_jit_compiled(unpack_nones, "unpack_nones")
        result = unpack_nones(data)
        assert result == (None, None, None)
        assert all(x is None for x in result)
        print("  PASS: test_none_elements")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_none_elements — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Nested list unpack (outer only)
    # ------------------------------------------------------------------ #
    try:
        def unpack_nested(lst):
            a, b = lst
            return (a, b)

        data = [[1, 2], [3, 4]]
        for _ in range(WARMUP):
            unpack_nested(data)

        check_jit_compiled(unpack_nested, "unpack_nested")
        a, b = unpack_nested(data)
        assert a == [1, 2]
        assert b == [3, 4]
        # Inner lists should be the same objects (identity preservation)
        assert a is data[0]
        assert b is data[1]
        print("  PASS: test_nested_list_unpack")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_list_unpack — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: Deopt — switch to tuple
    # ------------------------------------------------------------------ #
    try:
        def unpack_deopt_tuple(seq):
            a, b = seq
            return (a, b)

        lst = [10, 20]
        for _ in range(WARMUP):
            unpack_deopt_tuple(lst)

        check_jit_compiled(unpack_deopt_tuple, "unpack_deopt_tuple")
        assert unpack_deopt_tuple(lst) == (10, 20)

        # Switch to tuple — should deopt
        tup = (30, 40)
        result = unpack_deopt_tuple(tup)
        assert result == (30, 40), f"Tuple deopt: expected (30, 40), got {result}"
        print("  PASS: test_deopt_switch_to_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_switch_to_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Deopt — switch to generator
    # ------------------------------------------------------------------ #
    try:
        def unpack_deopt_gen(seq):
            a, b = seq
            return (a, b)

        lst = [5, 6]
        for _ in range(WARMUP):
            unpack_deopt_gen(lst)

        check_jit_compiled(unpack_deopt_gen, "unpack_deopt_gen")
        assert unpack_deopt_gen(lst) == (5, 6)

        # Switch to generator — should deopt
        def gen():
            yield 7
            yield 8

        result = unpack_deopt_gen(gen())
        assert result == (7, 8), f"Generator deopt: expected (7, 8), got {result}"
        print("  PASS: test_deopt_switch_to_generator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_switch_to_generator — {e}")
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

        def unpack_deopt_custom(seq):
            a, b = seq
            return (a, b)

        lst = [100, 200]
        for _ in range(WARMUP):
            unpack_deopt_custom(lst)

        check_jit_compiled(unpack_deopt_custom, "unpack_deopt_custom")
        assert unpack_deopt_custom(lst) == (100, 200)

        # Switch to custom iterable — should deopt
        ci = CustomIter(300, 400)
        result = unpack_deopt_custom(ci)
        assert result == (300, 400), f"Custom deopt: expected (300, 400), got {result}"
        print("  PASS: test_deopt_switch_to_custom_iterable")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_switch_to_custom_iterable — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: List unpack in loop
    # ------------------------------------------------------------------ #
    try:
        def sum_unpacked(pairs):
            total = 0
            for pair in pairs:
                a, b = pair
                total += a + b
            return total

        data = [[i, i * 2] for i in range(100)]
        for _ in range(WARMUP):
            sum_unpacked(data[:1])

        check_jit_compiled(sum_unpacked, "sum_unpacked")
        result = sum_unpacked(data)
        # sum of i + 2i = 3i for i in 0..99 = 3 * (99*100/2) = 14850
        assert result == 14850, f"Expected 14850, got {result}"
        print("  PASS: test_list_unpack_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_unpack_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: List unpack with mutation before unpack
    # ------------------------------------------------------------------ #
    try:
        def unpack_after_mutate(lst):
            lst[0] = 999
            a, b = lst
            return (a, b)

        data = [1, 2]
        for _ in range(WARMUP):
            data[0] = 1  # Reset
            unpack_after_mutate(data)

        check_jit_compiled(unpack_after_mutate, "unpack_after_mutate")
        data = [1, 2]
        result = unpack_after_mutate(data)
        assert result == (999, 2), f"Expected (999, 2), got {result}"
        assert data[0] == 999, "Mutation should be visible"
        print("  PASS: test_list_unpack_with_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_unpack_with_mutation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: ValueError — too many values
    # ------------------------------------------------------------------ #
    try:
        def unpack_too_many(lst):
            a, b = lst
            return (a, b)

        good = [1, 2]
        for _ in range(WARMUP):
            unpack_too_many(good)

        check_jit_compiled(unpack_too_many, "unpack_too_many")

        caught = False
        try:
            unpack_too_many([1, 2, 3])
        except ValueError as ex:
            caught = True
            assert "too many values to unpack" in str(ex)
        assert caught, "Should raise ValueError for too many values"
        print("  PASS: test_valueerror_too_many_values")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_valueerror_too_many_values — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: ValueError — not enough values
    # ------------------------------------------------------------------ #
    try:
        def unpack_too_few(lst):
            a, b, c = lst
            return (a, b, c)

        good = [1, 2, 3]
        for _ in range(WARMUP):
            unpack_too_few(good)

        check_jit_compiled(unpack_too_few, "unpack_too_few")

        caught = False
        try:
            unpack_too_few([1, 2])
        except ValueError as ex:
            caught = True
            assert "not enough values to unpack" in str(ex)
        assert caught, "Should raise ValueError for not enough values"
        print("  PASS: test_valueerror_not_enough_values")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_valueerror_not_enough_values — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Identity preservation through unpack
    # ------------------------------------------------------------------ #
    try:
        def unpack_identity(lst):
            a, b = lst
            return (a, b)

        sentinel_a = object()
        sentinel_b = object()
        data = [sentinel_a, sentinel_b]

        for _ in range(WARMUP):
            unpack_identity(data)

        check_jit_compiled(unpack_identity, "unpack_identity")
        a, b = unpack_identity(data)
        assert a is sentinel_a, "Identity of first element must be preserved"
        assert b is sentinel_b, "Identity of second element must be preserved"
        print("  PASS: test_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: List unpack with starred assignment
    # ------------------------------------------------------------------ #
    try:
        def unpack_starred(lst):
            a, *rest, z = lst
            return (a, rest, z)

        data = [1, 2, 3, 4, 5]
        for _ in range(WARMUP):
            unpack_starred(data)

        check_jit_compiled(unpack_starred, "unpack_starred")
        a, rest, z = unpack_starred(data)
        assert a == 1
        assert rest == [2, 3, 4]
        assert z == 5

        # Minimal case: 2 elements with starred
        a2, rest2, z2 = unpack_starred([10, 20])
        assert a2 == 10
        assert rest2 == []
        assert z2 == 20
        print("  PASS: test_starred_assignment")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_starred_assignment — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Deopt — switch to list subclass
    # ------------------------------------------------------------------ #
    try:
        class MyList(list):
            pass

        def unpack_subclass(lst):
            a, b = lst
            return (a, b)

        plain = [1, 2]
        for _ in range(WARMUP):
            unpack_subclass(plain)

        check_jit_compiled(unpack_subclass, "unpack_subclass")
        assert unpack_subclass(plain) == (1, 2)

        # Switch to list subclass — may deopt
        sub = MyList([3, 4])
        result = unpack_subclass(sub)
        assert result == (3, 4), f"Subclass deopt: expected (3, 4), got {result}"
        print("  PASS: test_deopt_list_subclass")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_list_subclass — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Rapid type alternation (list vs tuple)
    # ------------------------------------------------------------------ #
    try:
        def unpack_alternating(seq):
            a, b = seq
            return (a, b)

        lst = [1, 2]
        for _ in range(WARMUP):
            unpack_alternating(lst)

        check_jit_compiled(unpack_alternating, "unpack_alternating")

        tup = (3, 4)
        ok = True
        for i in range(50):
            rl = unpack_alternating(lst)
            rt = unpack_alternating(tup)
            if rl != (1, 2):
                print(f"  FAIL: list iteration {i}: expected (1,2), got {rl}")
                ok = False
                break
            if rt != (3, 4):
                print(f"  FAIL: tuple iteration {i}: expected (3,4), got {rt}")
                ok = False
                break

        if ok:
            print("  PASS: test_rapid_type_alternation")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: List unpack from function return
    # ------------------------------------------------------------------ #
    try:
        def make_list():
            return [10, 20, 30]

        def unpack_from_return():
            a, b, c = make_list()
            return (a, b, c)

        for _ in range(WARMUP):
            unpack_from_return()

        check_jit_compiled(unpack_from_return, "unpack_from_return")
        result = unpack_from_return()
        assert result == (10, 20, 30), f"Expected (10, 20, 30), got {result}"
        print("  PASS: test_unpack_from_function_return")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_unpack_from_function_return — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 20: Unpack equivalence — manual index vs unpack
    # ------------------------------------------------------------------ #
    try:
        def via_unpack(lst):
            a, b, c = lst
            return (a, b, c)

        def via_index(lst):
            return (lst[0], lst[1], lst[2])

        data = [7, 14, 21]
        for _ in range(WARMUP):
            via_unpack(data)

        check_jit_compiled(via_unpack, "via_unpack")

        for vals in [[0, 0, 0], [1, 2, 3], [-1, -2, -3], [100, 200, 300]]:
            ur = via_unpack(vals)
            ir = via_index(vals)
            assert ur == ir, (
                f"Mismatch for {vals}: unpack={ur}, index={ir}"
            )

        print("  PASS: test_unpack_vs_index_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_unpack_vs_index_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Summary
    # ------------------------------------------------------------------ #
    print()
    print(f"UNPACK_SEQUENCE_LIST: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
