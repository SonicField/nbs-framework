#!/usr/bin/env python3
"""
test_call_builtin_fast.py — Correctness and deopt tests for
CALL_BUILTIN_FAST specialisation.

Targets: CALL_BUILTIN_FAST.

CALL_BUILTIN_FAST specialises calls to C-implemented builtin functions that
use the METH_FASTCALL calling convention (positional arguments passed as a
C array + nargs). Instead of going through the generic CALL dispatch (which
must resolve the callable, pack/unpack arguments, and dispatch through
vectorcall or tp_call), the specialisation calls the C function pointer
directly with the fastcall convention.

Common METH_FASTCALL builtins: isinstance(), issubclass(), getattr(),
setattr(), hasattr(), min(), max(), sorted(), print(), pow(), round(),
divmod(), map(), filter(), zip(), enumerate(), range(), slice(), format(),
vars(), dir().

The adaptive specialiser emits CALL_BUILTIN_FAST after observing repeated
calls to the same METH_FASTCALL builtin.

Deopt triggers:
  - Callable changes (e.g. isinstance -> getattr)
  - Callable is not a METH_FASTCALL builtin (e.g. user-defined function)
  - Callable is shadowed or replaced at runtime

Tests cover:
  - isinstance() with single type and tuple of types
  - issubclass() checks
  - getattr() with and without default
  - hasattr() checks
  - min() and max() with two args
  - pow() two-arg and three-arg
  - divmod() on ints and floats
  - round() with ndigits
  - sorted() with key argument
  - format() with format spec
  - Deopt: builtin -> user function
  - Deopt: one builtin -> different builtin
  - Deopt: shadowed builtin
  - Loop with builtin calls
  - Rapid callable alternation
  - Exception propagation from builtin
  - isinstance() with custom __instancecheck__
  - map() and filter() basics
  - enumerate() with start
  - Equivalence: isinstance(x, T) vs type(x) is T (for exact types)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after callable change (deopt fires)
  3. Semantic equivalence with known-good reference implementations

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_builtin_fast.py
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
    # Test 1: isinstance() with single type
    # ------------------------------------------------------------------
    try:
        def call_isinstance(obj, tp):
            return isinstance(obj, tp)

        for _ in range(WARMUP):
            call_isinstance(42, int)
        check_jit_compiled(call_isinstance, "call_isinstance")

        assert call_isinstance(42, int) is True
        assert call_isinstance(42, str) is False
        assert call_isinstance("hello", str) is True
        assert call_isinstance(3.14, float) is True
        assert call_isinstance(True, bool) is True
        assert call_isinstance(True, int) is True  # bool is subclass of int
        assert call_isinstance(None, type(None)) is True
        print("  PASS: test_isinstance_single_type")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_single_type — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: isinstance() with tuple of types
    # ------------------------------------------------------------------
    try:
        def call_isinstance_tuple(obj, types):
            return isinstance(obj, types)

        for _ in range(WARMUP):
            call_isinstance_tuple(42, (int, str))
        check_jit_compiled(call_isinstance_tuple, "call_isinstance_tuple")

        assert call_isinstance_tuple(42, (int, str)) is True
        assert call_isinstance_tuple("x", (int, str)) is True
        assert call_isinstance_tuple(3.14, (int, str)) is False
        assert call_isinstance_tuple([], (list, tuple)) is True
        assert call_isinstance_tuple((), (list, tuple)) is True
        assert call_isinstance_tuple({}, (list, tuple)) is False
        print("  PASS: test_isinstance_tuple_of_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_tuple_of_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: issubclass() checks
    # ------------------------------------------------------------------
    try:
        def call_issubclass(cls, parent):
            return issubclass(cls, parent)

        for _ in range(WARMUP):
            call_issubclass(bool, int)
        check_jit_compiled(call_issubclass, "call_issubclass")

        assert call_issubclass(bool, int) is True
        assert call_issubclass(int, int) is True
        assert call_issubclass(int, object) is True
        assert call_issubclass(str, int) is False
        assert call_issubclass(list, object) is True

        class A:
            pass
        class B(A):
            pass
        assert call_issubclass(B, A) is True
        assert call_issubclass(A, B) is False
        print("  PASS: test_issubclass")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_issubclass — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: getattr() with and without default
    # ------------------------------------------------------------------
    try:
        def call_getattr2(obj, name):
            return getattr(obj, name)

        def call_getattr3(obj, name, default):
            return getattr(obj, name, default)

        class Obj:
            x = 10
            y = 20

        obj = Obj()
        for _ in range(WARMUP):
            call_getattr2(obj, "x")
        for _ in range(WARMUP):
            call_getattr3(obj, "x", None)
        check_jit_compiled(call_getattr2, "call_getattr2")
        check_jit_compiled(call_getattr3, "call_getattr3")

        assert call_getattr2(obj, "x") == 10
        assert call_getattr2(obj, "y") == 20

        # Without default, missing attr raises AttributeError
        raised = False
        try:
            call_getattr2(obj, "z")
        except AttributeError:
            raised = True
        assert raised, "Expected AttributeError for missing attr"

        # With default, missing attr returns default
        assert call_getattr3(obj, "z", 99) == 99
        assert call_getattr3(obj, "z", None) is None
        assert call_getattr3(obj, "x", 99) == 10  # exists, returns actual
        print("  PASS: test_getattr_with_default")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_getattr_with_default — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: hasattr() checks
    # ------------------------------------------------------------------
    try:
        def call_hasattr(obj, name):
            return hasattr(obj, name)

        class Obj2:
            x = 10

        obj = Obj2()
        for _ in range(WARMUP):
            call_hasattr(obj, "x")
        check_jit_compiled(call_hasattr, "call_hasattr")

        assert call_hasattr(obj, "x") is True
        assert call_hasattr(obj, "y") is False
        assert call_hasattr("hello", "upper") is True
        assert call_hasattr("hello", "nonexistent") is False
        assert call_hasattr([], "append") is True
        assert call_hasattr(42, "__add__") is True
        print("  PASS: test_hasattr")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_hasattr — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: min() and max() with two args
    # ------------------------------------------------------------------
    try:
        def call_min(a, b):
            return min(a, b)

        def call_max(a, b):
            return max(a, b)

        for _ in range(WARMUP):
            call_min(3, 5)
        for _ in range(WARMUP):
            call_max(3, 5)
        check_jit_compiled(call_min, "call_min")
        check_jit_compiled(call_max, "call_max")

        assert call_min(3, 5) == 3
        assert call_min(5, 3) == 3
        assert call_min(-1, 1) == -1
        assert call_min(0, 0) == 0

        assert call_max(3, 5) == 5
        assert call_max(5, 3) == 5
        assert call_max(-1, 1) == 1
        assert call_max(0, 0) == 0

        # Strings
        assert call_min("apple", "banana") == "apple"
        assert call_max("apple", "banana") == "banana"
        print("  PASS: test_min_max_two_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_min_max_two_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: pow() two-arg and three-arg
    # ------------------------------------------------------------------
    try:
        def call_pow2(base, exp):
            return pow(base, exp)

        def call_pow3(base, exp, mod):
            return pow(base, exp, mod)

        for _ in range(WARMUP):
            call_pow2(2, 10)
        for _ in range(WARMUP):
            call_pow3(2, 10, 1000)
        check_jit_compiled(call_pow2, "call_pow2")
        check_jit_compiled(call_pow3, "call_pow3")

        assert call_pow2(2, 10) == 1024
        assert call_pow2(3, 0) == 1
        assert call_pow2(5, 3) == 125
        assert call_pow2(2, -1) == 0.5

        # Three-arg pow (modular exponentiation)
        assert call_pow3(2, 10, 1000) == 24
        assert call_pow3(2, 10, 100) == 24
        assert call_pow3(3, 4, 5) == 1  # 81 % 5 == 1
        assert call_pow3(7, 3, 10) == 3  # 343 % 10 == 3
        print("  PASS: test_pow_two_three_arg")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_pow_two_three_arg — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: divmod() on ints and floats
    # ------------------------------------------------------------------
    try:
        def call_divmod(a, b):
            return divmod(a, b)

        for _ in range(WARMUP):
            call_divmod(17, 5)
        check_jit_compiled(call_divmod, "call_divmod")

        assert call_divmod(17, 5) == (3, 2)
        assert call_divmod(10, 3) == (3, 1)
        assert call_divmod(0, 5) == (0, 0)
        assert call_divmod(-17, 5) == (-4, 3)
        assert call_divmod(17, -5) == (-4, -3)

        # Float divmod
        q, r = call_divmod(7.5, 2.5)
        assert q == 3.0
        assert abs(r) < 1e-10  # remainder ~0

        # ZeroDivisionError
        raised = False
        try:
            call_divmod(1, 0)
        except ZeroDivisionError:
            raised = True
        assert raised, "Expected ZeroDivisionError"
        print("  PASS: test_divmod")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_divmod — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: round() with ndigits
    # ------------------------------------------------------------------
    try:
        def call_round2(x, n):
            return round(x, n)

        for _ in range(WARMUP):
            call_round2(3.14159, 2)
        check_jit_compiled(call_round2, "call_round2")

        assert call_round2(3.14159, 2) == 3.14
        assert call_round2(3.14159, 0) == 3.0
        assert call_round2(3.14159, 4) == 3.1416
        assert call_round2(2.5, 0) == 2.0  # banker's rounding
        assert call_round2(3.5, 0) == 4.0  # banker's rounding
        assert call_round2(1234, -2) == 1200
        assert call_round2(1250, -2) == 1200  # banker's rounding
        assert call_round2(1350, -2) == 1400  # banker's rounding
        print("  PASS: test_round_with_ndigits")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_round_with_ndigits — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: sorted() with key argument
    # ------------------------------------------------------------------
    try:
        def call_sorted_key(lst, key):
            return sorted(lst, key=key)

        data = ["banana", "apple", "cherry"]
        for _ in range(WARMUP):
            call_sorted_key(data, len)
        check_jit_compiled(call_sorted_key, "call_sorted_key")

        assert call_sorted_key(data, len) == ["apple", "banana", "cherry"]
        assert call_sorted_key([3, 1, 2], None) == [1, 2, 3]
        assert call_sorted_key([-3, 1, -2], abs) == [1, -2, -3]
        assert call_sorted_key([], len) == []
        print("  PASS: test_sorted_with_key")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_sorted_with_key — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: format() with format spec
    # ------------------------------------------------------------------
    try:
        def call_format(value, spec):
            return format(value, spec)

        for _ in range(WARMUP):
            call_format(3.14, ".2f")
        check_jit_compiled(call_format, "call_format")

        assert call_format(3.14, ".2f") == "3.14"
        assert call_format(3.14159, ".4f") == "3.1416"
        assert call_format(42, "d") == "42"
        assert call_format(42, "08d") == "00000042"
        assert call_format(255, "x") == "ff"
        assert call_format(255, "X") == "FF"
        assert call_format(255, "b") == "11111111"
        assert call_format(0.5, ".0%") == "50%"
        print("  PASS: test_format_with_spec")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_format_with_spec — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt builtin -> user function
    # ------------------------------------------------------------------
    try:
        def user_isinstance(obj, tp):
            return "custom"

        def call_fn2(fn, a, b):
            return fn(a, b)

        for _ in range(WARMUP):
            call_fn2(isinstance, 42, int)
        check_jit_compiled(call_fn2, "call_fn2")

        assert call_fn2(isinstance, 42, int) is True
        # Deopt: switch to user function
        assert call_fn2(user_isinstance, 42, int) == "custom"
        # Back to builtin
        assert call_fn2(isinstance, "x", str) is True
        # Lambda
        assert call_fn2(lambda a, b: a + b, 3, 4) == 7
        print("  PASS: test_deopt_builtin_to_user_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_builtin_to_user_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Deopt one builtin -> different builtin
    # ------------------------------------------------------------------
    try:
        def call_two_arg(fn, a, b):
            return fn(a, b)

        for _ in range(WARMUP):
            call_two_arg(isinstance, 42, int)
        check_jit_compiled(call_two_arg, "call_two_arg")

        assert call_two_arg(isinstance, 42, int) is True
        # Switch to different builtins
        assert call_two_arg(pow, 2, 10) == 1024
        assert call_two_arg(divmod, 17, 5) == (3, 2)
        assert call_two_arg(min, 3, 7) == 3
        assert call_two_arg(max, 3, 7) == 7
        # Back to isinstance
        assert call_two_arg(isinstance, "x", str) is True
        print("  PASS: test_deopt_builtin_to_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_builtin_to_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Deopt shadowed builtin
    # ------------------------------------------------------------------
    try:
        def call_isinstance_direct(obj, tp):
            return isinstance(obj, tp)

        for _ in range(WARMUP):
            call_isinstance_direct(42, int)
        check_jit_compiled(call_isinstance_direct, "call_isinstance_direct")

        assert call_isinstance_direct(42, int) is True

        original_isinstance = isinstance
        try:
            globals()["isinstance"] = lambda obj, tp: "shadowed"
            assert call_isinstance_direct(42, int) == "shadowed"
        finally:
            globals()["isinstance"] = original_isinstance

        # After restoring, should work again
        assert call_isinstance_direct(42, int) is True
        print("  PASS: test_deopt_shadowed_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_shadowed_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Loop with builtin calls
    # ------------------------------------------------------------------
    try:
        def count_ints(items):
            count = 0
            for item in items:
                if isinstance(item, int):
                    count += 1
            return count

        data = [1, "two", 3, 4.0, 5, None, 7, "eight", 9, True]
        for _ in range(WARMUP):
            count_ints(data)
        check_jit_compiled(count_ints, "count_ints")

        # True is bool (subclass of int), so isinstance(True, int) is True
        assert count_ints(data) == 6  # 1, 3, 5, 7, 9, True
        assert count_ints([]) == 0
        assert count_ints([1, 2, 3]) == 3
        assert count_ints(["a", "b"]) == 0
        assert count_ints(list(range(100))) == 100
        print("  PASS: test_loop_builtin_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_loop_builtin_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Rapid callable alternation
    # ------------------------------------------------------------------
    try:
        def call_dyn2(fn, a, b):
            return fn(a, b)

        for _ in range(WARMUP):
            call_dyn2(isinstance, 42, int)
        check_jit_compiled(call_dyn2, "call_dyn2")

        for cycle in range(50):
            r1 = call_dyn2(isinstance, 42, int)
            r2 = call_dyn2(pow, 2, 3)
            r3 = call_dyn2(min, 10, 20)
            assert r1 is True, f"isinstance failed at cycle {cycle}"
            assert r2 == 8, f"pow failed at cycle {cycle}"
            assert r3 == 10, f"min failed at cycle {cycle}"

        # Final check
        assert call_dyn2(max, 100, 200) == 200
        print("  PASS: test_rapid_callable_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_callable_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Exception propagation from builtin
    # ------------------------------------------------------------------
    try:
        def call_divmod_wrap(a, b):
            return divmod(a, b)

        for _ in range(WARMUP):
            call_divmod_wrap(10, 3)
        check_jit_compiled(call_divmod_wrap, "call_divmod_wrap")

        assert call_divmod_wrap(10, 3) == (3, 1)

        # ZeroDivisionError
        raised_zd = False
        try:
            call_divmod_wrap(1, 0)
        except ZeroDivisionError:
            raised_zd = True
        assert raised_zd, "Expected ZeroDivisionError"

        # TypeError from isinstance with bad second arg
        def call_isinstance_wrap(obj, tp):
            return isinstance(obj, tp)

        for _ in range(WARMUP):
            call_isinstance_wrap(42, int)

        raised_te = False
        try:
            call_isinstance_wrap(42, 42)  # second arg must be type/tuple
        except TypeError:
            raised_te = True
        assert raised_te, "Expected TypeError for isinstance(42, 42)"

        # TypeError from min with uncomparable types
        raised_te2 = False
        try:
            call_dyn2(min, 42, "hello")
        except TypeError:
            raised_te2 = True
        assert raised_te2, "Expected TypeError for min(int, str)"
        print("  PASS: test_exception_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_exception_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: isinstance() with custom __instancecheck__
    # ------------------------------------------------------------------
    try:
        class EvenMeta(type):
            def __instancecheck__(cls, instance):
                return isinstance(instance, int) and instance % 2 == 0

        class EvenNumber(metaclass=EvenMeta):
            pass

        def check_even(obj, tp):
            return isinstance(obj, tp)

        for _ in range(WARMUP):
            check_even(42, int)
        check_jit_compiled(check_even, "check_even")

        # Custom metaclass __instancecheck__
        assert check_even(4, EvenNumber) is True
        assert check_even(3, EvenNumber) is False
        assert check_even(0, EvenNumber) is True
        assert check_even(100, EvenNumber) is True
        assert check_even("hello", EvenNumber) is False
        # Normal isinstance still works
        assert check_even(42, int) is True
        print("  PASS: test_isinstance_custom_instancecheck")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_custom_instancecheck — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: map() and filter() basics
    # ------------------------------------------------------------------
    try:
        def call_map(fn, iterable):
            return list(map(fn, iterable))

        def call_filter(fn, iterable):
            return list(filter(fn, iterable))

        for _ in range(WARMUP):
            call_map(str, [1, 2, 3])
        for _ in range(WARMUP):
            call_filter(None, [0, 1, 2, "", "x"])
        check_jit_compiled(call_map, "call_map")
        check_jit_compiled(call_filter, "call_filter")

        assert call_map(str, [1, 2, 3]) == ["1", "2", "3"]
        assert call_map(abs, [-1, -2, 3]) == [1, 2, 3]
        assert call_map(len, ["a", "bb", "ccc"]) == [1, 2, 3]
        assert call_map(str, []) == []

        # filter with None keeps truthy values
        assert call_filter(None, [0, 1, 2, "", "x"]) == [1, 2, "x"]
        assert call_filter(None, []) == []
        assert call_filter(lambda x: x > 2, [1, 2, 3, 4, 5]) == [3, 4, 5]
        print("  PASS: test_map_filter")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_map_filter — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — isinstance(x, T) vs type(x) is T (exact)
    # ------------------------------------------------------------------
    try:
        def via_isinstance(x, T):
            return isinstance(x, T)

        def via_type_is(x, T):
            return type(x) is T

        for _ in range(WARMUP):
            via_isinstance(42, int)
        check_jit_compiled(via_isinstance, "via_isinstance")

        # For exact types (no subclass), isinstance and type-is agree
        exact_cases = [
            (42, int),
            ("hello", str),
            (3.14, float),
            ([1, 2], list),
            ((1, 2), tuple),
            ({"a": 1}, dict),
            ({1, 2}, set),
            (None, type(None)),
        ]
        for val, tp in exact_cases:
            r1 = via_isinstance(val, tp)
            r2 = via_type_is(val, tp)
            assert r1 == r2, (
                f"Mismatch for {type(val).__name__}: "
                f"isinstance={r1}, type-is={r2}"
            )

        # For subclasses, they disagree — isinstance returns True, type-is False
        assert via_isinstance(True, int) is True
        assert via_type_is(True, int) is False  # type(True) is bool, not int
        print("  PASS: test_equivalence_isinstance_vs_type_is")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_isinstance_vs_type_is — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_BUILTIN_FAST: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
