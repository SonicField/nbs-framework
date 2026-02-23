#!/usr/bin/env python3
"""
test_call_py_with_defaults.py — Correctness and deopt tests for
CALL_PY_WITH_DEFAULTS specialisation.

Targets: CALL_PY_WITH_DEFAULTS.

CALL_PY_WITH_DEFAULTS specialises Python function calls when some
arguments use default values. This is the companion to CALL_PY_EXACT_ARGS:
while EXACT_ARGS handles cases where every parameter is provided by the
caller, WITH_DEFAULTS handles cases where the caller omits trailing
arguments that have defaults.

The specialisation skips the generic argument parsing overhead by knowing
at compile time which defaults to fill in.

Deopt triggers:
  - Callable changes to a function with a different signature
  - Callable is not a Python function (e.g. builtin, C function)
  - Default values change (function.__defaults__ modified)

Tests cover:
  - Call with all defaults used (no args provided for defaulted params)
  - Call with some defaults used (partial override)
  - Call with no defaults used (all args provided explicitly)
  - Different default value types (int, str, None, tuple, bool)
  - Mutable default pitfall (list default shared across calls)
  - Deopt: function with defaults -> builtin
  - Deopt: function with defaults -> function without defaults
  - Deopt: function with defaults -> different defaults
  - Closure with defaults
  - Multiple defaulted parameters
  - Default None sentinel pattern
  - Recursive function with defaults
  - Method with defaults
  - Rapid callable alternation
  - Default value identity preservation
  - Exception propagation
  - Loop calling with/without defaults alternating
  - Keyword-style positional (all positional, some defaulted)
  - Equivalence: f(x) vs f.__call__(x) with defaults
  - Function whose defaults are modified at runtime

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct default values used when args omitted
  3. Correct deopt when callable type changes

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_py_with_defaults.py
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
    # Test 1: All defaults used (no args for defaulted params)
    # ------------------------------------------------------------------
    try:
        def greet(name="world"):
            return f"hello {name}"

        def call_greet(f):
            return f()

        for _ in range(WARMUP):
            call_greet(greet)
        check_jit_compiled(call_greet, "call_greet")

        assert call_greet(greet) == "hello world"
        print("  PASS: test_all_defaults_used")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_all_defaults_used — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Some defaults used (partial override)
    # ------------------------------------------------------------------
    try:
        def power(base, exp=2):
            return base ** exp

        def call_power_default(f, b):
            return f(b)

        def call_power_explicit(f, b, e):
            return f(b, e)

        for _ in range(WARMUP):
            call_power_default(power, 3)
        check_jit_compiled(call_power_default, "call_power_default")

        # Using default exp=2
        assert call_power_default(power, 3) == 9
        assert call_power_default(power, 5) == 25

        for _ in range(WARMUP):
            call_power_explicit(power, 2, 3)

        # Overriding default
        assert call_power_explicit(power, 2, 3) == 8
        assert call_power_explicit(power, 2, 10) == 1024
        print("  PASS: test_partial_default_override")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_partial_default_override — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: No defaults used (all args provided)
    # ------------------------------------------------------------------
    try:
        def add_default(a, b=10):
            return a + b

        def call_both(f, a, b):
            return f(a, b)

        for _ in range(WARMUP):
            call_both(add_default, 1, 2)
        check_jit_compiled(call_both, "call_both")

        assert call_both(add_default, 1, 2) == 3
        assert call_both(add_default, 0, 0) == 0
        assert call_both(add_default, 5, 5) == 10
        print("  PASS: test_no_defaults_used")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_no_defaults_used — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Different default value types
    # ------------------------------------------------------------------
    try:
        def typed_defaults(a, b=42, c="default", d=None, e=True, f=(1, 2)):
            return (a, b, c, d, e, f)

        def call_typed(fn, a):
            return fn(a)

        for _ in range(WARMUP):
            call_typed(typed_defaults, "x")
        check_jit_compiled(call_typed, "call_typed")

        result = call_typed(typed_defaults, "x")
        assert result == ("x", 42, "default", None, True, (1, 2))
        print("  PASS: test_different_default_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_default_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Mutable default pitfall (list default shared)
    # ------------------------------------------------------------------
    try:
        def append_to(val, lst=[]):
            lst.append(val)
            return lst

        # Reset by creating a fresh function each time
        # Actually, test the pitfall directly — the list accumulates
        def call_append(f, v):
            return f(v)

        # Get a fresh function
        def make_appender():
            def appender(val, lst=[]):
                lst.append(val)
                return list(lst)  # return copy to not alias
            return appender

        appender = make_appender()

        for _ in range(WARMUP):
            # Warm up with explicit list to avoid growing default
            appender(1, [])
        check_jit_compiled(call_append, "call_append")

        # Now use the default — each call appends to same default list
        fresh = make_appender()
        r1 = fresh(10)  # default list gets [10]
        r2 = fresh(20)  # default list gets [10, 20]
        assert r1 == [10], f"Expected [10], got {r1}"
        assert r2 == [10, 20], f"Expected [10, 20], got {r2}"
        print("  PASS: test_mutable_default_pitfall")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mutable_default_pitfall — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Deopt — function with defaults -> builtin
    # ------------------------------------------------------------------
    try:
        def my_len(x, extra=0):
            return len(x) + extra

        def call_one(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_one(my_len, [1, 2, 3])
        check_jit_compiled(call_one, "call_one")

        assert call_one(my_len, [1, 2, 3]) == 3  # extra=0 default
        # Deopt to builtin len
        assert call_one(len, [1, 2, 3]) == 3
        assert call_one(len, "hello") == 5
        # Back to Python function
        assert call_one(my_len, [1]) == 1
        print("  PASS: test_deopt_to_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Deopt — defaults function -> no-defaults function
    # ------------------------------------------------------------------
    try:
        def with_default(x, y=10):
            return x + y

        def without_default(x, y):
            return x * y

        def call_two(f, a, b):
            return f(a, b)

        for _ in range(WARMUP):
            call_two(with_default, 1, 2)
        check_jit_compiled(call_two, "call_two")

        assert call_two(with_default, 1, 2) == 3
        # Deopt to function without defaults (same arity when called with 2)
        assert call_two(without_default, 3, 4) == 12
        # Back
        assert call_two(with_default, 5, 5) == 10
        print("  PASS: test_deopt_to_no_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_no_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt — different default values
    # ------------------------------------------------------------------
    try:
        def add_ten(x, y=10):
            return x + y

        def add_twenty(x, y=20):
            return x + y

        def call_default(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_default(add_ten, 1)
        check_jit_compiled(call_default, "call_default")

        assert call_default(add_ten, 1) == 11   # 1 + 10
        # Different function, different default
        assert call_default(add_twenty, 1) == 21  # 1 + 20
        # Back
        assert call_default(add_ten, 5) == 15    # 5 + 10
        assert call_default(add_twenty, 5) == 25  # 5 + 20
        print("  PASS: test_deopt_different_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Closure with defaults
    # ------------------------------------------------------------------
    try:
        def make_adder(base):
            def adder(x, extra=0):
                return base + x + extra
            return adder

        add5 = make_adder(5)
        add10 = make_adder(10)

        def call_adder(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_adder(add5, 1)
        check_jit_compiled(call_adder, "call_adder")

        assert call_adder(add5, 1) == 6     # 5 + 1 + 0
        assert call_adder(add10, 1) == 11   # 10 + 1 + 0
        assert call_adder(add5, 0) == 5     # 5 + 0 + 0
        assert call_adder(add10, 0) == 10   # 10 + 0 + 0
        print("  PASS: test_closure_with_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_closure_with_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Multiple defaulted parameters
    # ------------------------------------------------------------------
    try:
        def multi_default(a, b=2, c=3, d=4):
            return a + b + c + d

        def call_one_arg(f, a):
            return f(a)

        def call_two_args(f, a, b):
            return f(a, b)

        def call_three_args(f, a, b, c):
            return f(a, b, c)

        def call_four_args(f, a, b, c, d):
            return f(a, b, c, d)

        for _ in range(WARMUP):
            call_one_arg(multi_default, 1)
        check_jit_compiled(call_one_arg, "call_one_arg_multi")

        # 1 arg: 1 + 2 + 3 + 4 = 10
        assert call_one_arg(multi_default, 1) == 10
        # 2 args: 1 + 10 + 3 + 4 = 18
        for _ in range(WARMUP):
            call_two_args(multi_default, 1, 10)
        assert call_two_args(multi_default, 1, 10) == 18
        # 3 args: 1 + 10 + 100 + 4 = 115
        for _ in range(WARMUP):
            call_three_args(multi_default, 1, 10, 100)
        assert call_three_args(multi_default, 1, 10, 100) == 115
        # 4 args: 1 + 10 + 100 + 1000 = 1111
        for _ in range(WARMUP):
            call_four_args(multi_default, 1, 10, 100, 1000)
        assert call_four_args(multi_default, 1, 10, 100, 1000) == 1111
        print("  PASS: test_multiple_defaulted_params")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_defaulted_params — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Default None sentinel pattern
    # ------------------------------------------------------------------
    try:
        def process(data, transform=None):
            if transform is None:
                return data
            return transform(data)

        def call_process_default(f, d):
            return f(d)

        def call_process_explicit(f, d, t):
            return f(d, t)

        for _ in range(WARMUP):
            call_process_default(process, 42)
        check_jit_compiled(call_process_default, "call_process_default")

        # No transform — return data as-is
        assert call_process_default(process, 42) == 42
        assert call_process_default(process, "hello") == "hello"

        for _ in range(WARMUP):
            call_process_explicit(process, 42, str)

        # With transform
        assert call_process_explicit(process, 42, str) == "42"
        assert call_process_explicit(process, [3, 1, 2], sorted) == [1, 2, 3]
        print("  PASS: test_none_sentinel_pattern")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_none_sentinel_pattern — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Recursive function with defaults
    # ------------------------------------------------------------------
    try:
        def fib(n, a=0, b=1):
            if n == 0:
                return a
            return fib(n - 1, b, a + b)

        for _ in range(WARMUP):
            fib(10)
        check_jit_compiled(fib, "fib")

        assert fib(0) == 0
        assert fib(1) == 1
        assert fib(5) == 5
        assert fib(10) == 55
        assert fib(20) == 6765
        print("  PASS: test_recursive_with_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_recursive_with_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Method with defaults
    # ------------------------------------------------------------------
    try:
        class Formatter:
            def format(self, value, prefix="[", suffix="]"):
                return f"{prefix}{value}{suffix}"

        def call_format(obj, val):
            return obj.format(val)

        fmt = Formatter()
        for _ in range(WARMUP):
            call_format(fmt, "test")
        check_jit_compiled(call_format, "call_format")

        assert call_format(fmt, "hello") == "[hello]"
        assert call_format(fmt, 42) == "[42]"

        # With explicit args overriding defaults
        assert fmt.format("x", "(", ")") == "(x)"
        assert fmt.format("x", "<") == "<x]"
        print("  PASS: test_method_with_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid callable alternation
    # ------------------------------------------------------------------
    try:
        def inc_default(x, step=1):
            return x + step

        def dec_default(x, step=1):
            return x - step

        def call_step(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_step(inc_default, 10)
        check_jit_compiled(call_step, "call_step")

        for cycle in range(50):
            assert call_step(inc_default, 10) == 11, f"inc failed at cycle {cycle}"
            assert call_step(dec_default, 10) == 9, f"dec failed at cycle {cycle}"

        assert call_step(inc_default, 0) == 1
        assert call_step(dec_default, 0) == -1
        print("  PASS: test_rapid_callable_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_callable_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Default value identity preservation
    # ------------------------------------------------------------------
    try:
        sentinel = object()

        def get_sentinel(x, default=sentinel):
            if x is None:
                return default
            return x

        def call_sentinel(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_sentinel(get_sentinel, None)
        check_jit_compiled(call_sentinel, "call_sentinel")

        result = call_sentinel(get_sentinel, None)
        assert result is sentinel, "Default sentinel identity not preserved"

        assert call_sentinel(get_sentinel, 42) == 42
        assert call_sentinel(get_sentinel, "hello") == "hello"
        print("  PASS: test_default_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_default_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Exception propagation
    # ------------------------------------------------------------------
    try:
        def validate(x, min_val=0):
            if x < min_val:
                raise ValueError(f"{x} < {min_val}")
            return x

        def call_validate(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_validate(validate, 5)
        check_jit_compiled(call_validate, "call_validate")

        assert call_validate(validate, 5) == 5
        assert call_validate(validate, 0) == 0

        raised = False
        try:
            call_validate(validate, -1)
        except ValueError as e:
            raised = True
            assert "-1 < 0" in str(e)
        assert raised, "Expected ValueError"

        # After exception, function still works
        assert call_validate(validate, 10) == 10
        print("  PASS: test_exception_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_exception_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Loop calling with/without defaults alternating
    # ------------------------------------------------------------------
    try:
        def scale(x, factor=2):
            return x * factor

        def loop_scale(n):
            total = 0
            for i in range(n):
                total += scale(i)  # uses default factor=2
            return total

        for _ in range(WARMUP):
            loop_scale(10)
        check_jit_compiled(loop_scale, "loop_scale")

        # sum(i*2 for i in range(10)) = 2*(0+1+2+...+9) = 2*45 = 90
        assert loop_scale(10) == 90
        assert loop_scale(0) == 0
        assert loop_scale(1) == 0  # scale(0) = 0
        assert loop_scale(5) == 20  # 2*(0+1+2+3+4) = 20
        print("  PASS: test_loop_with_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_loop_with_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: All positional, some defaulted
    # ------------------------------------------------------------------
    try:
        def make_range(start, stop, step=1):
            return list(range(start, stop, step))

        def call_range_default(f, a, b):
            return f(a, b)

        def call_range_explicit(f, a, b, c):
            return f(a, b, c)

        for _ in range(WARMUP):
            call_range_default(make_range, 0, 5)
        check_jit_compiled(call_range_default, "call_range_default")

        assert call_range_default(make_range, 0, 5) == [0, 1, 2, 3, 4]
        assert call_range_default(make_range, 1, 4) == [1, 2, 3]

        for _ in range(WARMUP):
            call_range_explicit(make_range, 0, 10, 2)
        assert call_range_explicit(make_range, 0, 10, 2) == [0, 2, 4, 6, 8]
        assert call_range_explicit(make_range, 0, 10, 3) == [0, 3, 6, 9]
        print("  PASS: test_positional_some_defaulted")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_positional_some_defaulted — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Equivalence — f(x) vs f.__call__(x) with defaults
    # ------------------------------------------------------------------
    try:
        def negate(x, absolute=False):
            result = -x
            if absolute:
                result = abs(result)
            return result

        def direct_call(f, x):
            return f(x)

        def dunder_call(f, x):
            return f.__call__(x)

        for _ in range(WARMUP):
            direct_call(negate, 5)
        check_jit_compiled(direct_call, "direct_call")

        for v in [0, 1, -1, 100, -100]:
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
    # Test 20: Function with defaults modified at runtime
    # ------------------------------------------------------------------
    try:
        def add_offset(x, offset=0):
            return x + offset

        def call_offset(f, x):
            return f(x)

        for _ in range(WARMUP):
            call_offset(add_offset, 10)
        check_jit_compiled(call_offset, "call_offset")

        assert call_offset(add_offset, 10) == 10  # offset=0

        # Modify __defaults__ at runtime
        add_offset.__defaults__ = (100,)
        assert call_offset(add_offset, 10) == 110  # offset=100

        add_offset.__defaults__ = (-5,)
        assert call_offset(add_offset, 10) == 5   # offset=-5

        # Restore
        add_offset.__defaults__ = (0,)
        assert call_offset(add_offset, 10) == 10
        print("  PASS: test_defaults_modified_at_runtime")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_defaults_modified_at_runtime — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_PY_WITH_DEFAULTS: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
