#!/usr/bin/env python3
"""
test_unpack_sequence_two_tuple.py — Correctness and deopt tests for
UNPACK_SEQUENCE_TWO_TUPLE specialisation.

Targets: UNPACK_SEQUENCE_TWO_TUPLE.

UNPACK_SEQUENCE_TWO_TUPLE specialises the common pattern `a, b = t`
when `t` is a tuple of exactly two elements.  Instead of going through
the generic UNPACK_SEQUENCE path (which calls iter(), next(), checks
length), the specialisation verifies that the object is a tuple with
ob_size == 2 and loads both elements directly from the tuple's ob_item
array — no iterator protocol overhead.

The JIT specialisation emits a GuardType(TTuple) + length check, then
loads the two elements by index.

Deopt triggers:
  - Sequence is not a tuple (list, string, custom iterable)
  - Tuple has wrong length (not exactly 2)
  - Subclass of tuple

Tests cover:
  - Basic two-tuple unpacking (int, str, mixed types)
  - Unpacking function return values
  - Unpacking with None/bool values
  - Nested tuple inside two-tuple
  - Swap idiom (a, b = b, a)
  - Unpacking in loop
  - Multiple unpacking operations in one function
  - Deopt: list instead of tuple
  - Deopt: three-element tuple (ValueError)
  - Deopt: one-element tuple (ValueError)
  - Deopt: string of length 2
  - Deopt: custom __iter__
  - Deopt: generator
  - Deopt: tuple subclass
  - Unpacking from enumerate()
  - Unpacking from dict.items()
  - Identity preservation (object references)
  - Rapid type alternation (tuple/list)
  - Large values (big ints, long strings)
  - Chained unpacking

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct error for invalid sequences

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_unpack_sequence_two_tuple.py
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
    print("=== UNPACK_SEQUENCE_TWO_TUPLE Correctness & Deopt Tests ===")
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

    # ── Test 1: Basic two-tuple unpacking (ints) ───────────────────────

    def unpack_ints_1(t):
        a, b = t
        return a + b

    for _ in range(WARMUP):
        unpack_ints_1((3, 7))

    check_jit_compiled(unpack_ints_1, "unpack_ints_1")

    try:
        assert unpack_ints_1((3, 7)) == 10
        assert unpack_ints_1((0, 0)) == 0
        assert unpack_ints_1((-1, 1)) == 0
        assert unpack_ints_1((100, 200)) == 300
        print("PASS  Test 1: basic two-tuple unpacking (ints)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic two-tuple unpacking — {e}")
        failed += 1

    # ── Test 2: Two-tuple with mixed types ─────────────────────────────

    def unpack_mixed_2(t):
        a, b = t
        return (a, b)

    for _ in range(WARMUP):
        unpack_mixed_2(("hello", 42))

    check_jit_compiled(unpack_mixed_2, "unpack_mixed_2")

    try:
        assert unpack_mixed_2(("hello", 42)) == ("hello", 42)
        assert unpack_mixed_2((None, True)) == (None, True)
        assert unpack_mixed_2((3.14, "pi")) == (3.14, "pi")
        assert unpack_mixed_2(([1, 2], {3: 4})) == ([1, 2], {3: 4})
        print("PASS  Test 2: two-tuple with mixed types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: mixed types — {e}")
        failed += 1

    # ── Test 3: Unpacking function return value ────────────────────────

    def returns_pair():
        return (10, 20)

    def unpack_return_3():
        a, b = returns_pair()
        return a * b

    for _ in range(WARMUP):
        unpack_return_3()

    check_jit_compiled(unpack_return_3, "unpack_return_3")

    try:
        assert unpack_return_3() == 200
        print("PASS  Test 3: unpacking function return value")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: function return — {e}")
        failed += 1

    # ── Test 4: Unpacking with None and bool ───────────────────────────

    def unpack_none_bool_4(t):
        a, b = t
        return a is None, b is True

    for _ in range(WARMUP):
        unpack_none_bool_4((None, True))

    check_jit_compiled(unpack_none_bool_4, "unpack_none_bool_4")

    try:
        assert unpack_none_bool_4((None, True)) == (True, True)
        assert unpack_none_bool_4((None, False)) == (True, False)
        assert unpack_none_bool_4((0, None)) == (False, False)
        assert unpack_none_bool_4((None, None)) == (True, False)
        print("PASS  Test 4: unpacking with None and bool")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: None/bool — {e}")
        failed += 1

    # ── Test 5: Nested tuple inside two-tuple ──────────────────────────

    def unpack_nested_5(t):
        a, b = t
        return a, len(b)

    for _ in range(WARMUP):
        unpack_nested_5((1, (2, 3, 4)))

    check_jit_compiled(unpack_nested_5, "unpack_nested_5")

    try:
        assert unpack_nested_5((1, (2, 3, 4))) == (1, 3)
        assert unpack_nested_5(("x", ())) == ("x", 0)
        assert unpack_nested_5((0, (1,))) == (0, 1)
        print("PASS  Test 5: nested tuple inside two-tuple")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: nested tuple — {e}")
        failed += 1

    # ── Test 6: Swap idiom (a, b = b, a) ──────────────────────────────

    def swap_6(a, b):
        a, b = b, a
        return (a, b)

    for _ in range(WARMUP):
        swap_6(1, 2)

    check_jit_compiled(swap_6, "swap_6")

    try:
        assert swap_6(1, 2) == (2, 1)
        assert swap_6("x", "y") == ("y", "x")
        assert swap_6(0, 0) == (0, 0)
        assert swap_6(None, 42) == (42, None)
        # Repeated swaps return to original
        a, b = 10, 20
        a, b = swap_6(a, b)
        a, b = swap_6(a, b)
        assert (a, b) == (10, 20)
        print("PASS  Test 6: swap idiom (a, b = b, a)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: swap idiom — {e}")
        failed += 1

    # ── Test 7: Unpacking in loop ──────────────────────────────────────

    def sum_pairs_7(pairs):
        total = 0
        for a, b in pairs:
            total += a + b
        return total

    data = [(i, i + 1) for i in range(10)]

    for _ in range(WARMUP):
        sum_pairs_7(data)

    check_jit_compiled(sum_pairs_7, "sum_pairs_7")

    try:
        assert sum_pairs_7(data) == sum(i + (i + 1) for i in range(10))
        assert sum_pairs_7([]) == 0
        assert sum_pairs_7([(1, 2)]) == 3
        assert sum_pairs_7([(0, 0), (0, 0)]) == 0
        print("PASS  Test 7: unpacking in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: loop unpacking — {e}")
        failed += 1

    # ── Test 8: Multiple unpacking in one function ─────────────────────

    def multi_unpack_8(t1, t2):
        a, b = t1
        c, d = t2
        return a + c, b + d

    for _ in range(WARMUP):
        multi_unpack_8((1, 2), (3, 4))

    check_jit_compiled(multi_unpack_8, "multi_unpack_8")

    try:
        assert multi_unpack_8((1, 2), (3, 4)) == (4, 6)
        assert multi_unpack_8((0, 0), (0, 0)) == (0, 0)
        assert multi_unpack_8((-1, 10), (1, -10)) == (0, 0)
        print("PASS  Test 8: multiple unpacking in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: multiple unpacking — {e}")
        failed += 1

    # ── Test 9: Deopt — list instead of tuple ──────────────────────────

    def unpack_deopt_9(seq):
        a, b = seq
        return (a, b)

    for _ in range(WARMUP):
        unpack_deopt_9((1, 2))

    check_jit_compiled(unpack_deopt_9, "unpack_deopt_9")

    try:
        # Tuple path (specialised)
        assert unpack_deopt_9((1, 2)) == (1, 2)

        # List path (deopt)
        assert unpack_deopt_9([3, 4]) == (3, 4)
        assert unpack_deopt_9([0, 0]) == (0, 0)

        # Tuple still works after deopt
        assert unpack_deopt_9((5, 6)) == (5, 6)

        print("PASS  Test 9: deopt — list instead of tuple")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: list deopt — {e}")
        failed += 1

    # ── Test 10: Deopt — three-element tuple (ValueError) ─────────────

    def unpack_three_10(t):
        a, b = t
        return (a, b)

    for _ in range(WARMUP):
        unpack_three_10((1, 2))

    check_jit_compiled(unpack_three_10, "unpack_three_10")

    try:
        assert unpack_three_10((1, 2)) == (1, 2)

        try:
            unpack_three_10((1, 2, 3))
            assert False, "expected ValueError for 3-element tuple"
        except ValueError:
            pass

        # Still works after error
        assert unpack_three_10((7, 8)) == (7, 8)

        print("PASS  Test 10: deopt — three-element tuple (ValueError)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: wrong length — {e}")
        failed += 1

    # ── Test 11: Deopt — one-element tuple (ValueError) ────────────────

    def unpack_one_11(t):
        a, b = t
        return (a, b)

    for _ in range(WARMUP):
        unpack_one_11((1, 2))

    check_jit_compiled(unpack_one_11, "unpack_one_11")

    try:
        assert unpack_one_11((1, 2)) == (1, 2)

        try:
            unpack_one_11((1,))
            assert False, "expected ValueError for 1-element tuple"
        except ValueError:
            pass

        # Empty tuple
        try:
            unpack_one_11(())
            assert False, "expected ValueError for empty tuple"
        except ValueError:
            pass

        # Still works
        assert unpack_one_11((9, 10)) == (9, 10)

        print("PASS  Test 11: deopt — wrong-length tuples (ValueError)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: wrong length — {e}")
        failed += 1

    # ── Test 12: Deopt — string of length 2 ───────────────────────────

    def unpack_str_12(seq):
        a, b = seq
        return (a, b)

    for _ in range(WARMUP):
        unpack_str_12((1, 2))

    check_jit_compiled(unpack_str_12, "unpack_str_12")

    try:
        # Tuple path
        assert unpack_str_12((1, 2)) == (1, 2)

        # String path (deopt — string is iterable of length 2)
        assert unpack_str_12("ab") == ("a", "b")
        assert unpack_str_12("xy") == ("x", "y")

        # Tuple still works
        assert unpack_str_12((3, 4)) == (3, 4)

        print("PASS  Test 12: deopt — string of length 2")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: string deopt — {e}")
        failed += 1

    # ── Test 13: Deopt — custom iterable ──────────────────────────────

    class Pair:
        def __init__(self, a, b):
            self.a = a
            self.b = b
        def __iter__(self):
            yield self.a
            yield self.b

    def unpack_custom_13(seq):
        a, b = seq
        return (a, b)

    for _ in range(WARMUP):
        unpack_custom_13((1, 2))

    check_jit_compiled(unpack_custom_13, "unpack_custom_13")

    try:
        # Tuple path
        assert unpack_custom_13((1, 2)) == (1, 2)

        # Custom iterable (deopt)
        assert unpack_custom_13(Pair(10, 20)) == (10, 20)
        assert unpack_custom_13(Pair("a", "b")) == ("a", "b")

        # Tuple still works
        assert unpack_custom_13((5, 6)) == (5, 6)

        print("PASS  Test 13: deopt — custom iterable")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: custom iterable — {e}")
        failed += 1

    # ── Test 14: Deopt — generator ────────────────────────────────────

    def gen_two():
        yield 100
        yield 200

    def unpack_gen_14(use_gen):
        if use_gen:
            a, b = gen_two()
        else:
            a, b = (1, 2)
        return (a, b)

    for _ in range(WARMUP):
        unpack_gen_14(False)

    check_jit_compiled(unpack_gen_14, "unpack_gen_14")

    try:
        # Tuple path
        assert unpack_gen_14(False) == (1, 2)

        # Generator path (deopt)
        assert unpack_gen_14(True) == (100, 200)

        # Tuple still works
        assert unpack_gen_14(False) == (1, 2)

        print("PASS  Test 14: deopt — generator")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: generator deopt — {e}")
        failed += 1

    # ── Test 15: Unpacking from enumerate() ───────────────────────────

    def sum_enumerated_15(items):
        total = 0
        for i, val in enumerate(items):
            total += i * val
        return total

    for _ in range(WARMUP):
        sum_enumerated_15([10, 20, 30])

    check_jit_compiled(sum_enumerated_15, "sum_enumerated_15")

    try:
        # 0*10 + 1*20 + 2*30 = 0 + 20 + 60 = 80
        assert sum_enumerated_15([10, 20, 30]) == 80
        assert sum_enumerated_15([]) == 0
        assert sum_enumerated_15([1]) == 0
        assert sum_enumerated_15([1, 1, 1]) == 3  # 0+1+2
        print("PASS  Test 15: unpacking from enumerate()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: enumerate — {e}")
        failed += 1

    # ── Test 16: Unpacking from dict.items() ──────────────────────────

    def sum_dict_16(d):
        total = 0
        for k, v in d.items():
            total += v
        return total

    test_dict = {"a": 1, "b": 2, "c": 3}

    for _ in range(WARMUP):
        sum_dict_16(test_dict)

    check_jit_compiled(sum_dict_16, "sum_dict_16")

    try:
        assert sum_dict_16(test_dict) == 6
        assert sum_dict_16({}) == 0
        assert sum_dict_16({"x": 100}) == 100
        print("PASS  Test 16: unpacking from dict.items()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: dict.items — {e}")
        failed += 1

    # ── Test 17: Identity preservation ────────────────────────────────

    def unpack_identity_17(t):
        a, b = t
        return a, b

    sentinel_a = object()
    sentinel_b = object()

    for _ in range(WARMUP):
        unpack_identity_17((sentinel_a, sentinel_b))

    check_jit_compiled(unpack_identity_17, "unpack_identity_17")

    try:
        ra, rb = unpack_identity_17((sentinel_a, sentinel_b))
        assert ra is sentinel_a, "identity of first element lost"
        assert rb is sentinel_b, "identity of second element lost"

        # Mutable objects
        list_a = [1, 2, 3]
        list_b = [4, 5, 6]
        ra, rb = unpack_identity_17((list_a, list_b))
        assert ra is list_a
        assert rb is list_b
        list_a.append(99)
        assert ra[-1] == 99  # Same object

        print("PASS  Test 17: identity preservation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: identity — {e}")
        failed += 1

    # ── Test 18: Rapid type alternation (tuple/list) ──────────────────

    def unpack_alt_18(seq):
        a, b = seq
        return a + b

    for _ in range(WARMUP):
        unpack_alt_18((1, 2))

    check_jit_compiled(unpack_alt_18, "unpack_alt_18")

    try:
        for i in range(100):
            if i % 2 == 0:
                assert unpack_alt_18((i, 1)) == i + 1
            else:
                assert unpack_alt_18([i, 1]) == i + 1

        # Final tuple check
        assert unpack_alt_18((99, 1)) == 100

        print("PASS  Test 18: rapid type alternation (tuple/list)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: type alternation — {e}")
        failed += 1

    # ── Test 19: Deopt — tuple subclass ───────────────────────────────

    class MyTuple(tuple):
        pass

    def unpack_subclass_19(t):
        a, b = t
        return (a, b)

    for _ in range(WARMUP):
        unpack_subclass_19((1, 2))

    check_jit_compiled(unpack_subclass_19, "unpack_subclass_19")

    try:
        # Builtin tuple
        assert unpack_subclass_19((1, 2)) == (1, 2)

        # Tuple subclass (may or may not deopt — depends on implementation)
        mt = MyTuple((10, 20))
        assert unpack_subclass_19(mt) == (10, 20)

        # Builtin still works
        assert unpack_subclass_19((3, 4)) == (3, 4)

        print("PASS  Test 19: tuple subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: tuple subclass — {e}")
        failed += 1

    # ── Test 20: Chained unpacking and reassembly ─────────────────────

    def chain_unpack_20(t1, t2):
        a, b = t1
        c, d = t2
        # Reassemble into new pairs
        e, f = (a + c, b + d)
        return (e, f)

    for _ in range(WARMUP):
        chain_unpack_20((1, 2), (3, 4))

    check_jit_compiled(chain_unpack_20, "chain_unpack_20")

    try:
        assert chain_unpack_20((1, 2), (3, 4)) == (4, 6)
        assert chain_unpack_20((0, 0), (0, 0)) == (0, 0)
        assert chain_unpack_20((10, 20), (30, 40)) == (40, 60)

        # Accumulate through chain
        result = (0, 0)
        for i in range(10):
            result = chain_unpack_20(result, (i, i * 2))
        assert result == (sum(range(10)), sum(i * 2 for i in range(10)))

        print("PASS  Test 20: chained unpacking and reassembly")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: chained unpacking — {e}")
        failed += 1

    # ── Summary ──────────────────────────────────────────────────────

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
