#!/usr/bin/env python3
"""
test_call_isinstance.py — Correctness and deopt tests for CALL_ISINSTANCE
specialisation.

Targets: CALL_ISINSTANCE.

CALL_ISINSTANCE specialises isinstance(obj, type_or_tuple) calls. CPython's
adaptive interpreter replaces the generic CALL opcode with CALL_ISINSTANCE
when it detects repeated calls to the isinstance() builtin function.

The CinderX JIT then emits an optimised type check (GuardIs on the builtin
isinstance function, then direct type comparison) instead of a full
CALL through the C calling convention.

Mechanism:
1. Adaptive interpreter detects CALL to isinstance (builtin)
2. Replaces CALL with CALL_ISINSTANCE
3. CinderX JIT emits GuardIs(isinstance_func) + type check
4. Direct tp_flags / PyObject_IsInstance fast path

Deopt triggers:
  - isinstance() builtin is shadowed or replaced
  - Called with non-type second argument (falls back to generic path)
  - Called with unusual first argument (custom __instancecheck__)

Tests cover:
  - Basic isinstance(obj, type) — True case
  - Basic isinstance(obj, type) — False case
  - isinstance with tuple of types (multi-type check)
  - isinstance with inheritance (subclass is instance of parent)
  - isinstance with int/float/str/list/dict/tuple builtins
  - isinstance with None/NoneType
  - isinstance with bool (subclass of int)
  - isinstance with custom class hierarchy
  - isinstance with abstract base class (__subclasshook__)
  - isinstance with custom __instancecheck__
  - Deopt: isinstance shadowed by local function
  - isinstance in conditional branch (if isinstance(...))
  - isinstance with deeply nested inheritance
  - Rapid isinstance checks (1000 cycles)
  - Stability — 10000 isinstance calls
  - isinstance with multiple inheritance (diamond)
  - isinstance with tuple of mixed types
  - isinstance result used in boolean expression
  - isinstance with object (everything is instance of object)
  - Equivalence: isinstance(x, T) vs type(x) is T for exact types

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct isinstance result when JIT-compiled (warmup -> JIT -> check)
  2. Result matches interpreter semantics exactly
  3. Edge cases (shadowing, metaclasses, ABC) handled correctly

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_isinstance.py
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
    # Test 1: Basic isinstance — True case
    # ------------------------------------------------------------------
    try:
        def check_is_int(obj):
            return isinstance(obj, int)

        for _ in range(WARMUP):
            check_is_int(42)
        check_jit_compiled(check_is_int, "check_is_int")

        assert check_is_int(0) is True
        assert check_is_int(1) is True
        assert check_is_int(-999) is True
        assert check_is_int(2**63) is True
        print("  PASS: test_basic_isinstance_true")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_isinstance_true — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Basic isinstance — False case
    # ------------------------------------------------------------------
    try:
        def check_is_str(obj):
            return isinstance(obj, str)

        for _ in range(WARMUP):
            check_is_str("hello")
        check_jit_compiled(check_is_str, "check_is_str")

        assert check_is_str(42) is False
        assert check_is_str(3.14) is False
        assert check_is_str([1, 2]) is False
        assert check_is_str(None) is False
        assert check_is_str("hello") is True  # sanity check
        print("  PASS: test_basic_isinstance_false")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_isinstance_false — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: isinstance with tuple of types
    # ------------------------------------------------------------------
    try:
        def check_numeric(obj):
            return isinstance(obj, (int, float, complex))

        for _ in range(WARMUP):
            check_numeric(42)
        check_jit_compiled(check_numeric, "check_numeric")

        assert check_numeric(42) is True
        assert check_numeric(3.14) is True
        assert check_numeric(1+2j) is True
        assert check_numeric("42") is False
        assert check_numeric([42]) is False
        assert check_numeric(None) is False
        print("  PASS: test_isinstance_tuple_of_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_tuple_of_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: isinstance with inheritance (subclass is instance of parent)
    # ------------------------------------------------------------------
    try:
        class Animal:
            pass

        class Dog(Animal):
            pass

        class Labrador(Dog):
            pass

        def check_is_animal(obj):
            return isinstance(obj, Animal)

        lab = Labrador()
        for _ in range(WARMUP):
            check_is_animal(lab)
        check_jit_compiled(check_is_animal, "check_is_animal")

        assert check_is_animal(Labrador()) is True
        assert check_is_animal(Dog()) is True
        assert check_is_animal(Animal()) is True
        assert check_is_animal("not an animal") is False
        assert check_is_animal(42) is False
        print("  PASS: test_isinstance_inheritance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_inheritance — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: isinstance with builtin types (int/float/str/list/dict/tuple)
    # ------------------------------------------------------------------
    try:
        def check_builtin_types(obj):
            return (
                isinstance(obj, int),
                isinstance(obj, float),
                isinstance(obj, str),
                isinstance(obj, list),
                isinstance(obj, dict),
                isinstance(obj, tuple),
            )

        for _ in range(WARMUP):
            check_builtin_types(42)
        check_jit_compiled(check_builtin_types, "check_builtin_types")

        assert check_builtin_types(42) == (True, False, False, False, False, False)
        assert check_builtin_types(3.14) == (False, True, False, False, False, False)
        assert check_builtin_types("hi") == (False, False, True, False, False, False)
        assert check_builtin_types([1]) == (False, False, False, True, False, False)
        assert check_builtin_types({"a": 1}) == (False, False, False, False, True, False)
        assert check_builtin_types((1,)) == (False, False, False, False, False, True)
        print("  PASS: test_isinstance_builtin_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_builtin_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: isinstance with None/NoneType
    # ------------------------------------------------------------------
    try:
        def check_is_none_type(obj):
            return isinstance(obj, type(None))

        for _ in range(WARMUP):
            check_is_none_type(None)
        check_jit_compiled(check_is_none_type, "check_is_none_type")

        assert check_is_none_type(None) is True
        assert check_is_none_type(0) is False
        assert check_is_none_type("") is False
        assert check_is_none_type(False) is False
        print("  PASS: test_isinstance_nonetype")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_nonetype — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: isinstance with bool (subclass of int)
    # ------------------------------------------------------------------
    try:
        def check_is_int_bool(obj):
            return isinstance(obj, int)

        def check_is_bool(obj):
            return isinstance(obj, bool)

        for _ in range(WARMUP):
            check_is_int_bool(True)
            check_is_bool(True)
        check_jit_compiled(check_is_int_bool, "check_is_int_bool")
        check_jit_compiled(check_is_bool, "check_is_bool")

        # bool is a subclass of int
        assert check_is_int_bool(True) is True
        assert check_is_int_bool(False) is True
        assert check_is_int_bool(1) is True

        assert check_is_bool(True) is True
        assert check_is_bool(False) is True
        assert check_is_bool(1) is False   # int is NOT bool
        assert check_is_bool(0) is False   # int is NOT bool
        print("  PASS: test_isinstance_bool_subclass_of_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_bool_subclass_of_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: isinstance with custom class hierarchy
    # ------------------------------------------------------------------
    try:
        class Shape:
            pass

        class Circle(Shape):
            pass

        class Square(Shape):
            pass

        class ColoredCircle(Circle):
            pass

        def check_is_shape(obj):
            return isinstance(obj, Shape)

        def check_is_circle(obj):
            return isinstance(obj, Circle)

        cc = ColoredCircle()
        for _ in range(WARMUP):
            check_is_shape(cc)
            check_is_circle(cc)
        check_jit_compiled(check_is_shape, "check_is_shape")
        check_jit_compiled(check_is_circle, "check_is_circle")

        assert check_is_shape(ColoredCircle()) is True
        assert check_is_shape(Circle()) is True
        assert check_is_shape(Square()) is True
        assert check_is_circle(ColoredCircle()) is True
        assert check_is_circle(Circle()) is True
        assert check_is_circle(Square()) is False
        print("  PASS: test_isinstance_custom_hierarchy")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_custom_hierarchy — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: isinstance with ABC (__subclasshook__)
    # ------------------------------------------------------------------
    try:
        from abc import ABC, abstractmethod

        class Printable(ABC):
            @abstractmethod
            def display(self):
                pass

        class Document(Printable):
            def display(self):
                return "doc"

        def check_is_printable(obj):
            return isinstance(obj, Printable)

        doc = Document()
        for _ in range(WARMUP):
            check_is_printable(doc)
        check_jit_compiled(check_is_printable, "check_is_printable")

        assert check_is_printable(Document()) is True
        assert check_is_printable("not printable") is False
        assert check_is_printable(42) is False
        print("  PASS: test_isinstance_abc")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_abc — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: isinstance with custom __instancecheck__
    # ------------------------------------------------------------------
    try:
        class EvenMeta(type):
            def __instancecheck__(cls, instance):
                if isinstance(instance, int):
                    return instance % 2 == 0
                return False

        class EvenNumber(metaclass=EvenMeta):
            pass

        def check_is_even(obj):
            return isinstance(obj, EvenNumber)

        for _ in range(WARMUP):
            check_is_even(4)
        check_jit_compiled(check_is_even, "check_is_even")

        assert check_is_even(4) is True
        assert check_is_even(2) is True
        assert check_is_even(0) is True
        assert check_is_even(3) is False
        assert check_is_even(7) is False
        assert check_is_even("hello") is False
        print("  PASS: test_isinstance_custom_instancecheck")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_custom_instancecheck — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — isinstance shadowed by local function
    # ------------------------------------------------------------------
    try:
        def check_normal(obj):
            return isinstance(obj, int)

        for _ in range(WARMUP):
            check_normal(42)
        check_jit_compiled(check_normal, "check_normal")

        # Verify normal isinstance works
        assert check_normal(42) is True
        assert check_normal("x") is False

        # Shadow isinstance in a different function's scope
        def check_with_shadow(obj):
            # This function has its own isinstance — tests that the
            # specialisation in check_normal is unaffected
            def isinstance(o, t):  # noqa: F811
                return "shadowed"
            return isinstance(obj, int)

        assert check_with_shadow(42) == "shadowed"

        # Original function should still work correctly
        assert check_normal(42) is True
        assert check_normal("x") is False
        print("  PASS: test_deopt_isinstance_shadowed")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_isinstance_shadowed — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: isinstance in conditional branch
    # ------------------------------------------------------------------
    try:
        def classify(obj):
            if isinstance(obj, int):
                return "int"
            elif isinstance(obj, str):
                return "str"
            elif isinstance(obj, list):
                return "list"
            else:
                return "other"

        for _ in range(WARMUP):
            classify(42)
        check_jit_compiled(classify, "classify")

        assert classify(42) == "int"
        assert classify("hello") == "str"
        assert classify([1, 2]) == "list"
        assert classify(3.14) == "other"
        assert classify(None) == "other"
        assert classify(True) == "int"  # bool is subclass of int
        print("  PASS: test_isinstance_in_conditional")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_in_conditional — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: isinstance with deeply nested inheritance (10 levels)
    # ------------------------------------------------------------------
    try:
        class L0: pass
        class L1(L0): pass
        class L2(L1): pass
        class L3(L2): pass
        class L4(L3): pass
        class L5(L4): pass
        class L6(L5): pass
        class L7(L6): pass
        class L8(L7): pass
        class L9(L8): pass

        def check_is_l0(obj):
            return isinstance(obj, L0)

        def check_is_l5(obj):
            return isinstance(obj, L5)

        leaf = L9()
        for _ in range(WARMUP):
            check_is_l0(leaf)
            check_is_l5(leaf)
        check_jit_compiled(check_is_l0, "check_is_l0")
        check_jit_compiled(check_is_l5, "check_is_l5")

        assert check_is_l0(L9()) is True   # 10 levels up
        assert check_is_l0(L5()) is True   # 5 levels up
        assert check_is_l0(L0()) is True   # exact match
        assert check_is_l5(L9()) is True   # 4 levels up
        assert check_is_l5(L4()) is False  # L4 is NOT a subclass of L5
        assert check_is_l5(L0()) is False
        print("  PASS: test_isinstance_deep_inheritance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_deep_inheritance — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid isinstance checks (1000 cycles)
    # ------------------------------------------------------------------
    try:
        class Marker:
            pass

        def rapid_check(obj):
            return isinstance(obj, Marker)

        m = Marker()
        for _ in range(WARMUP):
            rapid_check(m)
        check_jit_compiled(rapid_check, "rapid_check")

        for i in range(1000):
            assert rapid_check(m) is True, f"cycle {i}: expected True"
            assert rapid_check(i) is False, f"cycle {i}: expected False for int"
        print("  PASS: test_rapid_isinstance_checks")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_isinstance_checks — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Stability — 10000 isinstance calls
    # ------------------------------------------------------------------
    try:
        class StableClass:
            pass

        def stable_check(obj):
            return isinstance(obj, StableClass)

        sc = StableClass()
        for _ in range(WARMUP):
            stable_check(sc)
        check_jit_compiled(stable_check, "stable_check")

        for i in range(10000):
            result = stable_check(sc)
            assert result is True, f"iteration {i}: got {result}, expected True"
        print("  PASS: test_stability_10000_isinstance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_isinstance — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: isinstance with multiple inheritance (diamond)
    # ------------------------------------------------------------------
    try:
        class A:
            pass

        class B(A):
            pass

        class C(A):
            pass

        class D(B, C):  # diamond: D -> B -> A, D -> C -> A
            pass

        def check_diamond_a(obj):
            return isinstance(obj, A)

        def check_diamond_b(obj):
            return isinstance(obj, B)

        def check_diamond_c(obj):
            return isinstance(obj, C)

        d = D()
        for _ in range(WARMUP):
            check_diamond_a(d)
            check_diamond_b(d)
            check_diamond_c(d)
        check_jit_compiled(check_diamond_a, "check_diamond_a")
        check_jit_compiled(check_diamond_b, "check_diamond_b")
        check_jit_compiled(check_diamond_c, "check_diamond_c")

        # D is instance of all of A, B, C
        assert check_diamond_a(D()) is True
        assert check_diamond_b(D()) is True
        assert check_diamond_c(D()) is True

        # B is instance of A but not C
        assert check_diamond_a(B()) is True
        assert check_diamond_c(B()) is False

        # C is instance of A but not B
        assert check_diamond_a(C()) is True
        assert check_diamond_b(C()) is False
        print("  PASS: test_isinstance_diamond_inheritance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_diamond_inheritance — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: isinstance with tuple of mixed types (builtin + custom)
    # ------------------------------------------------------------------
    try:
        class Widget:
            pass

        def check_mixed_tuple(obj):
            return isinstance(obj, (int, str, Widget))

        w = Widget()
        for _ in range(WARMUP):
            check_mixed_tuple(42)
        check_jit_compiled(check_mixed_tuple, "check_mixed_tuple")

        assert check_mixed_tuple(42) is True
        assert check_mixed_tuple("hello") is True
        assert check_mixed_tuple(Widget()) is True
        assert check_mixed_tuple(3.14) is False
        assert check_mixed_tuple([]) is False
        assert check_mixed_tuple(None) is False
        print("  PASS: test_isinstance_mixed_type_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_mixed_type_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: isinstance result used in boolean expression
    # ------------------------------------------------------------------
    try:
        def is_int_and_positive(obj):
            return isinstance(obj, int) and obj > 0

        def is_str_or_list(obj):
            return isinstance(obj, str) or isinstance(obj, list)

        for _ in range(WARMUP):
            is_int_and_positive(42)
            is_str_or_list("hi")
        check_jit_compiled(is_int_and_positive, "is_int_and_positive")
        check_jit_compiled(is_str_or_list, "is_str_or_list")

        assert is_int_and_positive(42) is True
        assert is_int_and_positive(-1) is False
        assert is_int_and_positive(0) is False
        assert is_int_and_positive("42") is False

        assert is_str_or_list("hi") is True
        assert is_str_or_list([1]) is True
        assert is_str_or_list(42) is False
        assert is_str_or_list(None) is False
        print("  PASS: test_isinstance_in_boolean_expr")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_in_boolean_expr — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: isinstance with object (everything is instance of object)
    # ------------------------------------------------------------------
    try:
        class Anything:
            pass

        def check_is_object(obj):
            return isinstance(obj, object)

        for _ in range(WARMUP):
            check_is_object(42)
        check_jit_compiled(check_is_object, "check_is_object")

        # Everything in Python is an instance of object
        assert check_is_object(42) is True
        assert check_is_object("hello") is True
        assert check_is_object(3.14) is True
        assert check_is_object(None) is True
        assert check_is_object([]) is True
        assert check_is_object({}) is True
        assert check_is_object(Anything()) is True
        assert check_is_object(object()) is True
        print("  PASS: test_isinstance_object_universal")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_isinstance_object_universal — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — isinstance(x, T) vs type(x) is T (exact)
    # ------------------------------------------------------------------
    try:
        def check_isinstance_int(obj):
            return isinstance(obj, int)

        def check_type_is_int(obj):
            return type(obj) is int

        for _ in range(WARMUP):
            check_isinstance_int(42)
            check_type_is_int(42)
        check_jit_compiled(check_isinstance_int, "check_isinstance_int")
        check_jit_compiled(check_type_is_int, "check_type_is_int")

        # For exact types (no subclass), isinstance and type() is should agree
        test_values = [42, 0, -1, 2**30]
        for val in test_values:
            r_isinstance = check_isinstance_int(val)
            r_type_is = check_type_is_int(val)
            assert r_isinstance == r_type_is, (
                f"Mismatch for {val}: isinstance={r_isinstance}, "
                f"type is={r_type_is}"
            )

        # For subclass (bool), they should DIFFER
        # isinstance(True, int) is True, but type(True) is int is False
        assert check_isinstance_int(True) is True
        assert check_type_is_int(True) is False

        # For non-int, both return False
        for val in ["hello", 3.14, None, []]:
            assert check_isinstance_int(val) is False
            assert check_type_is_int(val) is False
        print("  PASS: test_equivalence_isinstance_vs_type_is")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_isinstance_vs_type_is — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_ISINSTANCE: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
