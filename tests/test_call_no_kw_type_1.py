#!/usr/bin/env python3
"""
test_call_no_kw_type_1.py — Correctness and deopt tests for CALL_NO_KW_TYPE_1 specialisation.

Targets: CALL_NO_KW_TYPE_1.

CALL_NO_KW_TYPE_1 is the CPython 3.12 specialisation for single-argument
type() calls — i.e. type(x). When the adaptive interpreter detects repeated
calls to the built-in type() with exactly one argument, it specialises the
call path to avoid the generic call machinery.

The single-argument form of type() returns the type of the argument:
  type(42) → <class 'int'>
  type("hello") → <class 'str'>
  type(obj) → obj.__class__

This is distinct from the three-argument form type(name, bases, dict) which
creates a new class — that form is NOT specialised by CALL_NO_KW_TYPE_1.

The CinderX JIT compiles CALL_NO_KW_TYPE_1 by emitting a direct type lookup,
bypassing the generic CALL dispatch.

Deopt triggers:
  - type() built-in is shadowed or replaced
  - type() is called with != 1 argument (fallback to generic)
  - Callable at the call site changes from type to something else

Tests cover:
  - type() on int, str, float, list, dict, tuple, set, bool, None, bytes
  - type() on custom class instance
  - type() on subclass instance
  - type() on lambda / function
  - type() identity: type(x) is x.__class__
  - type() on nested types (list of dicts, etc.)
  - Deopt: type() shadowed by local
  - Deopt: type() replaced in globals
  - Rapid type() calls (1000 iterations)
  - Stability — 10000 type() calls
  - type() on class object itself (metatype)
  - type() on type (metatype of type)
  - type() on dynamically created instance
  - type() with inheritance chain
  - type() on generator, iterator
  - type() on None vs other falsy values
  - type() consistency across warmup
  - Equivalence: type(x) vs x.__class__

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type() replacement

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_no_kw_type_1.py
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

    print("=== CALL_NO_KW_TYPE_1 Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: type() on int ───────────────────────────────────────────

    def call_type_int(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_int(42)
        check_jit_compiled(call_type_int, "call_type_int")

        result = call_type_int(42)
        assert result is int, f"Expected int, got {result}"
        result2 = call_type_int(-1)
        assert result2 is int, f"Expected int, got {result2}"
        result3 = call_type_int(0)
        assert result3 is int, f"Expected int, got {result3}"
        print("  PASS: test_type_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_int — {e}")
        failed += 1

    # ── Test 2: type() on str ───────────────────────────────────────────

    def call_type_str(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_str("hello")
        check_jit_compiled(call_type_str, "call_type_str")

        result = call_type_str("hello")
        assert result is str, f"Expected str, got {result}"
        result2 = call_type_str("")
        assert result2 is str, f"Expected str for empty string, got {result2}"
        print("  PASS: test_type_str")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_str — {e}")
        failed += 1

    # ── Test 3: type() on float, list, dict, tuple ──────────────────────

    def call_type_misc(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_misc(3.14)
        check_jit_compiled(call_type_misc, "call_type_misc")

        assert call_type_misc(3.14) is float, "float"
        assert call_type_misc([1, 2]) is list, "list"
        assert call_type_misc({"a": 1}) is dict, "dict"
        assert call_type_misc((1, 2)) is tuple, "tuple"
        assert call_type_misc({1, 2}) is set, "set"
        assert call_type_misc(b"abc") is bytes, "bytes"
        print("  PASS: test_type_builtin_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_builtin_types — {e}")
        failed += 1

    # ── Test 4: type() on bool and None ─────────────────────────────────

    def call_type_special(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_special(True)
        check_jit_compiled(call_type_special, "call_type_special")

        assert call_type_special(True) is bool, f"Expected bool, got {call_type_special(True)}"
        assert call_type_special(False) is bool, f"Expected bool, got {call_type_special(False)}"
        assert call_type_special(None) is type(None), f"Expected NoneType"
        print("  PASS: test_type_bool_none")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_bool_none — {e}")
        failed += 1

    # ── Test 5: type() on custom class instance ─────────────────────────

    class MyClass:
        pass

    def call_type_custom(x):
        return type(x)

    try:
        obj = MyClass()
        for _ in range(WARMUP):
            call_type_custom(obj)
        check_jit_compiled(call_type_custom, "call_type_custom")

        result = call_type_custom(obj)
        assert result is MyClass, f"Expected MyClass, got {result}"
        print("  PASS: test_type_custom_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_custom_class — {e}")
        failed += 1

    # ── Test 6: type() on subclass instance ─────────────────────────────

    class Base:
        pass

    class Derived(Base):
        pass

    def call_type_subclass(x):
        return type(x)

    try:
        d = Derived()
        for _ in range(WARMUP):
            call_type_subclass(d)
        check_jit_compiled(call_type_subclass, "call_type_subclass")

        result = call_type_subclass(d)
        assert result is Derived, f"Expected Derived, got {result}"
        assert result is not Base, "Should be Derived, not Base"
        # type() returns the concrete type, not the parent
        b = Base()
        assert call_type_subclass(b) is Base, "Base instance should return Base"
        print("  PASS: test_type_subclass")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_subclass — {e}")
        failed += 1

    # ── Test 7: type() identity — type(x) is x.__class__ ───────────────

    def call_type_identity(x):
        return type(x)

    try:
        test_values = [42, "hello", 3.14, [1], {}, (1,), True, None, b"x"]
        for _ in range(WARMUP):
            call_type_identity(42)
        check_jit_compiled(call_type_identity, "call_type_identity")

        for val in test_values:
            t = call_type_identity(val)
            assert t is val.__class__, (
                f"type({val!r}) returned {t}, but __class__ is {val.__class__}"
            )
        print("  PASS: test_type_identity_with_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_identity_with_class — {e}")
        failed += 1

    # ── Test 8: type() on function and lambda ───────────────────────────

    def some_func():
        pass

    some_lambda = lambda: None

    def call_type_callable(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_callable(some_func)
        check_jit_compiled(call_type_callable, "call_type_callable")

        from types import FunctionType
        assert call_type_callable(some_func) is FunctionType, "function"
        assert call_type_callable(some_lambda) is FunctionType, "lambda"
        print("  PASS: test_type_function_lambda")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_function_lambda — {e}")
        failed += 1

    # ── Test 9: type() on class object itself (metatype) ────────────────

    class Foo:
        pass

    def call_type_metatype(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_metatype(Foo)
        check_jit_compiled(call_type_metatype, "call_type_metatype")

        result = call_type_metatype(Foo)
        assert result is type, f"Expected type, got {result}"
        # type(int) is also type
        assert call_type_metatype(int) is type, "type(int) should be type"
        assert call_type_metatype(str) is type, "type(str) should be type"
        print("  PASS: test_type_metatype")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_metatype — {e}")
        failed += 1

    # ── Test 10: type() on type itself ──────────────────────────────────

    def call_type_of_type(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_of_type(type)
        check_jit_compiled(call_type_of_type, "call_type_of_type")

        result = call_type_of_type(type)
        assert result is type, f"type(type) should be type, got {result}"
        print("  PASS: test_type_of_type")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_of_type — {e}")
        failed += 1

    # ── Test 11: type() with deep inheritance chain ─────────────────────

    class A:
        pass

    class B(A):
        pass

    class C(B):
        pass

    class D(C):
        pass

    def call_type_deep(x):
        return type(x)

    try:
        d_obj = D()
        for _ in range(WARMUP):
            call_type_deep(d_obj)
        check_jit_compiled(call_type_deep, "call_type_deep")

        assert call_type_deep(d_obj) is D, "Should be D, the concrete type"
        assert call_type_deep(C()) is C, "Should be C"
        assert call_type_deep(B()) is B, "Should be B"
        assert call_type_deep(A()) is A, "Should be A"
        print("  PASS: test_type_deep_inheritance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_deep_inheritance — {e}")
        failed += 1

    # ── Test 12: type() on generator ────────────────────────────────────

    def my_gen():
        yield 1

    def call_type_gen(x):
        return type(x)

    try:
        from types import GeneratorType
        g = my_gen()
        for _ in range(WARMUP):
            call_type_gen(g)
        check_jit_compiled(call_type_gen, "call_type_gen")

        g2 = my_gen()
        result = call_type_gen(g2)
        assert result is GeneratorType, f"Expected generator, got {result}"
        print("  PASS: test_type_generator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_generator — {e}")
        failed += 1

    # ── Test 13: Deopt — type() shadowed by local variable ──────────────

    def call_type_shadow():
        # type is shadowed — this should deopt and use the local
        type = lambda x: "shadowed"
        return type(42)

    try:
        for _ in range(WARMUP):
            call_type_shadow()
        # Don't check JIT — shadowed type may prevent specialisation

        result = call_type_shadow()
        assert result == "shadowed", f"Expected 'shadowed', got {result}"
        print("  PASS: test_deopt_type_shadowed_local")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_type_shadowed_local — {e}")
        failed += 1

    # ── Test 14: Deopt — type() replaced in globals ─────────────────────

    _original_type = type

    def call_type_global(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_global(42)
        check_jit_compiled(call_type_global, "call_type_global")

        result_before = call_type_global(42)
        assert result_before is int, f"Before: expected int, got {result_before}"

        # Replace type in the module's globals (deopt trigger)
        globals()['type'] = lambda x: "replaced"
        try:
            result_after = call_type_global(42)
            assert result_after == "replaced", (
                f"After replacement: expected 'replaced', got {result_after}"
            )
        finally:
            # Restore type
            globals()['type'] = _original_type

        # After restoring, should work normally again
        result_restored = call_type_global(42)
        assert result_restored is int, (
            f"After restore: expected int, got {result_restored}"
        )
        print("  PASS: test_deopt_type_replaced_globals")
        passed += 1
    except Exception as e:
        globals()['type'] = _original_type  # ensure restore on failure
        print(f"  FAIL: test_deopt_type_replaced_globals — {e}")
        failed += 1

    # ── Test 15: Rapid type() calls (1000 iterations) ───────────────────

    def call_type_rapid(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_rapid("x")
        check_jit_compiled(call_type_rapid, "call_type_rapid")

        values = [42, "s", 3.14, [], {}, (), None, True, b"b", {1}]
        for i in range(1000):
            val = values[i % len(values)]
            result = call_type_rapid(val)
            assert result is val.__class__, (
                f"Iteration {i}: type({val!r}) = {result}, expected {val.__class__}"
            )
        print("  PASS: test_rapid_type_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_calls — {e}")
        failed += 1

    # ── Test 16: Stability — 10000 type() calls ────────────────────────

    def call_type_stability(x):
        return type(x)

    try:
        for _ in range(WARMUP):
            call_type_stability(99)
        check_jit_compiled(call_type_stability, "call_type_stability")

        for i in range(10000):
            result = call_type_stability(i)
            assert result is int, f"Iteration {i}: expected int, got {result}"
        print("  PASS: test_stability_10000_type_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_type_calls — {e}")
        failed += 1

    # ── Test 17: type() on dynamically created instance ─────────────────

    DynClass = type('DynClass', (object,), {'x': 10})

    def call_type_dynamic(x):
        return type(x)

    try:
        dyn_obj = DynClass()
        for _ in range(WARMUP):
            call_type_dynamic(dyn_obj)
        check_jit_compiled(call_type_dynamic, "call_type_dynamic")

        result = call_type_dynamic(dyn_obj)
        assert result is DynClass, f"Expected DynClass, got {result}"
        assert result.__name__ == 'DynClass', f"Name should be DynClass"
        print("  PASS: test_type_dynamic_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_dynamic_class — {e}")
        failed += 1

    # ── Test 18: type() in conditional — used for dispatch ──────────────

    def dispatch_by_type(x):
        t = type(x)
        if t is int:
            return "integer"
        elif t is str:
            return "string"
        elif t is list:
            return "list"
        else:
            return "other"

    try:
        for _ in range(WARMUP):
            dispatch_by_type(42)
        check_jit_compiled(dispatch_by_type, "dispatch_by_type")

        assert dispatch_by_type(42) == "integer"
        assert dispatch_by_type("hi") == "string"
        assert dispatch_by_type([1]) == "list"
        assert dispatch_by_type(3.14) == "other"
        print("  PASS: test_type_dispatch")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_dispatch — {e}")
        failed += 1

    # ── Test 19: type() on iterator ─────────────────────────────────────

    def call_type_iter(x):
        return type(x)

    try:
        it = iter([1, 2, 3])
        for _ in range(WARMUP):
            call_type_iter(it)
        check_jit_compiled(call_type_iter, "call_type_iter")

        it2 = iter([4, 5])
        result = call_type_iter(it2)
        assert result is type(iter([])), f"Expected list_iterator, got {result}"

        it3 = iter("abc")
        result3 = call_type_iter(it3)
        assert result3 is type(iter("")), f"Expected str_ascii_iterator, got {result3}"
        print("  PASS: test_type_iterator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_iterator — {e}")
        failed += 1

    # ── Test 20: Equivalence — type(x) vs x.__class__ across types ─────

    def call_type_equiv(x):
        return type(x)

    def call_class_equiv(x):
        return x.__class__

    try:
        for _ in range(WARMUP):
            call_type_equiv(42)
            call_class_equiv(42)
        check_jit_compiled(call_type_equiv, "call_type_equiv")

        test_values = [
            42, -1, 0, 3.14, 0.0, "hello", "", b"abc", b"",
            [1, 2], [], {}, {"a": 1}, (1,), (), {1, 2}, set(),
            True, False, None,
        ]
        for val in test_values:
            t1 = call_type_equiv(val)
            t2 = call_class_equiv(val)
            assert t1 is t2, (
                f"type({val!r}) = {t1} but __class__ = {t2}"
            )
        print("  PASS: test_equivalence_type_vs_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_type_vs_class — {e}")
        failed += 1

    # ── Summary ─────────────────────────────────────────────────────────

    print()
    print(f"CALL_NO_KW_TYPE_1: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
