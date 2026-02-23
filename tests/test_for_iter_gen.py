#!/usr/bin/env python3
"""
test_for_iter_gen.py — Correctness and deopt tests for
FOR_ITER_GEN specialisation.

Targets: FOR_ITER_GEN.

FOR_ITER_GEN specialises the for-loop iteration opcode when the iterator
is a generator object. Instead of going through the generic InvokeIterNext
path (which calls PyIter_Next → tp_iternext → gen_iternext), the
specialisation can call the generator's send(None) fast path directly.

The adaptive interpreter emits FOR_ITER_GEN after observing repeated
iteration over generator objects in a for loop.

Deopt triggers:
  - Iterator type changes (generator → list_iterator, etc.)
  - Iterator is not a generator (e.g. list, range, custom __iter__)

Tests cover:
  - Basic generator iteration (yield ints)
  - Empty generator (immediate return)
  - Generator with yield and return value
  - Generator yielding different types
  - Nested generator iteration (yield from)
  - Generator with exception (raise inside)
  - Generator with try/finally
  - Deopt: generator-compiled → list iteration
  - Deopt: generator-compiled → range iteration
  - Deopt: generator-compiled → custom iterator
  - Generator expression (genexpr)
  - Large generator (many yields)
  - Generator with send-like state (stateful)
  - Chained generators (pipeline)
  - Generator with conditional yields
  - Multiple for loops over generators in one function
  - Generator that yields None
  - Recursive generator (tree traversal)
  - Generator vs list comprehension equivalence
  - Rapid iterator type alternation

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (StopIteration, exceptions)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_for_iter_gen.py
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
    print("=== FOR_ITER_GEN Correctness & Deopt Tests ===")
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

    # ── Helper generators ─────────────────────────────────────────────

    def gen_ints(n):
        for i in range(n):
            yield i

    def gen_empty():
        return
        yield  # Make it a generator

    def gen_mixed():
        yield 42
        yield "hello"
        yield [1, 2, 3]
        yield None
        yield (True, False)

    # ── Test 1: Basic generator iteration ──────────────────────────────

    def iter_gen_1(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_gen_1(gen_ints(5))

    check_jit_compiled(iter_gen_1, "iter_gen_1")

    try:
        assert iter_gen_1(gen_ints(5)) == [0, 1, 2, 3, 4]
        assert iter_gen_1(gen_ints(0)) == []
        assert iter_gen_1(gen_ints(1)) == [0]
        assert iter_gen_1(gen_ints(10)) == list(range(10))
        print("PASS  Test 1: basic generator iteration")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic generator — {e}")
        failed += 1

    # ── Test 2: Empty generator ────────────────────────────────────────

    def iter_empty_2(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_empty_2(gen_ints(0))

    check_jit_compiled(iter_empty_2, "iter_empty_2")

    try:
        assert iter_empty_2(gen_empty()) == []
        # Also works with a generator that just has no items
        def gen_none_yielded():
            if False:
                yield
        assert iter_empty_2(gen_none_yielded()) == []
        print("PASS  Test 2: empty generator")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: empty generator — {e}")
        failed += 1

    # ── Test 3: Generator with return value (ignored by for loop) ──────

    def gen_with_return():
        yield 1
        yield 2
        return "final"  # This is discarded by for-loop

    def iter_return_3(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_return_3(gen_ints(3))

    check_jit_compiled(iter_return_3, "iter_return_3")

    try:
        assert iter_return_3(gen_with_return()) == [1, 2]
        # Return value is not visible to for-loop
        assert iter_return_3(gen_ints(3)) == [0, 1, 2]
        print("PASS  Test 3: generator with return value (ignored by for)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: return value — {e}")
        failed += 1

    # ── Test 4: Generator yielding different types ─────────────────────

    def iter_mixed_4(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_mixed_4(gen_ints(3))

    check_jit_compiled(iter_mixed_4, "iter_mixed_4")

    try:
        result = iter_mixed_4(gen_mixed())
        assert result == [42, "hello", [1, 2, 3], None, (True, False)]
        print("PASS  Test 4: generator yielding different types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: mixed types — {e}")
        failed += 1

    # ── Test 5: Nested generator (yield from) ──────────────────────────

    def gen_outer():
        yield 1
        yield from gen_ints(3)
        yield 99

    def iter_nested_5(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_nested_5(gen_ints(5))

    check_jit_compiled(iter_nested_5, "iter_nested_5")

    try:
        assert iter_nested_5(gen_outer()) == [1, 0, 1, 2, 99]
        print("PASS  Test 5: nested generator (yield from)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: yield from — {e}")
        failed += 1

    # ── Test 6: Generator with exception ───────────────────────────────

    def gen_raises():
        yield 1
        yield 2
        raise ValueError("gen error")

    def iter_except_6(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_except_6(gen_ints(3))

    check_jit_compiled(iter_except_6, "iter_except_6")

    try:
        try:
            iter_except_6(gen_raises())
            assert False, "expected ValueError"
        except ValueError as e:
            assert str(e) == "gen error"

        # Normal generator still works after error
        assert iter_except_6(gen_ints(3)) == [0, 1, 2]

        print("PASS  Test 6: generator with exception")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: exception — {e}")
        failed += 1

    # ── Test 7: Generator with try/finally ─────────────────────────────

    cleanup_ran = [False]

    def gen_finally():
        try:
            yield 1
            yield 2
            yield 3
        finally:
            cleanup_ran[0] = True

    def iter_finally_7(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_finally_7(gen_ints(3))

    check_jit_compiled(iter_finally_7, "iter_finally_7")

    try:
        cleanup_ran[0] = False
        assert iter_finally_7(gen_finally()) == [1, 2, 3]
        assert cleanup_ran[0], "finally block did not run"
        print("PASS  Test 7: generator with try/finally")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: try/finally — {e}")
        failed += 1

    # ── Test 8: Deopt — generator to list iteration ────────────────────

    def iter_deopt_list_8(iterable):
        result = []
        for x in iterable:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_deopt_list_8(gen_ints(3))

    check_jit_compiled(iter_deopt_list_8, "iter_deopt_list_8")

    try:
        # Generator path
        assert iter_deopt_list_8(gen_ints(3)) == [0, 1, 2]

        # List path (deopt)
        assert iter_deopt_list_8([10, 20, 30]) == [10, 20, 30]

        # Generator still works
        assert iter_deopt_list_8(gen_ints(2)) == [0, 1]

        print("PASS  Test 8: deopt — generator to list")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt list — {e}")
        failed += 1

    # ── Test 9: Deopt — generator to range iteration ───────────────────

    def iter_deopt_range_9(iterable):
        total = 0
        for x in iterable:
            total += x
        return total

    for _ in range(WARMUP):
        iter_deopt_range_9(gen_ints(5))

    check_jit_compiled(iter_deopt_range_9, "iter_deopt_range_9")

    try:
        # Generator path
        assert iter_deopt_range_9(gen_ints(5)) == 0 + 1 + 2 + 3 + 4

        # Range path (deopt)
        assert iter_deopt_range_9(range(5)) == 10

        # Generator still works
        assert iter_deopt_range_9(gen_ints(3)) == 3

        print("PASS  Test 9: deopt — generator to range")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt range — {e}")
        failed += 1

    # ── Test 10: Deopt — generator to custom iterator ──────────────────

    class CountUp:
        def __init__(self, n):
            self._n = n
            self._i = 0
        def __iter__(self):
            return self
        def __next__(self):
            if self._i >= self._n:
                raise StopIteration
            val = self._i
            self._i += 1
            return val

    def iter_deopt_custom_10(iterable):
        result = []
        for x in iterable:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_deopt_custom_10(gen_ints(3))

    check_jit_compiled(iter_deopt_custom_10, "iter_deopt_custom_10")

    try:
        # Generator path
        assert iter_deopt_custom_10(gen_ints(3)) == [0, 1, 2]

        # Custom iterator (deopt)
        assert iter_deopt_custom_10(CountUp(4)) == [0, 1, 2, 3]

        # Generator still works
        assert iter_deopt_custom_10(gen_ints(2)) == [0, 1]

        print("PASS  Test 10: deopt — generator to custom iterator")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: deopt custom — {e}")
        failed += 1

    # ── Test 11: Generator expression ──────────────────────────────────

    def iter_genexpr_11(n):
        result = []
        for x in (i * i for i in range(n)):
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_genexpr_11(5)

    check_jit_compiled(iter_genexpr_11, "iter_genexpr_11")

    try:
        assert iter_genexpr_11(5) == [0, 1, 4, 9, 16]
        assert iter_genexpr_11(0) == []
        assert iter_genexpr_11(1) == [0]
        print("PASS  Test 11: generator expression")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: genexpr — {e}")
        failed += 1

    # ── Test 12: Large generator (1000 yields) ─────────────────────────

    def iter_large_12(g):
        total = 0
        for x in g:
            total += x
        return total

    for _ in range(WARMUP):
        iter_large_12(gen_ints(10))

    check_jit_compiled(iter_large_12, "iter_large_12")

    try:
        assert iter_large_12(gen_ints(1000)) == sum(range(1000))
        assert iter_large_12(gen_ints(10000)) == sum(range(10000))
        print("PASS  Test 12: large generator (10000 yields)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: large generator — {e}")
        failed += 1

    # ── Test 13: Stateful generator ────────────────────────────────────

    def gen_accumulate(values):
        total = 0
        for v in values:
            total += v
            yield total

    def iter_stateful_13(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_stateful_13(gen_ints(5))

    check_jit_compiled(iter_stateful_13, "iter_stateful_13")

    try:
        assert iter_stateful_13(gen_accumulate([1, 2, 3, 4, 5])) == [1, 3, 6, 10, 15]
        assert iter_stateful_13(gen_accumulate([])) == []
        assert iter_stateful_13(gen_accumulate([10])) == [10]
        print("PASS  Test 13: stateful generator (running sum)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: stateful — {e}")
        failed += 1

    # ── Test 14: Chained generators (pipeline) ─────────────────────────

    def gen_double(g):
        for x in g:
            yield x * 2

    def gen_filter_even(g):
        for x in g:
            if x % 2 == 0:
                yield x

    def iter_chain_14(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_chain_14(gen_ints(5))

    check_jit_compiled(iter_chain_14, "iter_chain_14")

    try:
        # Pipeline: gen_ints -> double -> filter_even
        pipeline = gen_filter_even(gen_double(gen_ints(6)))
        assert iter_chain_14(pipeline) == [0, 2, 4, 6, 8, 10]

        # All doubled values are even, so filter passes all
        pipeline2 = gen_filter_even(gen_double(gen_ints(3)))
        assert iter_chain_14(pipeline2) == [0, 2, 4]

        print("PASS  Test 14: chained generators (pipeline)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: pipeline — {e}")
        failed += 1

    # ── Test 15: Generator with conditional yields ─────────────────────

    def gen_conditional(n):
        for i in range(n):
            if i % 2 == 0:
                yield i
            elif i % 3 == 0:
                yield i * 10

    def iter_cond_15(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_cond_15(gen_ints(5))

    check_jit_compiled(iter_cond_15, "iter_cond_15")

    try:
        # 0 (even), 2 (even), 3*10=30 (odd, div by 3), 4 (even), 6 (even),
        # 8 (even), 9*10=90 (odd, div by 3)
        assert iter_cond_15(gen_conditional(10)) == [0, 2, 30, 4, 6, 8, 90]
        assert iter_cond_15(gen_conditional(0)) == []
        assert iter_cond_15(gen_conditional(1)) == [0]
        print("PASS  Test 15: conditional yields")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: conditional — {e}")
        failed += 1

    # ── Test 16: Multiple for loops over generators ────────────────────

    def multi_loop_16(g1, g2):
        r1 = []
        for x in g1:
            r1.append(x)
        r2 = []
        for x in g2:
            r2.append(x)
        return r1, r2

    for _ in range(WARMUP):
        multi_loop_16(gen_ints(3), gen_ints(2))

    check_jit_compiled(multi_loop_16, "multi_loop_16")

    try:
        r1, r2 = multi_loop_16(gen_ints(3), gen_ints(4))
        assert r1 == [0, 1, 2]
        assert r2 == [0, 1, 2, 3]

        r1, r2 = multi_loop_16(gen_ints(0), gen_ints(1))
        assert r1 == []
        assert r2 == [0]

        print("PASS  Test 16: multiple for loops over generators")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: multi loop — {e}")
        failed += 1

    # ── Test 17: Generator that yields None ────────────────────────────

    def gen_nones(n):
        for _ in range(n):
            yield None

    def iter_none_17(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_none_17(gen_ints(3))

    check_jit_compiled(iter_none_17, "iter_none_17")

    try:
        assert iter_none_17(gen_nones(3)) == [None, None, None]
        assert iter_none_17(gen_nones(0)) == []
        # Ensure None is not confused with StopIteration
        assert len(iter_none_17(gen_nones(5))) == 5
        print("PASS  Test 17: generator yielding None")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: None yields — {e}")
        failed += 1

    # ── Test 18: Recursive generator (tree traversal) ──────────────────

    def gen_tree(node):
        """In-order traversal of a binary tree represented as (left, val, right)."""
        if node is None:
            return
        left, val, right = node
        yield from gen_tree(left)
        yield val
        yield from gen_tree(right)

    def iter_tree_18(g):
        result = []
        for x in g:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_tree_18(gen_ints(5))

    check_jit_compiled(iter_tree_18, "iter_tree_18")

    try:
        #       3
        #      / \
        #     1   5
        #    / \ / \
        #   0  2 4  6
        tree = (
            (
                (None, 0, None),
                1,
                (None, 2, None)
            ),
            3,
            (
                (None, 4, None),
                5,
                (None, 6, None)
            )
        )
        assert iter_tree_18(gen_tree(tree)) == [0, 1, 2, 3, 4, 5, 6]
        assert iter_tree_18(gen_tree(None)) == []
        assert iter_tree_18(gen_tree((None, 42, None))) == [42]
        print("PASS  Test 18: recursive generator (tree traversal)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: tree traversal — {e}")
        failed += 1

    # ── Test 19: Generator vs list comprehension equivalence ───────────

    def via_gen_19(n):
        result = []
        for x in (i * i for i in range(n) if i % 2 == 0):
            result.append(x)
        return result

    for _ in range(WARMUP):
        via_gen_19(10)

    check_jit_compiled(via_gen_19, "via_gen_19")

    try:
        gen_result = via_gen_19(10)
        list_result = [i * i for i in range(10) if i % 2 == 0]
        assert gen_result == list_result, (
            f"gen={gen_result}, list={list_result}"
        )

        assert via_gen_19(0) == []
        assert via_gen_19(1) == [0]
        assert via_gen_19(20) == [i * i for i in range(20) if i % 2 == 0]

        print("PASS  Test 19: generator vs list comprehension equivalence")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: gen vs listcomp — {e}")
        failed += 1

    # ── Test 20: Rapid iterator type alternation ───────────────────────

    def iter_rapid_20(iterable):
        result = []
        for x in iterable:
            result.append(x)
        return result

    for _ in range(WARMUP):
        iter_rapid_20(gen_ints(3))

    check_jit_compiled(iter_rapid_20, "iter_rapid_20")

    try:
        for i in range(50):
            if i % 4 == 0:
                assert iter_rapid_20(gen_ints(3)) == [0, 1, 2]
            elif i % 4 == 1:
                assert iter_rapid_20([10, 20]) == [10, 20]
            elif i % 4 == 2:
                assert iter_rapid_20(range(3)) == [0, 1, 2]
            else:
                assert iter_rapid_20((7, 8, 9)) == [7, 8, 9]

        # Final generator check
        assert iter_rapid_20(gen_ints(5)) == [0, 1, 2, 3, 4]

        print("PASS  Test 20: rapid iterator type alternation (50 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: type alternation — {e}")
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
