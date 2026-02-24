#!/usr/bin/env python3
"""
test_call_alloc_and_enter_init.py — Correctness and deopt tests for
CALL_ALLOC_AND_ENTER_INIT specialisation.

Targets: CALL_ALLOC_AND_ENTER_INIT.

CALL_ALLOC_AND_ENTER_INIT specialises calls to user-defined classes
(type instantiation) where the adaptive interpreter detects that:
1. The callable is a type (class)
2. The type uses the default type.__call__ (no metaclass override)
3. The type's __new__ is object.__new__ (standard allocation)

When these conditions hold, CPython replaces the generic CALL with
CALL_ALLOC_AND_ENTER_INIT, which allocates the instance directly and
enters __init__ without dispatching through __new__ or type.__call__.

Deopt triggers:
  - Type has custom __new__ (non-standard allocation)
  - Metaclass overrides __call__ (custom instantiation protocol)
  - __init__ is not found or is a descriptor with unexpected type
  - Different class passed to same call site

Tests cover:
  - Simple class with __init__ (no args)
  - Class with positional args
  - Class with keyword args
  - Class with *args and **kwargs
  - Class with default argument values
  - Inheritance chain — subclass __init__
  - Multiple inheritance with super().__init__
  - Class with __slots__
  - Dataclass-like pattern (manual)
  - Instance attribute mutation after construction
  - Deopt: class with custom __new__
  - Deopt: different class at same call site
  - Deopt: metaclass with __call__ override
  - Rapid instantiation (1000 objects)
  - Stability — 10000 instantiations
  - __init__ raising exception
  - __init__ with side effects (global counter)
  - Nested instantiation (class creates another class in __init__)
  - Property access immediately after construction
  - Equivalence: MyClass(args) vs type.__call__(MyClass, args)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after class change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_alloc_and_enter_init.py
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
    # Test 1: Simple class with __init__ (no args)
    # ------------------------------------------------------------------
    try:
        class Simple:
            def __init__(self):
                self.value = 42

        def make_simple():
            return Simple()

        for _ in range(WARMUP):
            make_simple()
        check_jit_compiled(make_simple, "make_simple")

        obj = make_simple()
        assert isinstance(obj, Simple)
        assert obj.value == 42
        assert type(obj) is Simple
        print("  PASS: test_simple_no_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_simple_no_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Class with positional args
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

        p = make_point(10, 20)
        assert p.x == 10
        assert p.y == 20

        p2 = make_point(-5, 0)
        assert p2.x == -5
        assert p2.y == 0

        # Different types for args
        p3 = make_point("hello", [1, 2])
        assert p3.x == "hello"
        assert p3.y == [1, 2]
        print("  PASS: test_positional_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_positional_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Class with keyword args
    # ------------------------------------------------------------------
    try:
        class Config:
            def __init__(self, name, debug=False, level=0):
                self.name = name
                self.debug = debug
                self.level = level

        def make_config(name, **kwargs):
            return Config(name, **kwargs)

        for _ in range(WARMUP):
            make_config("test")
        check_jit_compiled(make_config, "make_config")

        c1 = make_config("prod")
        assert c1.name == "prod"
        assert c1.debug is False
        assert c1.level == 0

        c2 = make_config("dev", debug=True, level=3)
        assert c2.name == "dev"
        assert c2.debug is True
        assert c2.level == 3
        print("  PASS: test_keyword_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_keyword_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Class with *args and **kwargs
    # ------------------------------------------------------------------
    try:
        class Flexible:
            def __init__(self, *args, **kwargs):
                self.args = args
                self.kwargs = kwargs

        def make_flexible(*args, **kwargs):
            return Flexible(*args, **kwargs)

        for _ in range(WARMUP):
            make_flexible(1, 2, 3)
        check_jit_compiled(make_flexible, "make_flexible")

        f1 = make_flexible(1, 2, 3)
        assert f1.args == (1, 2, 3)
        assert f1.kwargs == {}

        f2 = make_flexible(a=1, b=2)
        assert f2.args == ()
        assert f2.kwargs == {"a": 1, "b": 2}

        f3 = make_flexible(10, x="y")
        assert f3.args == (10,)
        assert f3.kwargs == {"x": "y"}
        print("  PASS: test_args_kwargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_args_kwargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Class with default argument values
    # ------------------------------------------------------------------
    try:
        class WithDefaults:
            def __init__(self, a, b=10, c=20):
                self.total = a + b + c

        def make_defaults(a, b=10, c=20):
            return WithDefaults(a, b, c)

        for _ in range(WARMUP):
            make_defaults(1)
        check_jit_compiled(make_defaults, "make_defaults")

        d1 = make_defaults(1)
        assert d1.total == 31  # 1 + 10 + 20

        d2 = make_defaults(1, 2)
        assert d2.total == 23  # 1 + 2 + 20

        d3 = make_defaults(1, 2, 3)
        assert d3.total == 6  # 1 + 2 + 3
        print("  PASS: test_default_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_default_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Inheritance chain — subclass __init__
    # ------------------------------------------------------------------
    try:
        class Animal:
            def __init__(self, name):
                self.name = name
                self.kind = "animal"

        class Dog(Animal):
            def __init__(self, name, breed):
                super().__init__(name)
                self.breed = breed
                self.kind = "dog"

        def make_dog(name, breed):
            return Dog(name, breed)

        for _ in range(WARMUP):
            make_dog("Rex", "Labrador")
        check_jit_compiled(make_dog, "make_dog")

        d = make_dog("Buddy", "Golden")
        assert isinstance(d, Dog)
        assert isinstance(d, Animal)
        assert d.name == "Buddy"
        assert d.breed == "Golden"
        assert d.kind == "dog"
        print("  PASS: test_inheritance_subclass_init")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inheritance_subclass_init — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Multiple inheritance with super().__init__
    # ------------------------------------------------------------------
    try:
        class Mixin1:
            def __init__(self, **kwargs):
                self.mixin1_val = kwargs.pop("m1", 0)
                super().__init__(**kwargs)

        class Mixin2:
            def __init__(self, **kwargs):
                self.mixin2_val = kwargs.pop("m2", 0)
                super().__init__(**kwargs)

        class Combined(Mixin1, Mixin2):
            def __init__(self, **kwargs):
                self.own_val = kwargs.pop("own", 0)
                super().__init__(**kwargs)

        def make_combined(**kwargs):
            return Combined(**kwargs)

        for _ in range(WARMUP):
            make_combined(own=1, m1=2, m2=3)
        check_jit_compiled(make_combined, "make_combined")

        c = make_combined(own=10, m1=20, m2=30)
        assert c.own_val == 10
        assert c.mixin1_val == 20
        assert c.mixin2_val == 30
        assert isinstance(c, Combined)
        assert isinstance(c, Mixin1)
        assert isinstance(c, Mixin2)
        print("  PASS: test_multiple_inheritance_super")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_inheritance_super — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Class with __slots__
    # ------------------------------------------------------------------
    try:
        class Slotted:
            __slots__ = ("x", "y")
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def make_slotted(x, y):
            return Slotted(x, y)

        for _ in range(WARMUP):
            make_slotted(1, 2)
        check_jit_compiled(make_slotted, "make_slotted")

        s = make_slotted(100, 200)
        assert s.x == 100
        assert s.y == 200

        # __slots__ classes should not have __dict__
        assert not hasattr(s, "__dict__")

        # Cannot set arbitrary attributes
        got_error = False
        try:
            s.z = 300
        except AttributeError:
            got_error = True
        assert got_error, "Expected AttributeError for slot-only class"
        print("  PASS: test_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slots — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Dataclass-like pattern (manual fields)
    # ------------------------------------------------------------------
    try:
        class Record:
            def __init__(self, id, name, active=True):
                self.id = id
                self.name = name
                self.active = active

            def __eq__(self, other):
                return (isinstance(other, Record) and
                        self.id == other.id and
                        self.name == other.name and
                        self.active == other.active)

        def make_record(id, name, active=True):
            return Record(id, name, active)

        for _ in range(WARMUP):
            make_record(1, "test")
        check_jit_compiled(make_record, "make_record")

        r1 = make_record(1, "Alice")
        r2 = make_record(1, "Alice")
        r3 = make_record(2, "Bob", False)

        assert r1 == r2
        assert r1 != r3
        assert r3.active is False
        assert r1.active is True
        print("  PASS: test_dataclass_pattern")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dataclass_pattern — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Instance attribute mutation after construction
    # ------------------------------------------------------------------
    try:
        class Mutable:
            def __init__(self, val):
                self.val = val
                self.history = [val]

        def make_mutable(val):
            return Mutable(val)

        for _ in range(WARMUP):
            make_mutable(0)
        check_jit_compiled(make_mutable, "make_mutable")

        m = make_mutable(10)
        assert m.val == 10
        assert m.history == [10]

        # Mutate after construction
        m.val = 20
        m.history.append(20)
        assert m.val == 20
        assert m.history == [10, 20]

        # New instance is independent
        m2 = make_mutable(99)
        assert m2.val == 99
        assert m2.history == [99]
        assert m.history == [10, 20]  # unchanged
        print("  PASS: test_mutation_after_construction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mutation_after_construction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — class with custom __new__
    # ------------------------------------------------------------------
    try:
        class Normal:
            def __init__(self, val):
                self.val = val

        class CustomNew:
            _instance = None
            def __new__(cls, val):
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                return cls._instance
            def __init__(self, val):
                self.val = val

        def make_obj(cls, val):
            return cls(val)

        # Warm up on Normal (standard __new__)
        for _ in range(WARMUP):
            make_obj(Normal, 0)
        check_jit_compiled(make_obj, "make_obj")

        n = make_obj(Normal, 42)
        assert n.val == 42

        # Deopt: switch to class with custom __new__ (singleton)
        CustomNew._instance = None
        s1 = make_obj(CustomNew, 1)
        s2 = make_obj(CustomNew, 2)
        assert s1 is s2  # same instance (singleton)
        assert s1.val == 2  # __init__ called again, overwrites

        # Back to Normal
        n2 = make_obj(Normal, 99)
        assert n2.val == 99
        assert n2 is not n  # different instance
        print("  PASS: test_deopt_custom_new")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_custom_new — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt — different class at same call site
    # ------------------------------------------------------------------
    try:
        class ClassA:
            def __init__(self, x):
                self.x = x
                self.tag = "A"

        class ClassB:
            def __init__(self, x):
                self.x = x
                self.tag = "B"

        class ClassC:
            def __init__(self, x):
                self.x = x * 2
                self.tag = "C"

        def make_tagged(cls, x):
            return cls(x)

        # Warm up on ClassA
        for _ in range(WARMUP):
            make_tagged(ClassA, 0)
        check_jit_compiled(make_tagged, "make_tagged")

        a = make_tagged(ClassA, 10)
        assert a.x == 10 and a.tag == "A"

        # Deopt: ClassB
        b = make_tagged(ClassB, 20)
        assert b.x == 20 and b.tag == "B"

        # Deopt: ClassC
        c = make_tagged(ClassC, 5)
        assert c.x == 10 and c.tag == "C"  # 5 * 2

        # Back to ClassA
        a2 = make_tagged(ClassA, 99)
        assert a2.x == 99 and a2.tag == "A"
        print("  PASS: test_deopt_different_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_class — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Deopt — metaclass with __call__ override
    # ------------------------------------------------------------------
    try:
        class TrackingMeta(type):
            call_count = 0
            def __call__(cls, *args, **kwargs):
                TrackingMeta.call_count += 1
                return super().__call__(*args, **kwargs)

        class Tracked(metaclass=TrackingMeta):
            def __init__(self, val):
                self.val = val

        class PlainClass:
            def __init__(self, val):
                self.val = val

        def make_instance(cls, val):
            return cls(val)

        # Warm up on PlainClass
        for _ in range(WARMUP):
            make_instance(PlainClass, 0)
        check_jit_compiled(make_instance, "make_instance")

        p = make_instance(PlainClass, 42)
        assert p.val == 42

        # Deopt: metaclass with __call__ override
        TrackingMeta.call_count = 0
        t = make_instance(Tracked, 99)
        assert t.val == 99
        assert TrackingMeta.call_count == 1

        t2 = make_instance(Tracked, 100)
        assert t2.val == 100
        assert TrackingMeta.call_count == 2

        # Back to PlainClass
        p2 = make_instance(PlainClass, 77)
        assert p2.val == 77
        print("  PASS: test_deopt_metaclass_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_metaclass_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid instantiation (1000 objects)
    # ------------------------------------------------------------------
    try:
        class Counter:
            def __init__(self, n):
                self.n = n

        def make_counter(n):
            return Counter(n)

        for _ in range(WARMUP):
            make_counter(0)
        check_jit_compiled(make_counter, "make_counter")

        objects = [make_counter(i) for i in range(1000)]
        assert len(objects) == 1000
        assert objects[0].n == 0
        assert objects[999].n == 999

        # All distinct instances
        assert objects[0] is not objects[1]
        assert len(set(id(o) for o in objects)) == 1000
        print("  PASS: test_rapid_instantiation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_instantiation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Stability — 10000 instantiations
    # ------------------------------------------------------------------
    try:
        class Stable:
            def __init__(self, v):
                self.v = v

        def make_stable(v):
            return Stable(v)

        for _ in range(WARMUP):
            make_stable(0)
        check_jit_compiled(make_stable, "make_stable")

        last = None
        for i in range(10000):
            obj = make_stable(i)
            assert obj.v == i
            last = obj
        assert last.v == 9999
        print("  PASS: test_stability_10000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: __init__ raising exception
    # ------------------------------------------------------------------
    try:
        class Strict:
            def __init__(self, val):
                if val < 0:
                    raise ValueError(f"val must be non-negative, got {val}")
                self.val = val

        def make_strict(val):
            return Strict(val)

        for _ in range(WARMUP):
            make_strict(0)
        check_jit_compiled(make_strict, "make_strict")

        s = make_strict(10)
        assert s.val == 10

        # __init__ raises ValueError
        got_error = False
        try:
            make_strict(-1)
        except ValueError as e:
            got_error = True
            assert "-1" in str(e)
        assert got_error, "Expected ValueError for negative val"

        # After exception, normal construction still works
        s2 = make_strict(5)
        assert s2.val == 5
        print("  PASS: test_init_raises_exception")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_init_raises_exception — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: __init__ with side effects (global counter)
    # ------------------------------------------------------------------
    try:
        init_counter = [0]

        class Counted:
            def __init__(self):
                init_counter[0] += 1
                self.my_id = init_counter[0]

        def make_counted():
            return Counted()

        init_counter[0] = 0
        for _ in range(WARMUP):
            make_counted()
        check_jit_compiled(make_counted, "make_counted")

        init_counter[0] = 0
        c1 = make_counted()
        c2 = make_counted()
        c3 = make_counted()

        assert c1.my_id == 1
        assert c2.my_id == 2
        assert c3.my_id == 3
        assert init_counter[0] == 3
        print("  PASS: test_init_side_effects")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_init_side_effects — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Nested instantiation (class creates another in __init__)
    # ------------------------------------------------------------------
    try:
        class Inner:
            def __init__(self, val):
                self.val = val

        class Outer:
            def __init__(self, val):
                self.inner = Inner(val * 2)
                self.val = val

        def make_outer(val):
            return Outer(val)

        for _ in range(WARMUP):
            make_outer(1)
        check_jit_compiled(make_outer, "make_outer")

        o = make_outer(5)
        assert o.val == 5
        assert isinstance(o.inner, Inner)
        assert o.inner.val == 10

        o2 = make_outer(0)
        assert o2.val == 0
        assert o2.inner.val == 0

        # Inner instances are independent
        assert o.inner is not o2.inner
        print("  PASS: test_nested_instantiation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_instantiation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Property access immediately after construction
    # ------------------------------------------------------------------
    try:
        class WithProperty:
            def __init__(self, width, height):
                self._width = width
                self._height = height

            @property
            def area(self):
                return self._width * self._height

            @property
            def perimeter(self):
                return 2 * (self._width + self._height)

        def make_rect(w, h):
            return WithProperty(w, h)

        for _ in range(WARMUP):
            make_rect(3, 4)
        check_jit_compiled(make_rect, "make_rect")

        r = make_rect(5, 10)
        assert r.area == 50
        assert r.perimeter == 30

        # Inline property access after construction
        assert make_rect(3, 4).area == 12
        assert make_rect(1, 1).perimeter == 4
        assert make_rect(0, 100).area == 0
        print("  PASS: test_property_after_construction")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_after_construction — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — MyClass(args) vs type.__call__(MyClass, args)
    # ------------------------------------------------------------------
    try:
        class EquivTest:
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def via_direct(x, y):
            return EquivTest(x, y)

        def via_type_call(x, y):
            return type.__call__(EquivTest, x, y)

        for _ in range(WARMUP):
            via_direct(1, 2)
        for _ in range(WARMUP):
            via_type_call(1, 2)
        check_jit_compiled(via_direct, "via_direct")
        check_jit_compiled(via_type_call, "via_type_call")

        test_cases = [
            (1, 2),
            (0, 0),
            (-1, 100),
            ("a", "b"),
            (None, [1, 2]),
        ]
        for x, y in test_cases:
            d = via_direct(x, y)
            t = via_type_call(x, y)
            assert d.x == t.x, f"x mismatch for ({x}, {y}): {d.x} vs {t.x}"
            assert d.y == t.y, f"y mismatch for ({x}, {y}): {d.y} vs {t.y}"
            assert type(d) is type(t) is EquivTest
        print("  PASS: test_equivalence_direct_vs_type_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_direct_vs_type_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_ALLOC_AND_ENTER_INIT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
