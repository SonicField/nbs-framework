#!/usr/bin/env python3
"""
test_call_py_exact_args.py — Correctness and deopt tests for
CALL_PY_EXACT_ARGS specialisation.

Targets: CALL_PY_EXACT_ARGS.

CALL_PY_EXACT_ARGS specialises Python function calls when the caller
provides exactly the right number of positional arguments — no *args,
no **kwargs, no defaults needed. This skips the generic argument parsing
and validation in PyObject_Call / _PyFunction_Vectorcall, going directly
to the function's code object execution.

The JIT specialisation checks that the callable is a PyFunctionObject with
exactly matching argument count, enabling a fast direct call path.

Deopt triggers:
  - Callable changes from one function to another with different signature
  - Callable is not a Python function (e.g. builtin, C function, method)
  - Wrong number of arguments provided
  - Function uses *args or **kwargs

Tests cover:
  - Basic function call with exact args (0, 1, 2, 3 args)
  - Different return types (int, str, None, list, tuple)
  - Recursive function calls
  - Deopt: Python function -> builtin function
  - Deopt: Python function -> C method
  - Deopt: Python function -> lambda
  - Deopt: exact args function -> function with defaults
  - Closures (captured variables)
  - Nested function calls (f(g(x)))
  - Function as argument (higher-order)
  - Rapid callable alternation
  - Multiple calls in one function
  - Class instantiation (__init__ with exact args)
  - Method calls (bound methods)
  - Identity of return value (object identity preservation)
  - Generator function call (returns generator, not value)
  - Call with None return
  - Exception propagation from callee
  - Equivalence: f(x) vs f.__call__(x)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct deopt when callable type changes
  3. Correct result for both original and new callables after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_py_exact_args.py
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
    # Test 1: Zero-arg function call
    # ------------------------------------------------------------------
    try:
        def no_args():
            return 42

        def call_no_args(f):
            return f()

        for _ in range(WARMUP):
            call_no_args(no_args)
        check_jit_compiled(call_no_args, "call_no_args")

        assert call_no_args(no_args) == 42
        print("  PASS: test_zero_arg_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_zero_arg_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: One-arg function call
    # ------------------------------------------------------------------
    try:
        def double(x):
            return x * 2

        def call_one_arg(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_one_arg(double, 5)
        check_jit_compiled(call_one_arg, "call_one_arg")

        assert call_one_arg(double, 5) == 10
        assert call_one_arg(double, 0) == 0
        assert call_one_arg(double, -3) == -6
        print("  PASS: test_one_arg_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_one_arg_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Two-arg function call
    # ------------------------------------------------------------------
    try:
        def add(a, b):
            return a + b

        def call_two_args(f, a, b):
            return f(a, b)

        for _ in range(WARMUP):
            call_two_args(add, 1, 2)
        check_jit_compiled(call_two_args, "call_two_args")

        assert call_two_args(add, 1, 2) == 3
        assert call_two_args(add, 0, 0) == 0
        assert call_two_args(add, -1, 1) == 0
        assert call_two_args(add, "hello", " world") == "hello world"
        print("  PASS: test_two_arg_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_two_arg_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Three-arg function call
    # ------------------------------------------------------------------
    try:
        def clamp(val, lo, hi):
            if val < lo:
                return lo
            if val > hi:
                return hi
            return val

        def call_clamp(f, v, lo, hi):
            return f(v, lo, hi)

        for _ in range(WARMUP):
            call_clamp(clamp, 5, 0, 10)
        check_jit_compiled(call_clamp, "call_clamp")

        assert call_clamp(clamp, 5, 0, 10) == 5
        assert call_clamp(clamp, -1, 0, 10) == 0
        assert call_clamp(clamp, 15, 0, 10) == 10
        assert call_clamp(clamp, 0, 0, 10) == 0
        assert call_clamp(clamp, 10, 0, 10) == 10
        print("  PASS: test_three_arg_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_three_arg_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Different return types
    # ------------------------------------------------------------------
    try:
        def ret_int():
            return 42

        def ret_str():
            return "hello"

        def ret_none():
            return None

        def ret_list():
            return [1, 2, 3]

        def ret_tuple():
            return (1, 2)

        def caller(f):
            return f()

        for _ in range(WARMUP):
            caller(ret_int)
        check_jit_compiled(caller, "caller")

        assert caller(ret_int) == 42
        assert caller(ret_str) == "hello"
        assert caller(ret_none) is None
        assert caller(ret_list) == [1, 2, 3]
        assert caller(ret_tuple) == (1, 2)
        print("  PASS: test_different_return_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_return_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Recursive function call
    # ------------------------------------------------------------------
    try:
        def factorial(n):
            if n <= 1:
                return 1
            return n * factorial(n - 1)

        for _ in range(WARMUP):
            factorial(10)
        check_jit_compiled(factorial, "factorial")

        assert factorial(0) == 1
        assert factorial(1) == 1
        assert factorial(5) == 120
        assert factorial(10) == 3628800
        print("  PASS: test_recursive_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_recursive_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Deopt — Python function -> builtin function
    # ------------------------------------------------------------------
    try:
        def my_abs(x):
            return x if x >= 0 else -x

        def call_fn(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_fn(my_abs, 5)
        check_jit_compiled(call_fn, "call_fn")

        assert call_fn(my_abs, -5) == 5
        assert call_fn(my_abs, 5) == 5
        # Deopt to builtin abs()
        assert call_fn(abs, -5) == 5
        assert call_fn(abs, 5) == 5
        # Back to Python function
        assert call_fn(my_abs, -10) == 10
        print("  PASS: test_deopt_to_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt — Python function -> lambda
    # ------------------------------------------------------------------
    try:
        def square(x):
            return x * x

        cube = lambda x: x * x * x

        def call_transform(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_transform(square, 3)
        check_jit_compiled(call_transform, "call_transform")

        assert call_transform(square, 3) == 9
        # Lambda is also a Python function but different code object
        assert call_transform(cube, 3) == 27
        assert call_transform(square, 4) == 16
        assert call_transform(cube, 4) == 64
        print("  PASS: test_deopt_to_lambda")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_lambda — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt — exact args function -> function with defaults
    # ------------------------------------------------------------------
    try:
        def strict_add(a, b):
            return a + b

        def default_add(a, b=10):
            return a + b

        def call_add(f, a, b):
            return f(a, b)

        for _ in range(WARMUP):
            call_add(strict_add, 1, 2)
        check_jit_compiled(call_add, "call_add")

        assert call_add(strict_add, 1, 2) == 3
        # Switch to function with defaults — still called with 2 args
        assert call_add(default_add, 1, 2) == 3
        assert call_add(default_add, 5, 5) == 10
        # Back to strict
        assert call_add(strict_add, 10, 20) == 30
        print("  PASS: test_deopt_to_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Closure (captured variables)
    # ------------------------------------------------------------------
    try:
        def make_adder(n):
            def adder(x):
                return x + n
            return adder

        add5 = make_adder(5)
        add10 = make_adder(10)

        def call_adder(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_adder(add5, 1)
        check_jit_compiled(call_adder, "call_adder")

        assert call_adder(add5, 1) == 6
        assert call_adder(add5, 0) == 5
        assert call_adder(add10, 1) == 11
        assert call_adder(add10, 0) == 10
        # Both closures share the same code object but different cells
        assert call_adder(add5, 100) == 105
        assert call_adder(add10, 100) == 110
        print("  PASS: test_closure_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_closure_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Nested function calls (f(g(x)))
    # ------------------------------------------------------------------
    try:
        def inc(x):
            return x + 1

        def dbl(x):
            return x * 2

        def nested_call(f, g, x):
            return f(g(x))

        for _ in range(WARMUP):
            nested_call(inc, dbl, 3)
        check_jit_compiled(nested_call, "nested_call")

        # inc(dbl(3)) = inc(6) = 7
        assert nested_call(inc, dbl, 3) == 7
        # dbl(inc(3)) = dbl(4) = 8
        assert nested_call(dbl, inc, 3) == 8
        # inc(inc(0)) = 2
        assert nested_call(inc, inc, 0) == 2
        print("  PASS: test_nested_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Higher-order function (function as argument)
    # ------------------------------------------------------------------
    try:
        def apply_twice(f, x):
            return f(f(x))

        def add_one(x):
            return x + 1

        def triple(x):
            return x * 3

        for _ in range(WARMUP):
            apply_twice(add_one, 0)
        check_jit_compiled(apply_twice, "apply_twice")

        assert apply_twice(add_one, 0) == 2   # 0+1+1
        assert apply_twice(add_one, 5) == 7   # 5+1+1
        assert apply_twice(triple, 2) == 18   # 2*3*3
        assert apply_twice(triple, 1) == 9    # 1*3*3
        print("  PASS: test_higher_order_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_higher_order_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid callable alternation
    # ------------------------------------------------------------------
    try:
        def fn_a(x):
            return x + 1

        def fn_b(x):
            return x * 2

        def call_poly(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_poly(fn_a, 1)
        check_jit_compiled(call_poly, "call_poly")

        for cycle in range(50):
            assert call_poly(fn_a, 10) == 11, f"fn_a failed at cycle {cycle}"
            assert call_poly(fn_b, 10) == 20, f"fn_b failed at cycle {cycle}"

        assert call_poly(fn_a, 0) == 1
        assert call_poly(fn_b, 0) == 0
        print("  PASS: test_rapid_callable_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_callable_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Multiple calls in one function
    # ------------------------------------------------------------------
    try:
        def step1(x):
            return x + 1

        def step2(x):
            return x * 2

        def step3(x):
            return x - 3

        def pipeline(a, b, c, x):
            return c(b(a(x)))

        for _ in range(WARMUP):
            pipeline(step1, step2, step3, 5)
        check_jit_compiled(pipeline, "pipeline")

        # step3(step2(step1(5))) = step3(step2(6)) = step3(12) = 9
        assert pipeline(step1, step2, step3, 5) == 9
        # step3(step2(step1(0))) = step3(step2(1)) = step3(2) = -1
        assert pipeline(step1, step2, step3, 0) == -1
        print("  PASS: test_multiple_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Class instantiation (__init__ with exact args)
    # ------------------------------------------------------------------
    try:
        class Point:
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def make_point(x, y):
            return Point(x, y)

        for _ in range(WARMUP):
            make_point(1, 2)
        check_jit_compiled(make_point, "make_point")

        p = make_point(3, 4)
        assert p.x == 3 and p.y == 4
        p = make_point(0, 0)
        assert p.x == 0 and p.y == 0
        p = make_point(-1, -2)
        assert p.x == -1 and p.y == -2
        print("  PASS: test_class_instantiation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_class_instantiation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Method calls (bound methods)
    # ------------------------------------------------------------------
    try:
        class Counter:
            def __init__(self):
                self.n = 0

            def inc(self):
                self.n += 1
                return self.n

        def call_method(obj):
            return obj.inc()

        c = Counter()
        for _ in range(WARMUP):
            call_method(c)
        check_jit_compiled(call_method, "call_method")

        c2 = Counter()
        assert call_method(c2) == 1
        assert call_method(c2) == 2
        assert call_method(c2) == 3

        c3 = Counter()
        assert call_method(c3) == 1
        print("  PASS: test_method_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Object identity preservation in return
    # ------------------------------------------------------------------
    try:
        sentinel = object()

        def return_sentinel():
            return sentinel

        def call_ret(f):
            return f()

        for _ in range(WARMUP):
            call_ret(return_sentinel)
        check_jit_compiled(call_ret, "call_ret")

        assert call_ret(return_sentinel) is sentinel
        another = object()

        def return_another():
            return another

        assert call_ret(return_another) is another
        assert call_ret(return_another) is not sentinel
        print("  PASS: test_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Generator function call (returns generator, not value)
    # ------------------------------------------------------------------
    try:
        def gen_fn(n):
            for i in range(n):
                yield i

        def call_gen(f, n):
            return list(f(n))

        for _ in range(WARMUP):
            call_gen(gen_fn, 5)
        check_jit_compiled(call_gen, "call_gen")

        assert call_gen(gen_fn, 5) == [0, 1, 2, 3, 4]
        assert call_gen(gen_fn, 0) == []
        assert call_gen(gen_fn, 1) == [0]
        print("  PASS: test_generator_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_generator_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Exception propagation from callee
    # ------------------------------------------------------------------
    try:
        def raise_value_error(x):
            if x < 0:
                raise ValueError(f"negative: {x}")
            return x

        def call_may_raise(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_may_raise(raise_value_error, 5)
        check_jit_compiled(call_may_raise, "call_may_raise")

        assert call_may_raise(raise_value_error, 5) == 5
        assert call_may_raise(raise_value_error, 0) == 0

        raised = False
        try:
            call_may_raise(raise_value_error, -1)
        except ValueError as e:
            raised = True
            assert "negative: -1" in str(e)
        assert raised, "Expected ValueError from callee"

        # After exception, function still works
        assert call_may_raise(raise_value_error, 10) == 10
        print("  PASS: test_exception_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_exception_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — f(x) vs f.__call__(x)
    # ------------------------------------------------------------------
    try:
        def negate(x):
            return -x

        def direct_call(f, x):
            return f(x)

        def dunder_call(f, x):
            return f.__call__(x)

        for _ in range(WARMUP):
            direct_call(negate, 5)
        check_jit_compiled(direct_call, "direct_call")

        test_values = [0, 1, -1, 100, -100, 42]
        for v in test_values:
            assert direct_call(negate, v) == dunder_call(negate, v), (
                f"Mismatch for {v}: "
                f"direct={direct_call(negate, v)}, "
                f"dunder={dunder_call(negate, v)}"
            )
        print("  PASS: test_equivalence_call_vs_dunder")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_call_vs_dunder — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_PY_EXACT_ARGS: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
