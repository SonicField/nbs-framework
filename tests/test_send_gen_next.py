#!/usr/bin/env python3
"""
test_send_gen_next.py — Correctness and deopt tests for
SEND_GEN_NEXT specialisation (FOR_ITER_GEN / SEND_GEN).

Targets: SEND_GEN_NEXT (CPython 3.12 specialised generator iteration).

In CPython 3.12, FOR_ITER is specialised for generators as FOR_ITER_GEN,
and SEND is specialised as SEND_GEN. These optimise the common case of
iterating over a generator by avoiding the full tp_iternext dispatch and
instead directly resuming the generator frame. CinderX JIT can further
optimise this path.

Mechanism:
1. Adaptive interpreter detects FOR_ITER/SEND with a generator object
2. Replaces with FOR_ITER_GEN / SEND_GEN
3. Directly resumes the generator frame, skipping tp_iternext overhead
4. Handles StopIteration internally without raising

Deopt triggers:
  - Iterator is not a generator (e.g. list_iterator, custom __next__)
  - Generator is exhausted
  - Generator throws an exception

Tests cover:
  - Simple generator with yield
  - Generator with multiple yields
  - Generator with yield in loop
  - Generator with arguments
  - Generator with return value
  - Generator expression (genexp)
  - Nested generators (yield from)
  - Generator with send()
  - Generator with throw()
  - Generator with close()
  - Deopt: generator to list iterator
  - Deopt: generator to custom iterator
  - Fibonacci generator
  - Rapid iteration (1000 yields)
  - Stability — 10000 iterations
  - Generator with try/finally
  - Generator with conditional yield
  - Chained generators
  - Empty generator
  - Equivalence: for-loop vs manual next()

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after iterator type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_send_gen_next.py
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
    # Test 1: Simple generator with yield
    # ------------------------------------------------------------------
    try:
        def single_yield():
            yield 42

        def consume_single():
            result = []
            for x in single_yield():
                result.append(x)
            return result

        for _ in range(WARMUP):
            consume_single()
        check_jit_compiled(consume_single, "consume_single")

        assert consume_single() == [42]
        print("  PASS: test_simple_yield")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_simple_yield — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Generator with multiple yields
    # ------------------------------------------------------------------
    try:
        def multi_yield():
            yield 1
            yield 2
            yield 3

        def consume_multi():
            return list(multi_yield())

        for _ in range(WARMUP):
            consume_multi()
        check_jit_compiled(consume_multi, "consume_multi")

        assert consume_multi() == [1, 2, 3]
        print("  PASS: test_multiple_yields")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_yields — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Generator with yield in loop
    # ------------------------------------------------------------------
    try:
        def count_up(n):
            for i in range(n):
                yield i

        def consume_count(n):
            result = []
            for x in count_up(n):
                result.append(x)
            return result

        for _ in range(WARMUP):
            consume_count(5)
        check_jit_compiled(consume_count, "consume_count")

        assert consume_count(0) == []
        assert consume_count(1) == [0]
        assert consume_count(5) == [0, 1, 2, 3, 4]
        assert consume_count(10) == list(range(10))
        print("  PASS: test_yield_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_yield_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Generator with arguments
    # ------------------------------------------------------------------
    try:
        def range_gen(start, stop, step=1):
            i = start
            while i < stop:
                yield i
                i += step

        def consume_range(start, stop, step=1):
            return list(range_gen(start, stop, step))

        for _ in range(WARMUP):
            consume_range(0, 5)
        check_jit_compiled(consume_range, "consume_range")

        assert consume_range(0, 5) == [0, 1, 2, 3, 4]
        assert consume_range(2, 8, 2) == [2, 4, 6]
        assert consume_range(10, 10) == []
        assert consume_range(-3, 3) == [-3, -2, -1, 0, 1, 2]
        print("  PASS: test_generator_with_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_generator_with_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Generator with return value (StopIteration.value)
    # ------------------------------------------------------------------
    try:
        def gen_with_return():
            yield 1
            yield 2
            return "done"

        def consume_return():
            g = gen_with_return()
            items = []
            while True:
                try:
                    items.append(next(g))
                except StopIteration as e:
                    return (items, e.value)

        for _ in range(WARMUP):
            consume_return()
        check_jit_compiled(consume_return, "consume_return")

        items, retval = consume_return()
        assert items == [1, 2]
        assert retval == "done"
        print("  PASS: test_generator_return_value")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_generator_return_value — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Generator expression (genexp)
    # ------------------------------------------------------------------
    try:
        def sum_genexp(n):
            return sum(x * x for x in range(n))

        for _ in range(WARMUP):
            sum_genexp(10)
        check_jit_compiled(sum_genexp, "sum_genexp")

        assert sum_genexp(0) == 0
        assert sum_genexp(1) == 0
        assert sum_genexp(4) == 0 + 1 + 4 + 9  # 14
        assert sum_genexp(10) == sum(x * x for x in range(10))
        print("  PASS: test_genexp")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_genexp — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Nested generators (yield from)
    # ------------------------------------------------------------------
    try:
        def inner_gen():
            yield 1
            yield 2

        def outer_gen():
            yield 0
            yield from inner_gen()
            yield 3

        def consume_nested():
            return list(outer_gen())

        for _ in range(WARMUP):
            consume_nested()
        check_jit_compiled(consume_nested, "consume_nested")

        assert consume_nested() == [0, 1, 2, 3]
        print("  PASS: test_yield_from")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_yield_from — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Generator with send()
    # ------------------------------------------------------------------
    try:
        def accumulator():
            total = 0
            while True:
                val = yield total
                if val is None:
                    break
                total += val

        def use_send():
            g = accumulator()
            next(g)  # prime
            r1 = g.send(10)
            r2 = g.send(20)
            r3 = g.send(30)
            return (r1, r2, r3)

        for _ in range(WARMUP):
            use_send()
        check_jit_compiled(use_send, "use_send")

        assert use_send() == (10, 30, 60)
        print("  PASS: test_generator_send")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_generator_send — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Generator with throw()
    # ------------------------------------------------------------------
    try:
        def resilient_gen():
            results = []
            for i in range(5):
                try:
                    yield i
                except ValueError:
                    results.append(f"caught:{i}")
            return results

        def use_throw():
            g = resilient_gen()
            vals = []
            vals.append(next(g))  # 0
            vals.append(next(g))  # 1
            g.throw(ValueError("test"))  # caught at i=2
            vals.append(next(g))  # 3
            return vals

        for _ in range(WARMUP):
            use_throw()
        check_jit_compiled(use_throw, "use_throw")

        assert use_throw() == [0, 1, 3]
        print("  PASS: test_generator_throw")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_generator_throw — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Generator with close()
    # ------------------------------------------------------------------
    try:
        cleanup_called = [False]

        def closeable_gen():
            try:
                yield 1
                yield 2
                yield 3
            finally:
                cleanup_called[0] = True

        def use_close():
            cleanup_called[0] = False
            g = closeable_gen()
            first = next(g)
            g.close()
            return (first, cleanup_called[0])

        for _ in range(WARMUP):
            use_close()
        check_jit_compiled(use_close, "use_close")

        first, cleaned = use_close()
        assert first == 1
        assert cleaned is True
        print("  PASS: test_generator_close")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_generator_close — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — generator to list iterator
    # ------------------------------------------------------------------
    try:
        def gen_123():
            yield 1
            yield 2
            yield 3

        def sum_iterable(iterable):
            total = 0
            for x in iterable:
                total += x
            return total

        # Warm up on generator
        for _ in range(WARMUP):
            sum_iterable(gen_123())
        check_jit_compiled(sum_iterable, "sum_iterable")

        assert sum_iterable(gen_123()) == 6

        # Deopt: list iterator
        assert sum_iterable([10, 20, 30]) == 60

        # Deopt: tuple iterator
        assert sum_iterable((100, 200)) == 300

        # Back to generator
        assert sum_iterable(gen_123()) == 6
        print("  PASS: test_deopt_gen_to_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_gen_to_list — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt — generator to custom iterator
    # ------------------------------------------------------------------
    try:
        def simple_gen(n):
            for i in range(n):
                yield i * 10

        class CustomIter:
            def __init__(self, n):
                self.n = n
                self.i = 0
            def __iter__(self):
                return self
            def __next__(self):
                if self.i >= self.n:
                    raise StopIteration
                val = self.i * 100
                self.i += 1
                return val

        def collect(iterable):
            result = []
            for x in iterable:
                result.append(x)
            return result

        for _ in range(WARMUP):
            collect(simple_gen(3))
        check_jit_compiled(collect, "collect")

        assert collect(simple_gen(3)) == [0, 10, 20]

        # Deopt: custom iterator
        assert collect(CustomIter(3)) == [0, 100, 200]

        # Back to generator
        assert collect(simple_gen(2)) == [0, 10]
        print("  PASS: test_deopt_gen_to_custom_iter")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_gen_to_custom_iter — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Fibonacci generator
    # ------------------------------------------------------------------
    try:
        def fibonacci(n):
            a, b = 0, 1
            for _ in range(n):
                yield a
                a, b = b, a + b

        def get_fibs(n):
            return list(fibonacci(n))

        for _ in range(WARMUP):
            get_fibs(10)
        check_jit_compiled(get_fibs, "get_fibs")

        assert get_fibs(0) == []
        assert get_fibs(1) == [0]
        assert get_fibs(2) == [0, 1]
        assert get_fibs(8) == [0, 1, 1, 2, 3, 5, 8, 13]
        assert get_fibs(10)[-1] == 34
        print("  PASS: test_fibonacci_generator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_fibonacci_generator — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid iteration (1000 yields)
    # ------------------------------------------------------------------
    try:
        def big_gen(n):
            for i in range(n):
                yield i

        def sum_gen(n):
            total = 0
            for x in big_gen(n):
                total += x
            return total

        for _ in range(WARMUP):
            sum_gen(10)
        check_jit_compiled(sum_gen, "sum_gen")

        assert sum_gen(1000) == sum(range(1000))
        assert sum_gen(0) == 0
        assert sum_gen(1) == 0
        print("  PASS: test_rapid_1000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_1000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Stability — 10000 iterations
    # ------------------------------------------------------------------
    try:
        def counter_gen(n):
            for i in range(n):
                yield 1

        def count_ones(n):
            total = 0
            for x in counter_gen(n):
                total += x
            return total

        for _ in range(WARMUP):
            count_ones(10)
        check_jit_compiled(count_ones, "count_ones")

        assert count_ones(10000) == 10000
        print("  PASS: test_stability_10000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Generator with try/finally
    # ------------------------------------------------------------------
    try:
        log = []

        def guarded_gen(items):
            log.clear()
            try:
                for item in items:
                    log.append(f"yield:{item}")
                    yield item
            finally:
                log.append("cleanup")

        def consume_guarded(items):
            return list(guarded_gen(items))

        for _ in range(WARMUP):
            consume_guarded([1])
        check_jit_compiled(consume_guarded, "consume_guarded")

        result = consume_guarded([10, 20, 30])
        assert result == [10, 20, 30]
        assert log == ["yield:10", "yield:20", "yield:30", "cleanup"]

        # Empty case
        result2 = consume_guarded([])
        assert result2 == []
        assert log == ["cleanup"]
        print("  PASS: test_try_finally")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_try_finally — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Generator with conditional yield
    # ------------------------------------------------------------------
    try:
        def filter_gen(items, predicate):
            for item in items:
                if predicate(item):
                    yield item

        def get_evens(items):
            return list(filter_gen(items, lambda x: x % 2 == 0))

        for _ in range(WARMUP):
            get_evens(range(10))
        check_jit_compiled(get_evens, "get_evens")

        assert get_evens(range(10)) == [0, 2, 4, 6, 8]
        assert get_evens([1, 3, 5]) == []
        assert get_evens([2, 4, 6]) == [2, 4, 6]
        assert get_evens([]) == []
        print("  PASS: test_conditional_yield")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_conditional_yield — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained generators
    # ------------------------------------------------------------------
    try:
        def doubles(items):
            for x in items:
                yield x * 2

        def add_one(items):
            for x in items:
                yield x + 1

        def pipeline(data):
            return list(add_one(doubles(data)))

        for _ in range(WARMUP):
            pipeline([1, 2, 3])
        check_jit_compiled(pipeline, "pipeline")

        assert pipeline([1, 2, 3]) == [3, 5, 7]  # (1*2)+1, (2*2)+1, (3*2)+1
        assert pipeline([]) == []
        assert pipeline([0]) == [1]
        assert pipeline([10, 20]) == [21, 41]
        print("  PASS: test_chained_generators")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_generators — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Empty generator
    # ------------------------------------------------------------------
    try:
        def empty_gen():
            return
            yield  # makes it a generator

        def consume_empty():
            return list(empty_gen())

        for _ in range(WARMUP):
            consume_empty()
        check_jit_compiled(consume_empty, "consume_empty")

        assert consume_empty() == []

        # Also test generator that yields nothing via condition
        def conditional_empty(flag):
            if flag:
                yield 1

        def consume_conditional(flag):
            return list(conditional_empty(flag))

        for _ in range(WARMUP):
            consume_conditional(False)
        assert consume_conditional(False) == []
        assert consume_conditional(True) == [1]
        print("  PASS: test_empty_generator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_generator — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — for-loop vs manual next()
    # ------------------------------------------------------------------
    try:
        def counting_gen(n):
            for i in range(n):
                yield i

        def via_for_loop(n):
            result = []
            for x in counting_gen(n):
                result.append(x)
            return result

        def via_manual_next(n):
            result = []
            g = counting_gen(n)
            while True:
                try:
                    result.append(next(g))
                except StopIteration:
                    break
            return result

        for _ in range(WARMUP):
            via_for_loop(5)
            via_manual_next(5)
        check_jit_compiled(via_for_loop, "via_for_loop")
        check_jit_compiled(via_manual_next, "via_manual_next")

        for n in [0, 1, 5, 10, 50]:
            fl = via_for_loop(n)
            mn = via_manual_next(n)
            assert fl == mn, f"Mismatch for n={n}: {fl} vs {mn}"
            assert fl == list(range(n))
        print("  PASS: test_equivalence_for_vs_next")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_for_vs_next — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nSEND_GEN_NEXT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
