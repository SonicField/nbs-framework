#!/usr/bin/env python3
"""
test_store_attr_slot.py — Correctness and deopt tests for STORE_ATTR_SLOT
specialisation.

Targets: STORE_ATTR_SLOT.

STORE_ATTR_SLOT specialises attribute store (obj.x = val) on classes that
use __slots__. Instead of dictionary-based attribute store
(STORE_ATTR_INSTANCE_VALUE), it writes directly to the fixed-offset slot
via the type's member descriptor.

The adaptive specialiser emits STORE_ATTR_SLOT after observing repeated
attribute store on a __slots__-based class instance. The CinderX JIT then
emits a GuardType on the receiver and uses the slot offset for direct write.

Mechanism:
1. Builder reads _PyAttrCache from CPython IC (type_version, slot_index)
2. findTypeByVersionTag(type_version) -> PyTypeObject*
3. GuardType(receiver, exact_type) emitted
4. Direct member-descriptor offset write

Deopt triggers:
  - Receiver type changes (different class with same attr name)
  - Receiver is subclass (GuardType fires)
  - Receiver uses __dict__ instead of __slots__
  - Storing to a read-only slot (unlikely with __slots__ but possible
    with custom descriptors)

Tests cover:
  - Basic slot store (single slot)
  - Multiple slots (different offsets)
  - Overwrite existing slot value
  - Store after deletion (re-initialise)
  - Different instances of the same slotted class
  - Slotted subclass inheriting parent slots
  - Slotted subclass with extra slots
  - Deopt: dict-based class with same attr name
  - Deopt: different slotted class with same attr name
  - Store None to slot
  - Rapid slot stores (1000 cycles)
  - Value type changes on store (int -> str -> list -> None)
  - Stability (10000 store-read cycles)
  - Polymorphic store across slotted types
  - Rapid type alternation on store (50 cycles)
  - Empty __slots__ parent with child slots
  - Mixed __slots__ and __dict__ store
  - Chained slot store (obj.inner.x = val)
  - Store callable to slot
  - Equivalence: obj.x = val vs descriptor __set__

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct store when JIT-compiled (warmup -> JIT -> store -> read back)
  2. Correct store after type change (deopt fires)
  3. Slot semantics preserved (AttributeError on deleted/uninitialised)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_store_attr_slot.py
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
    # Test 1: Basic single-slot store
    # ------------------------------------------------------------------
    try:
        class SingleSlot:
            __slots__ = ('value',)
            def __init__(self):
                pass

        def set_single_value(obj, val):
            obj.value = val

        s = SingleSlot()
        s.value = 0  # initialise so reads don't fail during warmup

        def warmup_store(obj):
            obj.value = 42

        for _ in range(WARMUP):
            warmup_store(s)
        check_jit_compiled(warmup_store, "warmup_store")

        # Now test the actual store function
        for _ in range(WARMUP):
            set_single_value(s, 99)
        check_jit_compiled(set_single_value, "set_single_value")

        set_single_value(s, 42)
        assert s.value == 42
        set_single_value(s, 0)
        assert s.value == 0
        set_single_value(s, -1)
        assert s.value == -1
        set_single_value(s, "hello")
        assert s.value == "hello"
        print("  PASS: test_basic_single_slot_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_single_slot_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Multiple slots (different offsets)
    # ------------------------------------------------------------------
    try:
        class MultiSlot:
            __slots__ = ('x', 'y', 'z')
            def __init__(self):
                self.x = 0
                self.y = 0
                self.z = 0

        def set_ms_x(obj, val):
            obj.x = val

        def set_ms_y(obj, val):
            obj.y = val

        def set_ms_z(obj, val):
            obj.z = val

        m = MultiSlot()
        for _ in range(WARMUP):
            set_ms_x(m, 1)
            set_ms_y(m, 2)
            set_ms_z(m, 3)
        check_jit_compiled(set_ms_x, "set_ms_x")
        check_jit_compiled(set_ms_y, "set_ms_y")
        check_jit_compiled(set_ms_z, "set_ms_z")

        set_ms_x(m, 10)
        set_ms_y(m, 20)
        set_ms_z(m, 30)
        assert m.x == 10
        assert m.y == 20
        assert m.z == 30
        print("  PASS: test_multiple_slots_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_slots_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Overwrite existing slot value
    # ------------------------------------------------------------------
    try:
        class Overwrite:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        def set_ow_val(obj, val):
            obj.val = val

        ow = Overwrite(1)
        for _ in range(WARMUP):
            set_ow_val(ow, 1)
        check_jit_compiled(set_ow_val, "set_ow_val")

        assert ow.val == 1
        set_ow_val(ow, 999)
        assert ow.val == 999
        set_ow_val(ow, -42)
        assert ow.val == -42
        set_ow_val(ow, 0)
        assert ow.val == 0
        print("  PASS: test_overwrite_slot")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overwrite_slot — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Store after deletion (re-initialise)
    # ------------------------------------------------------------------
    try:
        class Reinit:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        def set_reinit_val(obj, val):
            obj.val = val

        ri = Reinit(100)
        for _ in range(WARMUP):
            set_reinit_val(ri, 100)
        check_jit_compiled(set_reinit_val, "set_reinit_val")

        del ri.val
        # Verify deleted
        got_error = False
        try:
            _ = ri.val
        except AttributeError:
            got_error = True
        assert got_error, "Slot should be uninitialised after deletion"

        # Re-store via JIT
        set_reinit_val(ri, 777)
        assert ri.val == 777
        print("  PASS: test_store_after_deletion")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_after_deletion — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Different instances of the same slotted class
    # ------------------------------------------------------------------
    try:
        class Pair:
            __slots__ = ('val',)
            def __init__(self):
                self.val = 0

        def set_pair_val(obj, val):
            obj.val = val

        p1 = Pair()
        for _ in range(WARMUP):
            set_pair_val(p1, 1)
        check_jit_compiled(set_pair_val, "set_pair_val")

        p2 = Pair()
        p3 = Pair()
        set_pair_val(p1, 100)
        set_pair_val(p2, 200)
        set_pair_val(p3, 300)
        assert p1.val == 100
        assert p2.val == 200
        assert p3.val == 300
        print("  PASS: test_different_instances_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_instances_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Slotted subclass inheriting parent slots
    # ------------------------------------------------------------------
    try:
        class Base:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 0

        class Child(Base):
            __slots__ = ('y',)
            def __init__(self):
                super().__init__()
                self.y = 0

        def set_base_x(obj, val):
            obj.x = val

        b = Base()
        for _ in range(WARMUP):
            set_base_x(b, 1)
        check_jit_compiled(set_base_x, "set_base_x")

        set_base_x(b, 10)
        assert b.x == 10

        # Child inherits x — deopt (different type)
        c = Child()
        set_base_x(c, 20)
        assert c.x == 20

        # Base still works after deopt
        set_base_x(Base(), 50)
        print("  PASS: test_subclass_inheriting_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subclass_inheriting_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Slotted subclass with extra slots
    # ------------------------------------------------------------------
    try:
        class Vec2:
            __slots__ = ('x', 'y')
            def __init__(self):
                self.x = 0
                self.y = 0

        class Vec3(Vec2):
            __slots__ = ('z',)
            def __init__(self):
                super().__init__()
                self.z = 0

        def set_vec_z(obj, val):
            obj.z = val

        v3 = Vec3()
        for _ in range(WARMUP):
            set_vec_z(v3, 1)
        check_jit_compiled(set_vec_z, "set_vec_z")

        set_vec_z(v3, 99)
        assert v3.z == 99

        # Vec2 does not have z — should raise AttributeError
        v2 = Vec2()
        got_error = False
        try:
            set_vec_z(v2, 5)
        except AttributeError:
            got_error = True
        assert got_error, "Vec2 should not have z slot"
        print("  PASS: test_subclass_extra_slots_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subclass_extra_slots_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt — dict-based class with same attr name
    # ------------------------------------------------------------------
    try:
        class SlottedName:
            __slots__ = ('name',)
            def __init__(self):
                self.name = ""

        class DictName:
            def __init__(self):
                self.name = ""

        def set_name(obj, val):
            obj.name = val

        sn = SlottedName()
        for _ in range(WARMUP):
            set_name(sn, "test")
        check_jit_compiled(set_name, "set_name")

        set_name(sn, "slotted")
        assert sn.name == "slotted"

        # Deopt: dict-based class
        dn = DictName()
        set_name(dn, "dict-based")
        assert dn.name == "dict-based"

        # Back to slotted
        sn2 = SlottedName()
        set_name(sn2, "again")
        assert sn2.name == "again"
        print("  PASS: test_deopt_dict_class_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_class_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt — different slotted class with same attr name
    # ------------------------------------------------------------------
    try:
        class SlotA:
            __slots__ = ('data',)
            def __init__(self):
                self.data = ""

        class SlotB:
            __slots__ = ('data', 'extra')
            def __init__(self):
                self.data = ""
                self.extra = ""

        def set_data(obj, val):
            obj.data = val

        sa = SlotA()
        for _ in range(WARMUP):
            set_data(sa, "a")
        check_jit_compiled(set_data, "set_data")

        set_data(sa, "from_a")
        assert sa.data == "from_a"

        # Deopt: different slotted class
        sb = SlotB()
        set_data(sb, "from_b")
        assert sb.data == "from_b"

        # Back to SlotA
        set_data(SlotA(), "a_again")
        print("  PASS: test_deopt_different_slotted_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_slotted_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Store None to slot
    # ------------------------------------------------------------------
    try:
        class NoneSlot:
            __slots__ = ('val',)
            def __init__(self):
                self.val = 0

        def set_none_val(obj, val):
            obj.val = val

        ns = NoneSlot()
        for _ in range(WARMUP):
            set_none_val(ns, 0)
        check_jit_compiled(set_none_val, "set_none_val")

        set_none_val(ns, None)
        assert ns.val is None

        # Store non-None back
        set_none_val(ns, 42)
        assert ns.val == 42

        # Store None again
        set_none_val(ns, None)
        assert ns.val is None
        print("  PASS: test_store_none")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_none — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Rapid slot stores (1000 cycles)
    # ------------------------------------------------------------------
    try:
        class Rapid:
            __slots__ = ('counter',)
            def __init__(self):
                self.counter = 0

        def set_rapid_counter(obj, val):
            obj.counter = val

        r = Rapid()
        for _ in range(WARMUP):
            set_rapid_counter(r, 0)
        check_jit_compiled(set_rapid_counter, "set_rapid_counter")

        for i in range(1000):
            set_rapid_counter(r, i)
            assert r.counter == i, f"cycle {i}: expected {i}, got {r.counter}"
        print("  PASS: test_rapid_slot_stores")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_slot_stores — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Value type changes on store (int -> str -> list -> None)
    # ------------------------------------------------------------------
    try:
        class TypeChanger:
            __slots__ = ('val',)
            def __init__(self):
                self.val = 0

        def set_tc_val(obj, val):
            obj.val = val

        tc = TypeChanger()
        for _ in range(WARMUP):
            set_tc_val(tc, 0)
        check_jit_compiled(set_tc_val, "set_tc_val")

        set_tc_val(tc, 42)
        assert tc.val == 42

        set_tc_val(tc, "hello")
        assert tc.val == "hello"

        set_tc_val(tc, [1, 2, 3])
        assert tc.val == [1, 2, 3]

        set_tc_val(tc, None)
        assert tc.val is None

        set_tc_val(tc, {"key": "value"})
        assert tc.val == {"key": "value"}
        print("  PASS: test_value_type_changes_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_value_type_changes_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Stability — 10000 store-read cycles
    # ------------------------------------------------------------------
    try:
        class Stable:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 0

        def set_stable_x(obj, val):
            obj.x = val

        st = Stable()
        for _ in range(WARMUP):
            set_stable_x(st, 42)
        check_jit_compiled(set_stable_x, "set_stable_x")

        for i in range(10000):
            set_stable_x(st, 42)
            assert st.x == 42, f"iteration {i}: got {st.x}, expected 42"
        print("  PASS: test_stability_10000_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Polymorphic store across slotted types
    # ------------------------------------------------------------------
    try:
        class Dog:
            __slots__ = ('name',)
            def __init__(self):
                self.name = ""

        class Cat:
            __slots__ = ('name',)
            def __init__(self):
                self.name = ""

        class Fish:
            __slots__ = ('name',)
            def __init__(self):
                self.name = ""

        def set_animal_name(obj, val):
            obj.name = val

        dog = Dog()
        for _ in range(WARMUP):
            set_animal_name(dog, "Rex")
        check_jit_compiled(set_animal_name, "set_animal_name")

        set_animal_name(dog, "Rex")
        cat = Cat()
        set_animal_name(cat, "Whiskers")
        fish = Fish()
        set_animal_name(fish, "Nemo")

        assert dog.name == "Rex"
        assert cat.name == "Whiskers"
        assert fish.name == "Nemo"
        print("  PASS: test_polymorphic_slotted_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_polymorphic_slotted_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Rapid type alternation on store (50 cycles)
    # ------------------------------------------------------------------
    try:
        class SA:
            __slots__ = ('val',)
            def __init__(self):
                self.val = 0

        class SB:
            __slots__ = ('val',)
            def __init__(self):
                self.val = 0

        def set_alt_val(obj, val):
            obj.val = val

        a = SA()
        for _ in range(WARMUP):
            set_alt_val(a, 0)
        check_jit_compiled(set_alt_val, "set_alt_val")

        for cycle in range(50):
            obj_a = SA()
            obj_b = SB()
            set_alt_val(obj_a, cycle * 2)
            set_alt_val(obj_b, cycle * 2 + 1)
            assert obj_a.val == cycle * 2, f"SA failed at cycle {cycle}"
            assert obj_b.val == cycle * 2 + 1, f"SB failed at cycle {cycle}"
        print("  PASS: test_rapid_type_alternation_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Empty __slots__ parent with child slots
    # ------------------------------------------------------------------
    try:
        class EmptySlots:
            __slots__ = ()

        class WithAttr(EmptySlots):
            __slots__ = ('x',)
            def __init__(self):
                self.x = 0

        def set_with_attr_x(obj, val):
            obj.x = val

        # EmptySlots has no attributes
        e = EmptySlots()
        got_error = False
        try:
            e.x = 5
        except AttributeError:
            got_error = True
        assert got_error, "EmptySlots should not allow attribute assignment"

        w = WithAttr()
        for _ in range(WARMUP):
            set_with_attr_x(w, 1)
        check_jit_compiled(set_with_attr_x, "set_with_attr_x")

        set_with_attr_x(w, 77)
        assert w.x == 77
        print("  PASS: test_empty_slots_parent_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_slots_parent_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Mixed __slots__ and __dict__ store
    # ------------------------------------------------------------------
    try:
        class DictBase:
            pass  # has __dict__

        class MixedCorrect(DictBase):
            __slots__ = ('slot_attr',)
            def __init__(self):
                self.slot_attr = 0
                self.dict_attr = 0

        def set_mixed_slot(obj, val):
            obj.slot_attr = val

        def set_mixed_dict(obj, val):
            obj.dict_attr = val

        mx = MixedCorrect()
        for _ in range(WARMUP):
            set_mixed_slot(mx, 1)
            set_mixed_dict(mx, 2)
        check_jit_compiled(set_mixed_slot, "set_mixed_slot")

        set_mixed_slot(mx, 99)
        set_mixed_dict(mx, 88)
        assert mx.slot_attr == 99
        assert mx.dict_attr == 88

        # Independent mutation
        set_mixed_slot(mx, 111)
        assert mx.slot_attr == 111
        assert mx.dict_attr == 88  # unchanged
        print("  PASS: test_mixed_slots_and_dict_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_slots_and_dict_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained slot store (obj.inner.x = val)
    # ------------------------------------------------------------------
    try:
        class Inner:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 0

        class Outer:
            __slots__ = ('inner',)
            def __init__(self, inner):
                self.inner = inner

        def set_inner_x(obj, val):
            obj.inner.x = val

        o = Outer(Inner())
        for _ in range(WARMUP):
            set_inner_x(o, 1)
        check_jit_compiled(set_inner_x, "set_inner_x")

        set_inner_x(o, 42)
        assert o.inner.x == 42

        # Replace inner, store again
        o.inner = Inner()
        set_inner_x(o, 99)
        assert o.inner.x == 99
        print("  PASS: test_chained_slot_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_slot_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Store callable to slot
    # ------------------------------------------------------------------
    try:
        class Callback:
            __slots__ = ('func',)
            def __init__(self):
                self.func = None

        def set_cb_func(obj, val):
            obj.func = val

        def my_add(a, b):
            return a + b

        cb = Callback()
        for _ in range(WARMUP):
            set_cb_func(cb, my_add)
        check_jit_compiled(set_cb_func, "set_cb_func")

        set_cb_func(cb, my_add)
        assert cb.func is my_add
        assert cb.func(3, 4) == 7

        # Replace with lambda
        mul_fn = lambda a, b: a * b
        set_cb_func(cb, mul_fn)
        assert cb.func(3, 4) == 12

        # Replace with None
        set_cb_func(cb, None)
        assert cb.func is None
        print("  PASS: test_store_callable_to_slot")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_store_callable_to_slot — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — obj.x = val vs descriptor __set__
    # ------------------------------------------------------------------
    try:
        class Described:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 0

        def set_via_attr(obj, val):
            obj.x = val

        def set_via_descriptor(obj, val):
            type(obj).__dict__['x'].__set__(obj, val)

        desc = Described()
        for _ in range(WARMUP):
            set_via_attr(desc, 0)
        check_jit_compiled(set_via_attr, "set_via_attr")

        values = [42, 0, -1, "test", None, [1, 2], (3, 4)]
        for val in values:
            # Store via JIT attr
            set_via_attr(desc, val)
            r_attr = desc.x

            # Store via descriptor
            set_via_descriptor(desc, val)
            r_desc = desc.x

            if val is None:
                assert r_attr is None and r_desc is None, (
                    f"None mismatch: attr={r_attr}, desc={r_desc}"
                )
            else:
                assert r_attr == r_desc, (
                    f"Mismatch for {val}: attr result={r_attr}, "
                    f"descriptor result={r_desc}"
                )
        print("  PASS: test_equivalence_attr_vs_descriptor_store")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_attr_vs_descriptor_store — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nSTORE_ATTR_SLOT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
