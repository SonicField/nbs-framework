#!/usr/bin/env python3
"""
test_load_attr_property.py — Correctness and deopt tests for
LOAD_ATTR_PROPERTY specialisation.

Targets: LOAD_ATTR_PROPERTY.

LOAD_ATTR_PROPERTY specialises attribute load operations (obj.attr) when
the attribute is defined as a @property on the class. Instead of going
through the generic LOAD_ATTR path (which must check descriptors, instance
dict, and class dict), the specialisation directly calls the property's
fget function.

The adaptive specialiser emits LOAD_ATTR_PROPERTY after observing repeated
attribute access on instances whose type has a property descriptor for the
accessed attribute name. The specialisation caches the property descriptor
and calls fget directly, bypassing the full descriptor protocol.

Deopt triggers:
  - Object type changes (different class with different property)
  - Property descriptor is replaced with a plain attribute
  - Property descriptor is deleted from the class
  - Object switches to a type without property for that attribute
  - Property descriptor is replaced with a different descriptor type

Tests cover:
  - Basic property getter
  - Computed property (derived from other attributes)
  - Property with getter, setter, and deleter
  - Property returning different types
  - Property with side effects (access counter)
  - Property raising exception
  - Inherited property
  - Overridden property in subclass
  - Deopt: replace property with plain attribute
  - Deopt: switch to class without property
  - Deopt: switch to class with different property
  - Property in loop
  - Multiple properties on same class
  - Property returning None
  - Cached property pattern (manual)
  - Property with __slots__
  - Property on dynamically created class (type())
  - Rapid type alternation with property
  - Property deleted from class at runtime
  - Property access vs fget equivalence

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type/descriptor change (deopt fires)
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

    # ------------------------------------------------------------------ #
    # Test 1: Basic property getter
    # ------------------------------------------------------------------ #
    try:
        class BasicProp:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

        def get_value(obj):
            return obj.value

        obj = BasicProp(42)
        for _ in range(WARMUP):
            get_value(obj)

        check_jit_compiled(get_value, "get_value")
        result = get_value(obj)
        assert result == 42, f"Expected 42, got {result}"
        obj2 = BasicProp(99)
        assert get_value(obj2) == 99, "Property should work with different instance"
        print("  PASS: test_basic_property_getter")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_property_getter — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Computed property (derived from other attributes)
    # ------------------------------------------------------------------ #
    try:
        class Rectangle:
            def __init__(self, w, h):
                self.width = w
                self.height = h

            @property
            def area(self):
                return self.width * self.height

        def get_area(obj):
            return obj.area

        rect = Rectangle(3, 7)
        for _ in range(WARMUP):
            get_area(rect)

        check_jit_compiled(get_area, "get_area")
        assert get_area(rect) == 21, f"Expected 21, got {get_area(rect)}"
        rect.width = 10
        assert get_area(rect) == 70, "Computed property should reflect updated attributes"
        print("  PASS: test_computed_property")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_computed_property — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Property with getter, setter, and deleter
    # ------------------------------------------------------------------ #
    try:
        class FullProp:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

            @value.setter
            def value(self, v):
                self._x = v * 2  # Doubles on set

            @value.deleter
            def value(self):
                self._x = 0

        def read_value(obj):
            return obj.value

        obj = FullProp(5)
        for _ in range(WARMUP):
            read_value(obj)

        check_jit_compiled(read_value, "read_value")
        assert read_value(obj) == 5, "Getter should return 5"
        obj.value = 10
        assert read_value(obj) == 20, "Setter doubles, getter returns 20"
        del obj.value
        assert read_value(obj) == 0, "Deleter resets to 0"
        print("  PASS: test_property_getter_setter_deleter")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_getter_setter_deleter — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Property returning different types
    # ------------------------------------------------------------------ #
    try:
        class MultiType:
            def __init__(self, val):
                self._val = val

            @property
            def data(self):
                return self._val

        def get_data(obj):
            return obj.data

        obj_int = MultiType(42)
        obj_str = MultiType("hello")
        obj_list = MultiType([1, 2, 3])
        obj_none = MultiType(None)

        for _ in range(WARMUP):
            get_data(obj_int)

        check_jit_compiled(get_data, "get_data")
        assert get_data(obj_int) == 42
        assert get_data(obj_str) == "hello"
        assert get_data(obj_list) == [1, 2, 3]
        assert get_data(obj_none) is None
        print("  PASS: test_property_returning_different_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_returning_different_types — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: Property with side effects (access counter)
    # ------------------------------------------------------------------ #
    try:
        class Counted:
            def __init__(self, x):
                self._x = x
                self.access_count = 0

            @property
            def value(self):
                self.access_count += 1
                return self._x

        def get_counted(obj):
            return obj.value

        obj = Counted(7)
        obj.access_count = 0
        for _ in range(WARMUP):
            get_counted(obj)

        check_jit_compiled(get_counted, "get_counted")
        count_before = obj.access_count
        result = get_counted(obj)
        assert result == 7
        assert obj.access_count == count_before + 1, (
            f"Property getter must be called each time. "
            f"Before: {count_before}, after: {obj.access_count}"
        )
        print("  PASS: test_property_with_side_effects")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_with_side_effects — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: Property raising exception
    # ------------------------------------------------------------------ #
    try:
        class ErrorProp:
            @property
            def bad(self):
                raise ValueError("property error")

        def get_bad(obj):
            return obj.bad

        obj = ErrorProp()

        # Warmup with a working object, then switch to error-raising one
        class GoodProp:
            @property
            def bad(self):
                return 42

        good = GoodProp()
        for _ in range(WARMUP):
            get_bad(good)

        check_jit_compiled(get_bad, "get_bad")

        # After JIT compilation, the error-raising property should still raise
        caught = False
        try:
            get_bad(obj)
        except ValueError as ex:
            caught = True
            assert "property error" in str(ex)
        assert caught, "Property should raise ValueError"
        print("  PASS: test_property_raising_exception")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_raising_exception — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Inherited property
    # ------------------------------------------------------------------ #
    try:
        class Base:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

        class Child(Base):
            pass

        def get_inherited(obj):
            return obj.value

        child = Child(33)
        for _ in range(WARMUP):
            get_inherited(child)

        check_jit_compiled(get_inherited, "get_inherited")
        assert get_inherited(child) == 33, "Inherited property should work"
        base = Base(44)
        assert get_inherited(base) == 44, "Base property should also work"
        print("  PASS: test_inherited_property")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inherited_property — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: Overridden property in subclass
    # ------------------------------------------------------------------ #
    try:
        class Parent:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

        class Override(Parent):
            @property
            def value(self):
                return self._x * 10

        def get_override(obj):
            return obj.value

        parent = Parent(5)
        for _ in range(WARMUP):
            get_override(parent)

        check_jit_compiled(get_override, "get_override")
        assert get_override(parent) == 5, "Parent property returns 5"

        over = Override(5)
        assert get_override(over) == 50, "Overridden property returns 50"
        print("  PASS: test_overridden_property_in_subclass")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overridden_property_in_subclass — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Deopt — replace property with plain attribute
    # ------------------------------------------------------------------ #
    try:
        class PropToPlain:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x * 2

        def get_prop_or_plain(obj):
            return obj.value

        obj = PropToPlain(10)
        for _ in range(WARMUP):
            get_prop_or_plain(obj)

        check_jit_compiled(get_prop_or_plain, "get_prop_or_plain")
        assert get_prop_or_plain(obj) == 20, "Property returns 20"

        # Replace property with plain attribute on the instance
        obj.__dict__['value'] = 999
        # Property descriptor on class still takes precedence (data descriptor)
        # So this should still return 20 — property is a data descriptor
        result = get_prop_or_plain(obj)
        assert result == 20, (
            f"Data descriptor (property) takes precedence over instance dict. "
            f"Expected 20, got {result}"
        )

        # Now replace the property on the CLASS with a non-descriptor
        PropToPlain.value = 777
        # Now instance dict entry or class attribute should be used
        # Class attribute is checked — PropToPlain.value is now 777
        # But obj.__dict__['value'] = 999, and 777 is not a descriptor,
        # so instance dict takes precedence
        result = get_prop_or_plain(obj)
        assert result == 999, (
            f"After removing property from class, instance dict should win. "
            f"Expected 999, got {result}"
        )
        print("  PASS: test_deopt_property_to_plain_attribute")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_property_to_plain_attribute — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 10: Deopt — switch to class without property
    # ------------------------------------------------------------------ #
    try:
        class WithProp:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

        class WithoutProp:
            def __init__(self, x):
                self.value = x  # Plain attribute, not property

        def get_value_10(obj):
            return obj.value

        wp = WithProp(42)
        for _ in range(WARMUP):
            get_value_10(wp)

        check_jit_compiled(get_value_10, "get_value_10")
        assert get_value_10(wp) == 42

        # Switch to class without property — should deopt
        wop = WithoutProp(99)
        assert get_value_10(wop) == 99, "Plain attribute should return 99"
        print("  PASS: test_deopt_switch_to_class_without_property")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_switch_to_class_without_property — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: Deopt — switch to class with different property
    # ------------------------------------------------------------------ #
    try:
        class PropA:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x + 1

        class PropB:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x * 3

        def get_value_11(obj):
            return obj.value

        a = PropA(10)
        for _ in range(WARMUP):
            get_value_11(a)

        check_jit_compiled(get_value_11, "get_value_11")
        assert get_value_11(a) == 11, "PropA: 10+1=11"

        b = PropB(10)
        assert get_value_11(b) == 30, "PropB: 10*3=30"
        print("  PASS: test_deopt_switch_to_different_property")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_switch_to_different_property — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: Property in loop
    # ------------------------------------------------------------------ #
    try:
        class Counter:
            def __init__(self, start):
                self._val = start

            @property
            def current(self):
                return self._val

        def sum_property_loop(obj, n):
            total = 0
            for _ in range(n):
                total += obj.current
            return total

        c = Counter(3)
        for _ in range(WARMUP):
            sum_property_loop(c, 1)

        check_jit_compiled(sum_property_loop, "sum_property_loop")
        result = sum_property_loop(c, 100)
        assert result == 300, f"3 * 100 = 300, got {result}"
        print("  PASS: test_property_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: Multiple properties on same class
    # ------------------------------------------------------------------ #
    try:
        class MultiProp:
            def __init__(self, x, y, z):
                self._x = x
                self._y = y
                self._z = z

            @property
            def x(self):
                return self._x

            @property
            def y(self):
                return self._y

            @property
            def z(self):
                return self._z

        def get_all_props(obj):
            return (obj.x, obj.y, obj.z)

        mp = MultiProp(1, 2, 3)
        for _ in range(WARMUP):
            get_all_props(mp)

        check_jit_compiled(get_all_props, "get_all_props")
        result = get_all_props(mp)
        assert result == (1, 2, 3), f"Expected (1, 2, 3), got {result}"

        mp2 = MultiProp(10, 20, 30)
        assert get_all_props(mp2) == (10, 20, 30)
        print("  PASS: test_multiple_properties_on_same_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_properties_on_same_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: Property returning None
    # ------------------------------------------------------------------ #
    try:
        class NullProp:
            @property
            def nothing(self):
                return None

        def get_nothing(obj):
            return obj.nothing

        np = NullProp()
        for _ in range(WARMUP):
            get_nothing(np)

        check_jit_compiled(get_nothing, "get_nothing")
        result = get_nothing(np)
        assert result is None, f"Expected None, got {result}"
        print("  PASS: test_property_returning_none")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_returning_none — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Cached property pattern (manual)
    # ------------------------------------------------------------------ #
    try:
        class CachedProp:
            def __init__(self, x):
                self._x = x
                self._cache = None

            @property
            def expensive(self):
                if self._cache is None:
                    self._cache = self._x ** 2
                return self._cache

        def get_expensive(obj):
            return obj.expensive

        cp = CachedProp(7)
        for _ in range(WARMUP):
            get_expensive(cp)

        check_jit_compiled(get_expensive, "get_expensive")
        assert get_expensive(cp) == 49, f"7**2=49, got {get_expensive(cp)}"

        # Verify caching: _cache should be set
        assert cp._cache == 49

        # New instance — cache should be None until accessed
        cp2 = CachedProp(5)
        assert cp2._cache is None
        assert get_expensive(cp2) == 25
        assert cp2._cache == 25
        print("  PASS: test_cached_property_pattern")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_cached_property_pattern — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: Property with __slots__
    # ------------------------------------------------------------------ #
    try:
        class SlottedProp:
            __slots__ = ('_x',)

            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

        def get_slotted(obj):
            return obj.value

        sp = SlottedProp(88)
        for _ in range(WARMUP):
            get_slotted(sp)

        check_jit_compiled(get_slotted, "get_slotted")
        assert get_slotted(sp) == 88
        sp2 = SlottedProp(77)
        assert get_slotted(sp2) == 77
        print("  PASS: test_property_with_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_with_slots — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Property on class created dynamically (type())
    # ------------------------------------------------------------------ #
    try:
        def make_prop_class(multiplier):
            def getter(self):
                return self._x * multiplier
            return type('DynProp', (), {
                '__init__': lambda self, x: setattr(self, '_x', x),
                'value': property(getter),
            })

        DynA = make_prop_class(2)
        DynB = make_prop_class(5)

        def get_dyn(obj):
            return obj.value

        a = DynA(10)
        for _ in range(WARMUP):
            get_dyn(a)

        check_jit_compiled(get_dyn, "get_dyn")
        assert get_dyn(a) == 20, f"10*2=20, got {get_dyn(a)}"

        b = DynB(10)
        assert get_dyn(b) == 50, f"10*5=50, got {get_dyn(b)}"
        print("  PASS: test_property_on_dynamic_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_on_dynamic_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Rapid type alternation with property
    # ------------------------------------------------------------------ #
    try:
        class TypeX:
            def __init__(self, x):
                self._x = x

            @property
            def val(self):
                return self._x

        class TypeY:
            def __init__(self, x):
                self._x = x

            @property
            def val(self):
                return self._x + 100

        def get_val(obj):
            return obj.val

        tx = TypeX(1)
        for _ in range(WARMUP):
            get_val(tx)

        check_jit_compiled(get_val, "get_val")

        # Rapid alternation — forces repeated deopt
        ty = TypeY(1)
        ok = True
        for i in range(50):
            rx = get_val(tx)
            ry = get_val(ty)
            if rx != 1:
                print(f"  FAIL: TypeX iteration {i}: expected 1, got {rx}")
                ok = False
                break
            if ry != 101:
                print(f"  FAIL: TypeY iteration {i}: expected 101, got {ry}")
                ok = False
                break

        if ok:
            print("  PASS: test_rapid_type_alternation_with_property")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation_with_property — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: Property deleted from class at runtime
    # ------------------------------------------------------------------ #
    try:
        class Deletable:
            def __init__(self, x):
                self._x = x
                self.value = x * 3  # Also set instance attr as fallback

            @property
            def prop(self):
                return self._x * 2

        def get_prop(obj):
            return obj.prop

        d = Deletable(10)
        for _ in range(WARMUP):
            get_prop(d)

        check_jit_compiled(get_prop, "get_prop")
        assert get_prop(d) == 20, "Property returns 10*2=20"

        # Delete the property from the class
        del Deletable.prop

        # Now obj.prop should raise AttributeError (no instance attr 'prop')
        caught = False
        try:
            get_prop(d)
        except AttributeError:
            caught = True
        assert caught, "After deleting property, AttributeError expected"
        print("  PASS: test_property_deleted_from_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_deleted_from_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 20: Property access vs fget equivalence
    # ------------------------------------------------------------------ #
    try:
        class EquivProp:
            def __init__(self, x):
                self._x = x

            @property
            def value(self):
                return self._x

        def via_attr(obj):
            return obj.value

        obj = EquivProp(42)
        for _ in range(WARMUP):
            via_attr(obj)

        check_jit_compiled(via_attr, "via_attr")

        # Compare property access (JIT path) with direct fget call
        fget = EquivProp.value.fget
        for val in [0, 1, -1, 100, 999]:
            o = EquivProp(val)
            jit_result = via_attr(o)
            fget_result = fget(o)
            assert jit_result == fget_result, (
                f"Mismatch for val={val}: attr={jit_result}, fget={fget_result}"
            )

        print("  PASS: test_property_access_vs_fget_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_access_vs_fget_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Summary
    # ------------------------------------------------------------------ #
    print()
    print(f"LOAD_ATTR_PROPERTY: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
