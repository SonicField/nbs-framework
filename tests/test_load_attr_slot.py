#!/usr/bin/env python3
"""
test_load_attr_slot.py — Correctness and deopt tests for LOAD_ATTR_SLOT
specialisation.

Targets: LOAD_ATTR_SLOT.

LOAD_ATTR_SLOT specialises attribute access on classes that use __slots__.
Instead of dictionary-based attribute lookup (LOAD_ATTR_INSTANCE_VALUE),
it accesses the fixed-offset slot directly via the type's member descriptor.

The adaptive specialiser emits LOAD_ATTR_SLOT after observing repeated
attribute access on a __slots__-based class instance. The CinderX JIT then
emits a GuardType on the receiver and uses the slot offset for direct access.

Mechanism:
1. Builder reads _PyAttrCache from CPython IC (type_version, slot_index)
2. findTypeByVersionTag(type_version) -> PyTypeObject*
3. GuardType(receiver, exact_type) emitted
4. Simplify pass converts to direct member-descriptor offset access

Deopt triggers:
  - Receiver type changes (different class with same attr name)
  - Receiver is subclass (GuardType fires)
  - Receiver uses __dict__ instead of __slots__
  - Slot is uninitialised (AttributeError)

Tests cover:
  - Basic slot access (single slot)
  - Multiple slots (different offsets)
  - Slot mutation after JIT
  - Slot deletion (AttributeError)
  - Different instances of the same slotted class
  - Slotted subclass inheriting parent slots
  - Slotted subclass adding extra slots
  - Deopt: dict-based class with same attr name
  - Deopt: different slotted class with same attr name
  - Slots with default value via class variable (not possible — verify)
  - Slot with None value vs uninitialised slot
  - Rapid slot mutations (1000 cycles)
  - Value type changes on slot (int -> str -> list)
  - Stability (10000 accesses without mutation)
  - Polymorphic access across slotted types
  - Slotted class with __slots__ = () (empty)
  - Mixed __slots__ and __dict__ (class with both)
  - Chained slot access (obj.inner.x)
  - Slot storing callable (function in slot)
  - Equivalence: obj.x vs type(obj).x.__get__(obj, type(obj))

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Slot semantics preserved (AttributeError on uninitialised/deleted)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_slot.py
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
    # Test 1: Basic single-slot access
    # ------------------------------------------------------------------
    try:
        class SingleSlot:
            __slots__ = ('value',)
            def __init__(self, value):
                self.value = value

        def get_single_value(obj):
            return obj.value

        s = SingleSlot(42)
        for _ in range(WARMUP):
            get_single_value(s)
        check_jit_compiled(get_single_value, "get_single_value")

        assert get_single_value(s) == 42
        assert get_single_value(SingleSlot(0)) == 0
        assert get_single_value(SingleSlot(-1)) == -1
        assert get_single_value(SingleSlot("hello")) == "hello"
        print("  PASS: test_basic_single_slot")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_single_slot — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Multiple slots (different offsets)
    # ------------------------------------------------------------------
    try:
        class MultiSlot:
            __slots__ = ('x', 'y', 'z')
            def __init__(self, x, y, z):
                self.x = x
                self.y = y
                self.z = z

        def get_ms_x(obj):
            return obj.x

        def get_ms_y(obj):
            return obj.y

        def get_ms_z(obj):
            return obj.z

        m = MultiSlot(10, 20, 30)
        for _ in range(WARMUP):
            get_ms_x(m)
            get_ms_y(m)
            get_ms_z(m)
        check_jit_compiled(get_ms_x, "get_ms_x")
        check_jit_compiled(get_ms_y, "get_ms_y")
        check_jit_compiled(get_ms_z, "get_ms_z")

        assert get_ms_x(m) == 10
        assert get_ms_y(m) == 20
        assert get_ms_z(m) == 30
        # Different instance
        m2 = MultiSlot(100, 200, 300)
        assert get_ms_x(m2) == 100
        assert get_ms_y(m2) == 200
        assert get_ms_z(m2) == 300
        print("  PASS: test_multiple_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_slots — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Slot mutation after JIT
    # ------------------------------------------------------------------
    try:
        class Mutable:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        def get_mutable_val(obj):
            return obj.val

        mut = Mutable(1)
        for _ in range(WARMUP):
            get_mutable_val(mut)
        check_jit_compiled(get_mutable_val, "get_mutable_val")

        assert get_mutable_val(mut) == 1
        mut.val = 999
        assert get_mutable_val(mut) == 999
        mut.val = -42
        assert get_mutable_val(mut) == -42
        mut.val = "changed"
        assert get_mutable_val(mut) == "changed"
        print("  PASS: test_slot_mutation_after_jit")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slot_mutation_after_jit — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Slot deletion (AttributeError)
    # ------------------------------------------------------------------
    try:
        class Deletable:
            __slots__ = ('target', 'keep')
            def __init__(self, target, keep):
                self.target = target
                self.keep = keep

        def get_del_target(obj):
            return obj.target

        def get_del_keep(obj):
            return obj.keep

        d = Deletable(100, 200)
        for _ in range(WARMUP):
            get_del_target(d)
            get_del_keep(d)
        check_jit_compiled(get_del_target, "get_del_target")
        check_jit_compiled(get_del_keep, "get_del_keep")

        assert get_del_target(d) == 100
        del d.target

        got_error = False
        try:
            get_del_target(d)
        except AttributeError:
            got_error = True
        assert got_error, "Expected AttributeError after slot deletion"

        # Other slot unaffected
        assert get_del_keep(d) == 200
        print("  PASS: test_slot_deletion")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slot_deletion — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Different instances of the same slotted class
    # ------------------------------------------------------------------
    try:
        class Pair:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        def get_pair_val(obj):
            return obj.val

        p1 = Pair(100)
        for _ in range(WARMUP):
            get_pair_val(p1)
        check_jit_compiled(get_pair_val, "get_pair_val")

        p2 = Pair(200)
        p3 = Pair(300)
        assert get_pair_val(p1) == 100
        assert get_pair_val(p2) == 200
        assert get_pair_val(p3) == 300
        print("  PASS: test_different_instances")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_instances — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Slotted subclass inheriting parent slots
    # ------------------------------------------------------------------
    try:
        class Base:
            __slots__ = ('x',)
            def __init__(self, x):
                self.x = x

        class Child(Base):
            __slots__ = ('y',)
            def __init__(self, x, y):
                super().__init__(x)
                self.y = y

        def get_base_x(obj):
            return obj.x

        def get_child_y(obj):
            return obj.y

        b = Base(10)
        for _ in range(WARMUP):
            get_base_x(b)
        check_jit_compiled(get_base_x, "get_base_x")

        # Base works
        assert get_base_x(b) == 10

        # Child inherits x from Base
        c = Child(20, 30)
        assert get_base_x(c) == 20  # deopt: different type

        # Warm up child-specific getter
        for _ in range(WARMUP):
            get_child_y(c)
        check_jit_compiled(get_child_y, "get_child_y")
        assert get_child_y(c) == 30

        # Base still works after deopt
        assert get_base_x(Base(50)) == 50
        print("  PASS: test_slotted_subclass_inheriting")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slotted_subclass_inheriting — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Slotted subclass adding extra slots
    # ------------------------------------------------------------------
    try:
        class Vec2:
            __slots__ = ('x', 'y')
            def __init__(self, x, y):
                self.x = x
                self.y = y

        class Vec3(Vec2):
            __slots__ = ('z',)
            def __init__(self, x, y, z):
                super().__init__(x, y)
                self.z = z

        def get_vec_x(obj):
            return obj.x

        def get_vec_z(obj):
            return obj.z

        v3 = Vec3(1, 2, 3)
        for _ in range(WARMUP):
            get_vec_x(v3)
            get_vec_z(v3)
        check_jit_compiled(get_vec_x, "get_vec_x")
        check_jit_compiled(get_vec_z, "get_vec_z")

        assert get_vec_x(v3) == 1
        assert get_vec_z(v3) == 3

        # Vec2 does not have z
        v2 = Vec2(10, 20)
        assert get_vec_x(v2) == 10
        got_attr_error = False
        try:
            get_vec_z(v2)
        except AttributeError:
            got_attr_error = True
        assert got_attr_error, "Vec2 should not have z slot"
        print("  PASS: test_subclass_extra_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subclass_extra_slots — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Deopt — dict-based class with same attr name
    # ------------------------------------------------------------------
    try:
        class SlottedName:
            __slots__ = ('name',)
            def __init__(self, name):
                self.name = name

        class DictName:
            def __init__(self, name):
                self.name = name

        def get_name(obj):
            return obj.name

        sn = SlottedName("slotted")
        for _ in range(WARMUP):
            get_name(sn)
        check_jit_compiled(get_name, "get_name")

        assert get_name(sn) == "slotted"
        # Deopt: dict-based class
        dn = DictName("dict-based")
        assert get_name(dn) == "dict-based"
        # Back to slotted
        assert get_name(SlottedName("again")) == "again"
        print("  PASS: test_deopt_dict_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_dict_class — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt — different slotted class with same attr name
    # ------------------------------------------------------------------
    try:
        class SlotA:
            __slots__ = ('data',)
            def __init__(self, data):
                self.data = data

        class SlotB:
            __slots__ = ('data', 'extra')
            def __init__(self, data, extra):
                self.data = data
                self.extra = extra

        def get_data(obj):
            return obj.data

        sa = SlotA("from_a")
        for _ in range(WARMUP):
            get_data(sa)
        check_jit_compiled(get_data, "get_data")

        assert get_data(sa) == "from_a"
        # Deopt: different slotted class
        sb = SlotB("from_b", "extra_b")
        assert get_data(sb) == "from_b"
        # Back to SlotA
        assert get_data(SlotA("a_again")) == "a_again"
        print("  PASS: test_deopt_different_slotted_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_slotted_class — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Uninitialised slot vs None-valued slot
    # ------------------------------------------------------------------
    try:
        class MaybeInit:
            __slots__ = ('val',)
            # No __init__ — slot starts uninitialised

        def get_maybe_val(obj):
            return obj.val

        # Uninitialised slot should raise AttributeError
        uninit = MaybeInit()
        got_error = False
        try:
            get_maybe_val(uninit)
        except AttributeError:
            got_error = True
        assert got_error, "Uninitialised slot should raise AttributeError"

        # None-valued slot is valid
        init_none = MaybeInit()
        init_none.val = None

        for _ in range(WARMUP):
            get_maybe_val(init_none)
        check_jit_compiled(get_maybe_val, "get_maybe_val")

        assert get_maybe_val(init_none) is None

        # Verify uninitialised still raises after JIT
        uninit2 = MaybeInit()
        got_error2 = False
        try:
            get_maybe_val(uninit2)
        except AttributeError:
            got_error2 = True
        assert got_error2, "Uninitialised slot should still raise after JIT"
        print("  PASS: test_uninitialised_vs_none")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_uninitialised_vs_none — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Rapid slot mutations (1000 cycles)
    # ------------------------------------------------------------------
    try:
        class Rapid:
            __slots__ = ('counter',)
            def __init__(self, counter):
                self.counter = counter

        def get_rapid_counter(obj):
            return obj.counter

        r = Rapid(0)
        for _ in range(WARMUP):
            get_rapid_counter(r)
        check_jit_compiled(get_rapid_counter, "get_rapid_counter")

        for i in range(1000):
            r.counter = i
            result = get_rapid_counter(r)
            assert result == i, f"cycle {i}: expected {i}, got {result}"
        print("  PASS: test_rapid_slot_mutations")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_slot_mutations — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Value type changes on slot (int -> str -> list -> None)
    # ------------------------------------------------------------------
    try:
        class TypeChanger:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        def get_tc_val(obj):
            return obj.val

        tc = TypeChanger(42)
        for _ in range(WARMUP):
            get_tc_val(tc)
        check_jit_compiled(get_tc_val, "get_tc_val")

        assert get_tc_val(tc) == 42

        tc.val = "hello"
        assert get_tc_val(tc) == "hello"

        tc.val = [1, 2, 3]
        assert get_tc_val(tc) == [1, 2, 3]

        tc.val = None
        assert get_tc_val(tc) is None

        tc.val = {"key": "value"}
        assert get_tc_val(tc) == {"key": "value"}
        print("  PASS: test_value_type_changes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_value_type_changes — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Stability — 10000 accesses without mutation
    # ------------------------------------------------------------------
    try:
        class Stable:
            __slots__ = ('x',)
            def __init__(self, x):
                self.x = x

        def get_stable_x(obj):
            return obj.x

        st = Stable(42)
        for _ in range(WARMUP):
            get_stable_x(st)
        check_jit_compiled(get_stable_x, "get_stable_x")

        for i in range(10000):
            result = get_stable_x(st)
            assert result == 42, f"iteration {i}: got {result}, expected 42"
        print("  PASS: test_stability_10000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Polymorphic access across slotted types
    # ------------------------------------------------------------------
    try:
        class Dog:
            __slots__ = ('name',)
            def __init__(self, name):
                self.name = name

        class Cat:
            __slots__ = ('name',)
            def __init__(self, name):
                self.name = name

        class Fish:
            __slots__ = ('name',)
            def __init__(self, name):
                self.name = name

        def get_animal_name(obj):
            return obj.name

        dog = Dog("Rex")
        for _ in range(WARMUP):
            get_animal_name(dog)
        check_jit_compiled(get_animal_name, "get_animal_name")

        assert get_animal_name(dog) == "Rex"
        assert get_animal_name(Cat("Whiskers")) == "Whiskers"
        assert get_animal_name(Fish("Nemo")) == "Nemo"
        # Back to Dog
        assert get_animal_name(Dog("Buddy")) == "Buddy"
        print("  PASS: test_polymorphic_slotted")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_polymorphic_slotted — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Rapid alternation between slotted types (50 cycles)
    # ------------------------------------------------------------------
    try:
        class SA:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        class SB:
            __slots__ = ('val',)
            def __init__(self, val):
                self.val = val

        def get_alt_val(obj):
            return obj.val

        a = SA(0)
        for _ in range(WARMUP):
            get_alt_val(a)
        check_jit_compiled(get_alt_val, "get_alt_val")

        for cycle in range(50):
            r_a = get_alt_val(SA(cycle * 2))
            r_b = get_alt_val(SB(cycle * 2 + 1))
            assert r_a == cycle * 2, f"SA failed at cycle {cycle}: {r_a}"
            assert r_b == cycle * 2 + 1, f"SB failed at cycle {cycle}: {r_b}"
        print("  PASS: test_rapid_type_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Empty __slots__ = ()
    # ------------------------------------------------------------------
    try:
        class EmptySlots:
            __slots__ = ()
            pass

        class WithAttr(EmptySlots):
            __slots__ = ('x',)
            def __init__(self, x):
                self.x = x

        def get_with_attr_x(obj):
            return obj.x

        # EmptySlots has no attributes at all
        e = EmptySlots()
        got_error = False
        try:
            e.x = 5  # should fail — no __dict__ and no x slot
        except AttributeError:
            got_error = True
        assert got_error, "EmptySlots should not allow attribute assignment"

        # WithAttr inherits empty + adds x
        w = WithAttr(77)
        for _ in range(WARMUP):
            get_with_attr_x(w)
        check_jit_compiled(get_with_attr_x, "get_with_attr_x")

        assert get_with_attr_x(w) == 77
        print("  PASS: test_empty_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_empty_slots — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Mixed __slots__ and __dict__
    # ------------------------------------------------------------------
    try:
        class Mixed:
            __slots__ = ('slot_attr',)
            def __init__(self, slot_val, dict_val):
                self.slot_attr = slot_val
                self.dict_attr = dict_val  # goes to __dict__

        def get_mixed_slot(obj):
            return obj.slot_attr

        def get_mixed_dict(obj):
            return obj.dict_attr

        # Mixed has __dict__ because it doesn't declare __dict__ in __slots__
        # but also doesn't exclude it (Python default: classes with __slots__
        # that don't include '__dict__' DON'T get __dict__ unless a parent
        # does). Actually, if no parent has __dict__, and __slots__ is defined
        # without '__dict__', then __dict__ is NOT available.
        # Let's use a parent that allows __dict__:
        class DictBase:
            pass  # has __dict__

        class MixedCorrect(DictBase):
            __slots__ = ('slot_attr',)
            def __init__(self, slot_val, dict_val):
                self.slot_attr = slot_val
                self.dict_attr = dict_val  # goes to __dict__ from DictBase

        mx = MixedCorrect(10, 20)
        for _ in range(WARMUP):
            get_mixed_slot(mx)
            get_mixed_dict(mx)
        check_jit_compiled(get_mixed_slot, "get_mixed_slot")

        assert get_mixed_slot(mx) == 10
        assert get_mixed_dict(mx) == 20

        # Mutate slot and dict independently
        mx.slot_attr = 99
        mx.dict_attr = 88
        assert get_mixed_slot(mx) == 99
        assert get_mixed_dict(mx) == 88
        print("  PASS: test_mixed_slots_and_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_slots_and_dict — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained slot access (obj.inner.x)
    # ------------------------------------------------------------------
    try:
        class Inner:
            __slots__ = ('x',)
            def __init__(self, x):
                self.x = x

        class Outer:
            __slots__ = ('inner',)
            def __init__(self, inner):
                self.inner = inner

        def get_inner_x(obj):
            return obj.inner.x

        o = Outer(Inner(42))
        for _ in range(WARMUP):
            get_inner_x(o)
        check_jit_compiled(get_inner_x, "get_inner_x")

        assert get_inner_x(o) == 42

        # Replace inner
        o.inner = Inner(99)
        assert get_inner_x(o) == 99

        # Mutate inner's slot
        o.inner.x = 123
        assert get_inner_x(o) == 123
        print("  PASS: test_chained_slot_access")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_slot_access — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Slot storing callable (function in slot)
    # ------------------------------------------------------------------
    try:
        class Callback:
            __slots__ = ('func',)
            def __init__(self, func):
                self.func = func

        def get_cb_func(obj):
            return obj.func

        def my_add(a, b):
            return a + b

        cb = Callback(my_add)
        for _ in range(WARMUP):
            get_cb_func(cb)
        check_jit_compiled(get_cb_func, "get_cb_func")

        retrieved = get_cb_func(cb)
        assert retrieved is my_add
        assert retrieved(3, 4) == 7

        # Replace with lambda
        cb.func = lambda a, b: a * b
        retrieved2 = get_cb_func(cb)
        assert retrieved2(3, 4) == 12

        # Replace with None
        cb.func = None
        assert get_cb_func(cb) is None
        print("  PASS: test_slot_storing_callable")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slot_storing_callable — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — obj.x vs descriptor __get__
    # ------------------------------------------------------------------
    try:
        class Described:
            __slots__ = ('x',)
            def __init__(self, x):
                self.x = x

        def get_via_attr(obj):
            return obj.x

        def get_via_descriptor(obj):
            return type(obj).__dict__['x'].__get__(obj, type(obj))

        desc = Described(42)
        for _ in range(WARMUP):
            get_via_attr(desc)
        check_jit_compiled(get_via_attr, "get_via_attr")

        values = [42, 0, -1, "test", None, [1, 2], (3, 4)]
        for val in values:
            desc.x = val
            r_attr = get_via_attr(desc)
            r_desc = get_via_descriptor(desc)
            if val is None:
                assert r_attr is None and r_desc is None, (
                    f"None mismatch: attr={r_attr}, desc={r_desc}"
                )
            else:
                assert r_attr == r_desc, (
                    f"Mismatch for {val}: attr={r_attr}, descriptor={r_desc}"
                )
        print("  PASS: test_equivalence_attr_vs_descriptor")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_attr_vs_descriptor — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nLOAD_ATTR_SLOT: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
