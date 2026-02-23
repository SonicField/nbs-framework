#!/usr/bin/env python3
"""
test_store_attr_instance_value.py — Correctness and deopt tests for
STORE_ATTR_INSTANCE_VALUE specialisation.

Targets: STORE_ATTR_INSTANCE_VALUE, STORE_ATTR_SLOT.

STORE_ATTR_INSTANCE_VALUE specialises attribute store operations
(obj.attr = value) when the receiver has a regular instance dictionary.
Instead of going through the generic STORE_ATTR path (which must resolve
descriptors and check the MRO), the specialisation directly writes to the
instance dict at a cached offset.

STORE_ATTR_SLOT is the companion specialisation for classes using __slots__,
writing directly to the slot memory rather than going through the dict.

Both emit a GuardType on the receiver using the type version from CPython's
inline cache. The GuardType enables type propagation for downstream
operations and fires deopt when the receiver type changes.

Mechanism:
1. Builder reads _PyAttrCache from CPython IC (version[2], index)
2. findTypeByVersionTag(type_version) -> PyTypeObject*
3. GuardType(receiver, exact_type) emitted
4. StoreAttr emitted (runtime IC handles the actual store)

Deopt triggers:
  - Receiver type changes (different class)
  - Class hierarchy modified after JIT compilation
  - Receiver switches from dict-based to __slots__-based class (or vice versa)

Tests cover:
  - Basic attribute store
  - Store overwriting existing attribute
  - Store different value types
  - Store across different instances of same class
  - Store to new attribute (not present at compile time)
  - Subclass deopt (GuardType fires)
  - Polymorphic store (same function, different class types)
  - __slots__-based class (STORE_ATTR_SLOT)
  - Mixed dict and __slots__ store
  - Rapid store/load cycles
  - Store/load roundtrip with type alternation
  - Store stability (many writes, one final read)
  - Identity preservation through store/load
  - Store None and sentinel values
  - Multiple attributes on same instance
  - Deopt: switch to class with __setattr__
  - Store with property descriptor (setter)
  - Deopt: switch to class with different layout
  - Store in loop accumulator pattern
  - Store vs setattr() equivalence

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Store side effects match interpreter behaviour

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_store_attr_instance_value.py
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
    print("=== STORE_ATTR_INSTANCE_VALUE Correctness & Deopt Tests ===")
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
    # Test 1: Basic attribute store
    # ------------------------------------------------------------------ #
    try:
        class Box:
            def __init__(self, value):
                self.value = value

        def set_value(obj, v):
            obj.value = v

        b = Box(0)
        for _ in range(WARMUP):
            set_value(b, 42)

        check_jit_compiled(set_value, "set_value")
        set_value(b, 99)
        assert b.value == 99, f"Expected 99, got {b.value}"
        b2 = Box(0)
        set_value(b2, 77)
        assert b2.value == 77
        print("  PASS: test_basic_attribute_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_attribute_store — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Store overwriting existing attribute
    # ------------------------------------------------------------------ #
    try:
        class Counter:
            def __init__(self, count):
                self.count = count

        def set_count(obj, n):
            obj.count = n

        c = Counter(0)
        for _ in range(WARMUP):
            set_count(c, 1)

        check_jit_compiled(set_count, "set_count")
        set_count(c, 10)
        assert c.count == 10
        set_count(c, 20)
        assert c.count == 20
        set_count(c, 30)
        assert c.count == 30, f"Expected 30, got {c.count}"
        print("  PASS: test_store_overwriting_existing")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_overwriting_existing — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Store different value types
    # ------------------------------------------------------------------ #
    try:
        class TypeHolder:
            def __init__(self, val):
                self.val = val

        def set_val(obj, v):
            obj.val = v

        th = TypeHolder(0)
        for _ in range(WARMUP):
            set_val(th, 42)

        check_jit_compiled(set_val, "set_val")

        set_val(th, "hello")
        assert th.val == "hello"
        set_val(th, [1, 2, 3])
        assert th.val == [1, 2, 3]
        set_val(th, None)
        assert th.val is None
        set_val(th, {"key": "val"})
        assert th.val == {"key": "val"}
        print("  PASS: test_store_different_value_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_different_value_types — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Store across different instances of same class
    # ------------------------------------------------------------------ #
    try:
        class Bucket:
            def __init__(self, data):
                self.data = data

        def set_data(obj, d):
            obj.data = d

        b1 = Bucket("a")
        for _ in range(WARMUP):
            set_data(b1, "warm")

        check_jit_compiled(set_data, "set_data")

        b2 = Bucket("b")
        b3 = Bucket("c")
        set_data(b1, "x")
        set_data(b2, "y")
        set_data(b3, "z")
        assert b1.data == "x" and b2.data == "y" and b3.data == "z", (
            f"b1={b1.data}, b2={b2.data}, b3={b3.data}"
        )
        print("  PASS: test_store_across_instances")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_across_instances — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: Store to new attribute (not present at compile time)
    # ------------------------------------------------------------------ #
    try:
        class Expandable:
            def __init__(self, base):
                self.base = base

        def set_extra(obj, v):
            obj.extra = v

        e = Expandable(10)
        assert not hasattr(e, 'extra')

        # Warm up with an instance that has 'extra'
        temp = Expandable(0)
        temp.extra = 0
        for _ in range(WARMUP):
            set_extra(temp, 0)

        check_jit_compiled(set_extra, "set_extra")

        # Store to original instance which lacks 'extra' — deopt creates it
        set_extra(e, 777)
        assert hasattr(e, 'extra') and e.extra == 777, (
            f"extra={getattr(e, 'extra', 'MISSING')}"
        )
        print("  PASS: test_store_new_attribute")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_new_attribute — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: Subclass deopt (GuardType fires)
    # ------------------------------------------------------------------ #
    try:
        class Animal:
            def __init__(self, name):
                self.name = name

        class Dog(Animal):
            def __init__(self, name, breed):
                super().__init__(name)
                self.breed = breed

        def set_animal_name(obj, n):
            obj.name = n

        a = Animal("generic")
        for _ in range(WARMUP):
            set_animal_name(a, "generic")

        check_jit_compiled(set_animal_name, "set_animal_name")
        set_animal_name(a, "base_val")
        assert a.name == "base_val"

        # Subclass — GuardType should deopt
        dog = Dog("Rex", "Lab")
        set_animal_name(dog, "Fido")
        assert dog.name == "Fido"

        # Base still works after deopt
        set_animal_name(a, "post_deopt")
        assert a.name == "post_deopt"
        print("  PASS: test_subclass_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subclass_deopt — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Polymorphic store (same function, different class types)
    # ------------------------------------------------------------------ #
    try:
        class Red:
            def __init__(self, shade):
                self.shade = shade

        class Blue:
            def __init__(self, shade):
                self.shade = shade

        class Green:
            def __init__(self, shade):
                self.shade = shade

        def set_shade(obj, s):
            obj.shade = s

        r = Red("light")
        for _ in range(WARMUP):
            set_shade(r, "warm")

        check_jit_compiled(set_shade, "set_shade")

        bl = Blue("dark")
        g = Green("forest")
        set_shade(r, "crimson")
        set_shade(bl, "navy")
        set_shade(g, "emerald")
        assert r.shade == "crimson" and bl.shade == "navy" and g.shade == "emerald"
        print("  PASS: test_polymorphic_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_polymorphic_store — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: __slots__-based class (STORE_ATTR_SLOT)
    # ------------------------------------------------------------------ #
    try:
        class SlottedPoint:
            __slots__ = ('x', 'y')
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def set_slot_x(obj, v):
            obj.x = v

        sp = SlottedPoint(0, 0)
        for _ in range(WARMUP):
            set_slot_x(sp, 1)

        check_jit_compiled(set_slot_x, "set_slot_x")
        set_slot_x(sp, 42)
        assert sp.x == 42, f"Expected 42, got {sp.x}"
        set_slot_x(sp, 100)
        assert sp.x == 100
        print("  PASS: test_slots_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slots_store — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Mixed dict and __slots__ store
    # ------------------------------------------------------------------ #
    try:
        class DictStore:
            def __init__(self, tag):
                self.tag = tag

        class SlotStore:
            __slots__ = ('tag',)
            def __init__(self, tag):
                self.tag = tag

        def set_tag(obj, t):
            obj.tag = t

        ds = DictStore("d")
        for _ in range(WARMUP):
            set_tag(ds, "warm")

        check_jit_compiled(set_tag, "set_tag")

        ss = SlotStore("s")
        set_tag(ds, "dict_val")
        set_tag(ss, "slot_val")
        assert ds.tag == "dict_val" and ss.tag == "slot_val"
        print("  PASS: test_mixed_dict_and_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_dict_and_slots — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 10: Rapid store/load cycles (1000 iterations)
    # ------------------------------------------------------------------ #
    try:
        class Rapid:
            def __init__(self, n):
                self.n = n

        def set_rapid_n(obj, v):
            obj.n = v

        rp = Rapid(0)
        for _ in range(WARMUP):
            set_rapid_n(rp, 0)

        check_jit_compiled(set_rapid_n, "set_rapid_n")

        ok = True
        for i in range(1000):
            set_rapid_n(rp, i)
            if rp.n != i:
                print(f"  FAIL: cycle {i}: stored {i}, got {rp.n}")
                ok = False
                break

        if ok:
            print("  PASS: test_rapid_store_load_cycles")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_store_load_cycles — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: Store/load roundtrip with type alternation
    # ------------------------------------------------------------------ #
    try:
        class Alpha:
            def __init__(self, data):
                self.data = data

        class Beta:
            def __init__(self, data):
                self.data = data

        def set_data_poly(obj, d):
            obj.data = d

        aa = Alpha(0)
        for _ in range(WARMUP):
            set_data_poly(aa, 0)

        check_jit_compiled(set_data_poly, "set_data_poly")

        bb = Beta(0)
        ok = True
        for i in range(50):
            set_data_poly(aa, i * 2)
            set_data_poly(bb, i * 2 + 1)
            if aa.data != i * 2 or bb.data != i * 2 + 1:
                print(f"  FAIL: cycle {i}: alpha={aa.data}, beta={bb.data}")
                ok = False
                break

        if ok:
            print("  PASS: test_store_load_type_alternation")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_store_load_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: Store stability (many writes, one final read)
    # ------------------------------------------------------------------ #
    try:
        class Sink:
            def __init__(self):
                self.value = 0

        def store_sink(obj, v):
            obj.value = v

        sk = Sink()
        for _ in range(WARMUP):
            store_sink(sk, 0)

        check_jit_compiled(store_sink, "store_sink")

        for i in range(10000):
            store_sink(sk, i)

        assert sk.value == 9999, f"Expected 9999, got {sk.value}"
        print("  PASS: test_store_stability")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_stability — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: Identity preservation through store/load
    # ------------------------------------------------------------------ #
    try:
        class Holder:
            def __init__(self):
                self.ref = None

        def set_ref(obj, r):
            obj.ref = r

        h = Holder()
        for _ in range(WARMUP):
            set_ref(h, None)

        check_jit_compiled(set_ref, "set_ref")

        sentinel = object()
        set_ref(h, sentinel)
        assert h.ref is sentinel, "Identity must be preserved through store/load"

        lst = [1, 2, 3]
        set_ref(h, lst)
        assert h.ref is lst, "List identity must be preserved"
        lst.append(4)
        assert h.ref == [1, 2, 3, 4], "Mutation through original ref must be visible"
        print("  PASS: test_identity_preservation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_identity_preservation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: Store None and sentinel values
    # ------------------------------------------------------------------ #
    try:
        class NoneHolder:
            def __init__(self):
                self.val = "not_none"

        def set_nh(obj, v):
            obj.val = v

        nh = NoneHolder()
        for _ in range(WARMUP):
            set_nh(nh, "warm")

        check_jit_compiled(set_nh, "set_nh")

        set_nh(nh, None)
        assert nh.val is None, f"Expected None, got {nh.val}"
        set_nh(nh, True)
        assert nh.val is True
        set_nh(nh, False)
        assert nh.val is False
        set_nh(nh, 0)
        assert nh.val == 0 and nh.val is not False
        set_nh(nh, "")
        assert nh.val == ""
        set_nh(nh, ())
        assert nh.val == ()
        print("  PASS: test_store_none_and_sentinels")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_none_and_sentinels — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Multiple attributes on same instance
    # ------------------------------------------------------------------ #
    try:
        class Multi:
            def __init__(self, a, b, c):
                self.a = a
                self.b = b
                self.c = c

        def set_all(obj, a, b, c):
            obj.a = a
            obj.b = b
            obj.c = c

        m = Multi(0, 0, 0)
        for _ in range(WARMUP):
            set_all(m, 1, 2, 3)

        check_jit_compiled(set_all, "set_all")

        set_all(m, 10, 20, 30)
        assert m.a == 10 and m.b == 20 and m.c == 30, (
            f"a={m.a}, b={m.b}, c={m.c}"
        )

        # Verify independent — changing one doesn't affect others
        set_all(m, 99, 20, 30)
        assert m.a == 99 and m.b == 20 and m.c == 30
        print("  PASS: test_multiple_attributes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_attributes — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: Deopt — switch to class with __setattr__
    # ------------------------------------------------------------------ #
    try:
        class Plain:
            def __init__(self, x):
                self.x = x

        class WithSetattr:
            def __init__(self, x):
                object.__setattr__(self, 'x', x)
                object.__setattr__(self, '_log', [])

            def __setattr__(self, name, value):
                object.__setattr__(self, name, value * 2)

        def set_x(obj, v):
            obj.x = v

        p = Plain(0)
        for _ in range(WARMUP):
            set_x(p, 1)

        check_jit_compiled(set_x, "set_x")
        set_x(p, 42)
        assert p.x == 42

        # Switch to class with __setattr__ — should deopt
        ws = WithSetattr(5)
        set_x(ws, 10)
        assert ws.x == 20, f"__setattr__ doubles: expected 20, got {ws.x}"
        print("  PASS: test_deopt_to_setattr_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_setattr_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Store with property descriptor (setter)
    # ------------------------------------------------------------------ #
    try:
        class PropStore:
            def __init__(self, x):
                self._x = x

            @property
            def x(self):
                return self._x

            @x.setter
            def x(self, v):
                self._x = v * 3  # Triples on set

        def set_prop_x(obj, v):
            obj.x = v

        ps = PropStore(0)
        for _ in range(WARMUP):
            set_prop_x(ps, 1)

        check_jit_compiled(set_prop_x, "set_prop_x")
        set_prop_x(ps, 10)
        assert ps.x == 30, f"Property setter triples: expected 30, got {ps.x}"
        assert ps._x == 30
        print("  PASS: test_store_with_property_setter")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_with_property_setter — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Deopt — switch to class with different layout
    # ------------------------------------------------------------------ #
    try:
        class LayoutA:
            def __init__(self):
                self.x = 0
                self.y = 0

        class LayoutB:
            def __init__(self):
                self.z = 0
                self.w = 0
                self.x = 0  # x is at different dict offset

        def set_layout_x(obj, v):
            obj.x = v

        la = LayoutA()
        for _ in range(WARMUP):
            set_layout_x(la, 1)

        check_jit_compiled(set_layout_x, "set_layout_x")
        set_layout_x(la, 42)
        assert la.x == 42

        # Switch to different layout — x at different offset
        lb = LayoutB()
        set_layout_x(lb, 99)
        assert lb.x == 99, f"Expected 99, got {lb.x}"
        assert lb.z == 0 and lb.w == 0, "Other attributes must not be affected"
        print("  PASS: test_deopt_different_layout")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_layout — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: Store in loop accumulator pattern
    # ------------------------------------------------------------------ #
    try:
        class Accum:
            def __init__(self):
                self.total = 0

        def add_to_accum(obj, v):
            obj.total = obj.total + v

        acc = Accum()
        for _ in range(WARMUP):
            acc.total = 0
            add_to_accum(acc, 1)

        check_jit_compiled(add_to_accum, "add_to_accum")

        acc.total = 0
        for i in range(100):
            add_to_accum(acc, i)

        # sum(0..99) = 4950
        assert acc.total == 4950, f"Expected 4950, got {acc.total}"
        print("  PASS: test_store_loop_accumulator")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_loop_accumulator — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 20: Store vs setattr() equivalence
    # ------------------------------------------------------------------ #
    try:
        class Equiv:
            def __init__(self, x):
                self.x = x

        def via_store(obj, v):
            obj.x = v

        obj = Equiv(0)
        for _ in range(WARMUP):
            via_store(obj, 1)

        check_jit_compiled(via_store, "via_store")

        for val in [0, 1, -1, 100, None, "hello", [1, 2]]:
            o1 = Equiv(0)
            o2 = Equiv(0)
            via_store(o1, val)
            setattr(o2, 'x', val)
            if val is None:
                assert o1.x is None and o2.x is None
            elif isinstance(val, list):
                assert o1.x == o2.x
            else:
                assert o1.x == o2.x, (
                    f"Mismatch for val={val}: store={o1.x}, setattr={o2.x}"
                )

        print("  PASS: test_store_vs_setattr_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_vs_setattr_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Summary
    # ------------------------------------------------------------------ #
    print()
    print(f"STORE_ATTR_INSTANCE_VALUE: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
