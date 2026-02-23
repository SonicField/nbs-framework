#!/usr/bin/env python3
"""
test_load_attr_property.py — Correctness and deopt tests for
LOAD_ATTR_PROPERTY specialisation.

Targets: LOAD_ATTR_PROPERTY.

LOAD_ATTR_PROPERTY specialises attribute load operations (obj.attr) when
attr is a property descriptor (@property). Instead of going through the
generic LOAD_ATTR path (which calls PyObject_GetAttr → tp_getattro →
PyObject_GenericGetAttr → descriptor __get__ protocol), the specialisation
caches the property's fget function and calls it directly.

The adaptive specialiser emits LOAD_ATTR_PROPERTY after observing repeated
property accesses on instances of the same type.

Deopt triggers:
  - Object type changes (different class)
  - Property descriptor is replaced or deleted on the class
  - Object type gains a __getattribute__ override
  - Attribute is shadowed by an instance dict entry

Tests cover:
  - Basic @property access
  - Property returning different types
  - Property with computation (derived value)
  - Read-only property (no setter)
  - Property with getter and setter
  - Property raising an exception
  - Property accessing instance state
  - Deopt: switch to different class with same property name
  - Deopt: switch to object with plain attribute (not property)
  - Deopt: switch to object with __getattr__
  - Property in loop
  - Property on subclass (inherited)
  - Property on subclass (overridden)
  - Property with side effects (call counting)
  - Multiple properties in one function
  - Property returning None
  - Nested property (obj.prop returns object with prop)
  - Property vs direct fget() equivalence
  - Class property monkey-patched at runtime
  - Rapid type alternation

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (AttributeError, custom exceptions)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_property.py
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
    print("=== LOAD_ATTR_PROPERTY Correctness & Deopt Tests ===")
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

    # ── Helper classes ─────────────────────────────────────────────────

    class Circle:
        def __init__(self, radius):
            self._radius = radius

        @property
        def radius(self):
            return self._radius

        @property
        def area(self):
            return 3.14159265 * self._radius * self._radius

        @property
        def diameter(self):
            return self._radius * 2

    class Rect:
        def __init__(self, w, h):
            self._w = w
            self._h = h

        @property
        def area(self):
            return self._w * self._h

        @property
        def perimeter(self):
            return 2 * (self._w + self._h)

    # ── Test 1: Basic @property access ─────────────────────────────────

    def load_prop_1(obj):
        return obj.radius

    c = Circle(5)

    for _ in range(WARMUP):
        load_prop_1(c)

    check_jit_compiled(load_prop_1, "load_prop_1")

    try:
        assert load_prop_1(c) == 5
        c._radius = 10
        assert load_prop_1(c) == 10
        c._radius = 0
        assert load_prop_1(c) == 0
        print("PASS  Test 1: basic @property access")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic property — {e}")
        failed += 1

    # ── Test 2: Property returning different types ─────────────────────

    class TypeVary:
        def __init__(self, val):
            self._val = val

        @property
        def value(self):
            return self._val

    def load_types_2(obj):
        return obj.value

    tv = TypeVary(42)

    for _ in range(WARMUP):
        load_types_2(tv)

    check_jit_compiled(load_types_2, "load_types_2")

    try:
        assert load_types_2(tv) == 42
        tv._val = "hello"
        assert load_types_2(tv) == "hello"
        tv._val = None
        assert load_types_2(tv) is None
        tv._val = [1, 2, 3]
        assert load_types_2(tv) == [1, 2, 3]
        tv._val = {"a": 1}
        assert load_types_2(tv) == {"a": 1}
        print("PASS  Test 2: property returning different types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: type vary — {e}")
        failed += 1

    # ── Test 3: Property with computation (derived value) ──────────────

    def load_area_3(obj):
        return obj.area

    c3 = Circle(1)

    for _ in range(WARMUP):
        load_area_3(c3)

    check_jit_compiled(load_area_3, "load_area_3")

    try:
        c3._radius = 1
        area = load_area_3(c3)
        assert abs(area - 3.14159265) < 1e-6, f"area={area}"

        c3._radius = 10
        area = load_area_3(c3)
        assert abs(area - 314.159265) < 1e-4, f"area={area}"

        c3._radius = 0
        assert load_area_3(c3) == 0.0

        print("PASS  Test 3: computed property (area)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: computed — {e}")
        failed += 1

    # ── Test 4: Read-only property (no setter) ─────────────────────────

    class ReadOnly:
        def __init__(self, val):
            self._val = val

        @property
        def value(self):
            return self._val

    def load_readonly_4(obj):
        return obj.value

    ro = ReadOnly(42)

    for _ in range(WARMUP):
        load_readonly_4(ro)

    check_jit_compiled(load_readonly_4, "load_readonly_4")

    try:
        assert load_readonly_4(ro) == 42

        # Verify it is read-only
        try:
            ro.value = 99
            assert False, "expected AttributeError for read-only property"
        except AttributeError:
            pass

        # Still readable after failed set
        assert load_readonly_4(ro) == 42

        print("PASS  Test 4: read-only property")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: read-only — {e}")
        failed += 1

    # ── Test 5: Property with getter and setter ────────────────────────

    class Temperature:
        def __init__(self, celsius):
            self._celsius = celsius

        @property
        def celsius(self):
            return self._celsius

        @celsius.setter
        def celsius(self, val):
            self._celsius = val

        @property
        def fahrenheit(self):
            return self._celsius * 9 / 5 + 32

    def load_temp_5(obj):
        return obj.fahrenheit

    t = Temperature(0)

    for _ in range(WARMUP):
        load_temp_5(t)

    check_jit_compiled(load_temp_5, "load_temp_5")

    try:
        assert load_temp_5(t) == 32.0  # 0C = 32F
        t.celsius = 100
        assert load_temp_5(t) == 212.0  # 100C = 212F
        t.celsius = -40
        assert load_temp_5(t) == -40.0  # -40C = -40F
        print("PASS  Test 5: property with getter+setter (temperature)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: getter+setter — {e}")
        failed += 1

    # ── Test 6: Property raising an exception ──────────────────────────

    class Guarded:
        def __init__(self):
            self._val = None

        @property
        def value(self):
            if self._val is None:
                raise ValueError("value not set")
            return self._val

    def load_guarded_6(obj):
        return obj.value

    g = Guarded()
    g._val = 42

    for _ in range(WARMUP):
        load_guarded_6(g)

    check_jit_compiled(load_guarded_6, "load_guarded_6")

    try:
        assert load_guarded_6(g) == 42

        g._val = None
        try:
            load_guarded_6(g)
            assert False, "expected ValueError"
        except ValueError as e:
            assert "not set" in str(e)

        # Works again after error
        g._val = 99
        assert load_guarded_6(g) == 99

        print("PASS  Test 6: property raising exception")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: exception — {e}")
        failed += 1

    # ── Test 7: Property accessing instance state ──────────────────────

    class Counter:
        def __init__(self):
            self._count = 0
            self._accesses = 0

        @property
        def count(self):
            self._accesses += 1
            return self._count

    def load_count_7(obj):
        return obj.count

    ctr = Counter()
    ctr._count = 10

    for _ in range(WARMUP):
        load_count_7(ctr)

    check_jit_compiled(load_count_7, "load_count_7")

    try:
        ctr._accesses = 0
        ctr._count = 42
        assert load_count_7(ctr) == 42
        assert ctr._accesses == 1
        load_count_7(ctr)
        load_count_7(ctr)
        assert ctr._accesses == 3
        print("PASS  Test 7: property accessing instance state")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: instance state — {e}")
        failed += 1

    # ── Test 8: Deopt — different class with same property name ────────

    def load_area_8(obj):
        return obj.area

    for _ in range(WARMUP):
        load_area_8(Circle(5))

    check_jit_compiled(load_area_8, "load_area_8")

    try:
        c8 = Circle(5)
        area_c = load_area_8(c8)
        assert abs(area_c - 3.14159265 * 25) < 1e-4

        # Rect has same property name but different computation
        r8 = Rect(3, 4)
        assert load_area_8(r8) == 12

        # Circle still works
        assert abs(load_area_8(c8) - area_c) < 1e-10

        print("PASS  Test 8: deopt — different class, same property name")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt class — {e}")
        failed += 1

    # ── Test 9: Deopt — plain attribute (not property) ─────────────────

    class PlainArea:
        def __init__(self, area):
            self.area = area  # Plain attribute, not a property

    def load_area_9(obj):
        return obj.area

    for _ in range(WARMUP):
        load_area_9(Circle(5))

    check_jit_compiled(load_area_9, "load_area_9")

    try:
        c9 = Circle(3)
        assert abs(load_area_9(c9) - 3.14159265 * 9) < 1e-4

        # Plain attribute (deopt — not a property descriptor)
        pa = PlainArea(42)
        assert load_area_9(pa) == 42

        # Circle still works
        assert abs(load_area_9(c9) - 3.14159265 * 9) < 1e-4

        print("PASS  Test 9: deopt — plain attribute vs property")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt plain — {e}")
        failed += 1

    # ── Test 10: Deopt — object with __getattr__ ───────────────────────

    class WithGetattr:
        def __getattr__(self, name):
            if name == "area":
                return 999
            raise AttributeError(name)

    def load_area_10(obj):
        return obj.area

    for _ in range(WARMUP):
        load_area_10(Circle(5))

    check_jit_compiled(load_area_10, "load_area_10")

    try:
        c10 = Circle(2)
        assert abs(load_area_10(c10) - 3.14159265 * 4) < 1e-4

        wg = WithGetattr()
        assert load_area_10(wg) == 999

        assert abs(load_area_10(c10) - 3.14159265 * 4) < 1e-4

        print("PASS  Test 10: deopt — __getattr__ fallback")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: deopt __getattr__ — {e}")
        failed += 1

    # ── Test 11: Property in loop ──────────────────────────────────────

    def sum_prop_11(obj, n):
        total = 0
        for _ in range(n):
            total += obj.radius
        return total

    c11 = Circle(7)

    for _ in range(WARMUP):
        sum_prop_11(c11, 5)

    check_jit_compiled(sum_prop_11, "sum_prop_11")

    try:
        assert sum_prop_11(c11, 100) == 700
        c11._radius = 3
        assert sum_prop_11(c11, 10) == 30
        assert sum_prop_11(c11, 0) == 0
        print("PASS  Test 11: property in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: loop — {e}")
        failed += 1

    # ── Test 12: Property on subclass (inherited) ──────────────────────

    class Sphere(Circle):
        def __init__(self, radius):
            super().__init__(radius)

        @property
        def volume(self):
            return (4 / 3) * 3.14159265 * self._radius ** 3

    def load_inherited_12(obj):
        return obj.radius  # Inherited from Circle

    sp = Sphere(3)

    for _ in range(WARMUP):
        load_inherited_12(sp)

    check_jit_compiled(load_inherited_12, "load_inherited_12")

    try:
        assert load_inherited_12(sp) == 3
        sp._radius = 10
        assert load_inherited_12(sp) == 10

        # Circle also works
        c12 = Circle(7)
        assert load_inherited_12(c12) == 7

        print("PASS  Test 12: inherited property from subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: inherited — {e}")
        failed += 1

    # ── Test 13: Property on subclass (overridden) ─────────────────────

    class SpecialCircle(Circle):
        @property
        def area(self):
            # Override: returns int instead of float
            return int(3.14159265 * self._radius * self._radius)

    def load_overridden_13(obj):
        return obj.area

    for _ in range(WARMUP):
        load_overridden_13(Circle(5))

    check_jit_compiled(load_overridden_13, "load_overridden_13")

    try:
        c13 = Circle(5)
        area_float = load_overridden_13(c13)
        assert isinstance(area_float, float)

        sc = SpecialCircle(5)
        area_int = load_overridden_13(sc)
        assert isinstance(area_int, int)
        assert area_int == 78  # int(3.14159265 * 25)

        # Circle still works
        assert isinstance(load_overridden_13(c13), float)

        print("PASS  Test 13: overridden property on subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: overridden — {e}")
        failed += 1

    # ── Test 14: Property with side effects (call counting) ────────────

    class Counted:
        def __init__(self, val):
            self._val = val
            self.call_count = 0

        @property
        def value(self):
            self.call_count += 1
            return self._val

    def load_counted_14(obj):
        return obj.value

    cnt = Counted(42)

    for _ in range(WARMUP):
        load_counted_14(cnt)

    check_jit_compiled(load_counted_14, "load_counted_14")

    try:
        cnt.call_count = 0
        assert load_counted_14(cnt) == 42
        assert cnt.call_count == 1
        load_counted_14(cnt)
        load_counted_14(cnt)
        assert cnt.call_count == 3

        # Each access is a new call
        for _ in range(10):
            load_counted_14(cnt)
        assert cnt.call_count == 13

        print("PASS  Test 14: property with side effects (call counting)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: counting — {e}")
        failed += 1

    # ── Test 15: Multiple properties in one function ───────────────────

    def load_multi_15(obj):
        return obj.radius, obj.diameter, obj.area

    c15 = Circle(5)

    for _ in range(WARMUP):
        load_multi_15(c15)

    check_jit_compiled(load_multi_15, "load_multi_15")

    try:
        r, d, a = load_multi_15(c15)
        assert r == 5
        assert d == 10
        assert abs(a - 3.14159265 * 25) < 1e-4

        c15._radius = 1
        r, d, a = load_multi_15(c15)
        assert r == 1
        assert d == 2
        assert abs(a - 3.14159265) < 1e-6

        print("PASS  Test 15: multiple properties in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: multi prop — {e}")
        failed += 1

    # ── Test 16: Property returning None ────────────────────────────────

    class NoneHolder:
        @property
        def value(self):
            return None

    def load_none_16(obj):
        return obj.value

    nh = NoneHolder()

    for _ in range(WARMUP):
        load_none_16(nh)

    check_jit_compiled(load_none_16, "load_none_16")

    try:
        assert load_none_16(nh) is None
        # Repeated access
        for _ in range(100):
            assert load_none_16(nh) is None
        print("PASS  Test 16: property returning None")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: None — {e}")
        failed += 1

    # ── Test 17: Nested property ───────────────────────────────────────

    class Outer:
        def __init__(self):
            self._inner = Circle(5)

        @property
        def inner(self):
            return self._inner

    def load_nested_17(obj):
        return obj.inner.radius

    outer = Outer()

    for _ in range(WARMUP):
        load_nested_17(outer)

    check_jit_compiled(load_nested_17, "load_nested_17")

    try:
        assert load_nested_17(outer) == 5
        outer._inner._radius = 10
        assert load_nested_17(outer) == 10
        outer._inner = Circle(3)
        assert load_nested_17(outer) == 3
        print("PASS  Test 17: nested property (obj.prop.attr)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: nested — {e}")
        failed += 1

    # ── Test 18: Property vs direct fget() equivalence ─────────────────

    def load_prop_18(obj):
        return obj.radius

    c18 = Circle(7)

    for _ in range(WARMUP):
        load_prop_18(c18)

    check_jit_compiled(load_prop_18, "load_prop_18")

    try:
        fget = Circle.radius.fget
        for r in [0, 1, 5, 10, 100, -1]:
            c18._radius = r
            prop_result = load_prop_18(c18)
            fget_result = fget(c18)
            assert prop_result == fget_result, (
                f"mismatch for r={r}: prop={prop_result}, fget={fget_result}"
            )
        print("PASS  Test 18: property matches direct fget() call")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: fget equiv — {e}")
        failed += 1

    # ── Test 19: Monkey-patch property at runtime ──────────────────────

    class Patchable:
        def __init__(self, val):
            self._val = val

        @property
        def value(self):
            return self._val

    def load_patch_19(obj):
        return obj.value

    pat = Patchable(42)

    for _ in range(WARMUP):
        load_patch_19(pat)

    check_jit_compiled(load_patch_19, "load_patch_19")

    try:
        assert load_patch_19(pat) == 42

        # Monkey-patch the property
        Patchable.value = property(lambda self: self._val * 100)
        assert load_patch_19(pat) == 4200

        # Restore
        Patchable.value = property(lambda self: self._val)
        assert load_patch_19(pat) == 42

        print("PASS  Test 19: monkey-patch property at runtime")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: monkey-patch — {e}")
        failed += 1

    # ── Test 20: Rapid type alternation ────────────────────────────────

    def load_area_20(obj):
        return obj.area

    for _ in range(WARMUP):
        load_area_20(Circle(5))

    check_jit_compiled(load_area_20, "load_area_20")

    try:
        c20 = Circle(5)
        r20 = Rect(3, 4)
        pa20 = PlainArea(999)

        for i in range(50):
            if i % 3 == 0:
                result = load_area_20(c20)
                assert abs(result - 3.14159265 * 25) < 1e-4
            elif i % 3 == 1:
                assert load_area_20(r20) == 12
            else:
                assert load_area_20(pa20) == 999

        # Final Circle check
        assert abs(load_area_20(c20) - 3.14159265 * 25) < 1e-4

        print("PASS  Test 20: rapid type alternation (50 cycles)")
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
