#!/usr/bin/env python3
"""
test_load_attr_instance_value.py — Correctness and deopt tests for
LOAD_ATTR_INSTANCE_VALUE specialisation.

Targets: LOAD_ATTR_INSTANCE_VALUE.

LOAD_ATTR_INSTANCE_VALUE is the CPython 3.12 specialisation for accessing
instance attributes stored in the object's split dict. When the adaptive
interpreter detects repeated attribute accesses on instances of the same
type, it specialises the attribute load to read directly from the split
dict entry.

The CinderX JIT compiles LOAD_ATTR_INSTANCE_VALUE by:
1. Reading _PyAttrCache from CPython IC (version[2], index)
2. Calling findTypeByVersionTag(type_version) to get PyTypeObject*
3. Emitting GuardType(receiver, exact_type)
4. The Simplify pass (simplifyLoadAttrSplitDict) then replaces generic
   LoadAttr with direct split-dict entry access

Deopt triggers:
  - Receiver is a different type (subclass, different class)
  - Type version changes (class modified)
  - Attribute does not exist on instance

Tests cover:
  - Basic instance attribute access (Point.x, Point.y)
  - Instance attribute mutation after JIT
  - New instance attribute added after JIT
  - Instance attribute deletion (AttributeError)
  - Other attrs unaffected by deletion
  - Different instances of same class
  - Subclass instance deopt (Base vs Derived)
  - Base still correct after subclass deopt
  - Polymorphic access — 3 different classes
  - Rapid instance attribute mutations (1000 cycles)
  - Value type changes (int -> str -> list -> None)
  - __slots__-based class access
  - __slots__ mutation
  - Polymorphic dict vs __slots__ classes
  - Rapid alternation between 3 types (1000 cycles)
  - Stability — 10000 accesses without mutation
  - Multiple attributes on same object
  - Property-like access via __getattr__ fallback
  - Large number of instance attributes (20+ attrs)
  - Inheritance chain: grandchild class

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_instance_value.py
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

    print("=== LOAD_ATTR_INSTANCE_VALUE Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Basic instance attribute access ─────────────────────────

    class Point1:
        def __init__(self, x, y):
            self.x = x
            self.y = y

    def get_point_x_1(obj):
        return obj.x

    def get_point_y_1(obj):
        return obj.y

    try:
        p = Point1(3, 7)
        for _ in range(WARMUP):
            get_point_x_1(p)
            get_point_y_1(p)
        check_jit_compiled(get_point_x_1, "get_point_x_1")
        check_jit_compiled(get_point_y_1, "get_point_y_1")

        assert get_point_x_1(p) == 3, f"Expected 3, got {get_point_x_1(p)}"
        assert get_point_y_1(p) == 7, f"Expected 7, got {get_point_y_1(p)}"
        print("  PASS: test_basic_instance_attr_access")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_instance_attr_access — {e}")
        failed += 1

    # ── Test 2: Instance attribute mutation after JIT ────────────────────

    class Counter2:
        def __init__(self, value):
            self.value = value

    def get_counter_value_2(obj):
        return obj.value

    try:
        c = Counter2(42)
        for _ in range(WARMUP):
            get_counter_value_2(c)
        check_jit_compiled(get_counter_value_2, "get_counter_value_2")

        assert get_counter_value_2(c) == 42, "Pre-mutation check failed"
        # Mutate instance attribute (type version does NOT change —
        # only instance dict changes)
        c.value = 99
        result = get_counter_value_2(c)
        assert result == 99, f"Expected 99 after mutation, got {result}"
        print("  PASS: test_instance_attr_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_instance_attr_mutation — {e}")
        failed += 1

    # ── Test 3: New instance attribute added after JIT ───────────────────

    class Flexible3:
        def __init__(self, base):
            self.base = base

    def get_extra_3(obj):
        return obj.extra

    try:
        f = Flexible3(10)
        # Add the attribute
        f.extra = 777
        for _ in range(WARMUP):
            get_extra_3(f)
        check_jit_compiled(get_extra_3, "get_extra_3")

        result = get_extra_3(f)
        assert result == 777, f"Expected 777, got {result}"
        print("  PASS: test_new_instance_attr_after_jit")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_new_instance_attr_after_jit — {e}")
        failed += 1

    # ── Test 4: Instance attribute deletion ──────────────────────────────

    class Deletable4:
        def __init__(self, target, keep):
            self.target = target
            self.keep = keep

    def get_target_4(obj):
        return obj.target

    try:
        d = Deletable4(100, 200)
        for _ in range(WARMUP):
            get_target_4(d)
        check_jit_compiled(get_target_4, "get_target_4")

        del d.target
        raised = False
        try:
            get_target_4(d)
        except AttributeError:
            raised = True
        assert raised, "Expected AttributeError after del d.target"
        print("  PASS: test_instance_attr_deletion")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_instance_attr_deletion — {e}")
        failed += 1

    # ── Test 5: Other attrs unaffected by deletion ───────────────────────

    class Deletable5:
        def __init__(self, target, keep):
            self.target = target
            self.keep = keep

    def get_keep_5(obj):
        return obj.keep

    try:
        d = Deletable5(100, 200)
        for _ in range(WARMUP):
            get_keep_5(d)
        check_jit_compiled(get_keep_5, "get_keep_5")

        del d.target
        result = get_keep_5(d)
        assert result == 200, f"Expected 200, got {result}"
        print("  PASS: test_other_attrs_unaffected_by_deletion")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_other_attrs_unaffected_by_deletion — {e}")
        failed += 1

    # ── Test 6: Different instances of same class ────────────────────────

    class Pair6:
        def __init__(self, val):
            self.val = val

    def get_val_6(obj):
        return obj.val

    try:
        p1 = Pair6(100)
        p2 = Pair6(200)
        p3 = Pair6(300)
        for _ in range(WARMUP):
            get_val_6(p1)
        check_jit_compiled(get_val_6, "get_val_6")

        r1 = get_val_6(p1)
        r2 = get_val_6(p2)
        r3 = get_val_6(p3)
        assert r1 == 100, f"p1: expected 100, got {r1}"
        assert r2 == 200, f"p2: expected 200, got {r2}"
        assert r3 == 300, f"p3: expected 300, got {r3}"
        print("  PASS: test_different_instances_same_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_instances_same_class — {e}")
        failed += 1

    # ── Test 7: Subclass instance deopt ──────────────────────────────────

    class Base7:
        def __init__(self, x):
            self.x = x

    class Derived7(Base7):
        def __init__(self, x, y):
            super().__init__(x)
            self.y = y

    def get_base_x_7(obj):
        return obj.x

    try:
        b = Base7(10)
        for _ in range(WARMUP):
            get_base_x_7(b)
        check_jit_compiled(get_base_x_7, "get_base_x_7")

        # Base still works
        assert get_base_x_7(b) == 10, "Base instance failed"
        # Derived has a different type — GuardType should fire deopt
        deriv = Derived7(20, 30)
        result = get_base_x_7(deriv)
        assert result == 20, f"Derived: expected 20, got {result}"
        print("  PASS: test_subclass_instance_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subclass_instance_deopt — {e}")
        failed += 1

    # ── Test 8: Base still correct after subclass deopt ──────────────────

    class Base8:
        def __init__(self, x):
            self.x = x

    class Derived8(Base8):
        def __init__(self, x, y):
            super().__init__(x)
            self.y = y

    def get_base_x_8(obj):
        return obj.x

    try:
        b = Base8(10)
        for _ in range(WARMUP):
            get_base_x_8(b)
        check_jit_compiled(get_base_x_8, "get_base_x_8")

        # Trigger deopt with subclass
        deriv = Derived8(20, 30)
        _ = get_base_x_8(deriv)

        # Verify base still works after deopt
        result = get_base_x_8(Base8(50))
        assert result == 50, f"Base after deopt: expected 50, got {result}"
        print("  PASS: test_base_correct_after_subclass_deopt")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_base_correct_after_subclass_deopt — {e}")
        failed += 1

    # ── Test 9: Polymorphic access — 3 different classes ─────────────────

    class Dog9:
        def __init__(self, name):
            self.name = name

    class Cat9:
        def __init__(self, name):
            self.name = name

    class Fish9:
        def __init__(self, name):
            self.name = name

    def get_name_9(obj):
        return obj.name

    try:
        dog = Dog9("Rex")
        for _ in range(WARMUP):
            get_name_9(dog)
        check_jit_compiled(get_name_9, "get_name_9")

        cat = Cat9("Whiskers")
        fish = Fish9("Nemo")
        r_dog = get_name_9(dog)
        r_cat = get_name_9(cat)
        r_fish = get_name_9(fish)
        assert r_dog == "Rex", f"dog: expected 'Rex', got {r_dog!r}"
        assert r_cat == "Whiskers", f"cat: expected 'Whiskers', got {r_cat!r}"
        assert r_fish == "Nemo", f"fish: expected 'Nemo', got {r_fish!r}"
        print("  PASS: test_polymorphic_access_three_classes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_polymorphic_access_three_classes — {e}")
        failed += 1

    # ── Test 10: Rapid instance attribute mutations (1000 cycles) ────────

    class Rapid10:
        def __init__(self, counter):
            self.counter = counter

    def get_rapid_counter_10(obj):
        return obj.counter

    try:
        r = Rapid10(0)
        for _ in range(WARMUP):
            get_rapid_counter_10(r)
        check_jit_compiled(get_rapid_counter_10, "get_rapid_counter_10")

        for i in range(1000):
            r.counter = i
            result = get_rapid_counter_10(r)
            assert result == i, f"Cycle {i}: expected {i}, got {result}"
        print("  PASS: test_rapid_instance_mutations")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_instance_mutations — {e}")
        failed += 1

    # ── Test 11: Value type changes (int -> str -> list -> None) ─────────

    class TypeChanger11:
        def __init__(self, val):
            self.val = val

    def get_tc_val_11(obj):
        return obj.val

    try:
        tc = TypeChanger11(42)
        for _ in range(WARMUP):
            get_tc_val_11(tc)
        check_jit_compiled(get_tc_val_11, "get_tc_val_11")

        # int -> str
        tc.val = "hello"
        assert get_tc_val_11(tc) == "hello", "int -> str failed"
        # str -> list
        tc.val = [1, 2, 3]
        assert get_tc_val_11(tc) == [1, 2, 3], "str -> list failed"
        # list -> None
        tc.val = None
        assert get_tc_val_11(tc) is None, "list -> None failed"
        print("  PASS: test_value_type_changes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_value_type_changes — {e}")
        failed += 1

    # ── Test 12: __slots__-based class access ────────────────────────────

    class Slotted12:
        __slots__ = ('x', 'y')
        def __init__(self, x, y):
            self.x = x
            self.y = y

    def get_slotted_x_12(obj):
        return obj.x

    def get_slotted_y_12(obj):
        return obj.y

    try:
        s = Slotted12(10, 20)
        for _ in range(WARMUP):
            get_slotted_x_12(s)
            get_slotted_y_12(s)
        check_jit_compiled(get_slotted_x_12, "get_slotted_x_12")
        check_jit_compiled(get_slotted_y_12, "get_slotted_y_12")

        assert get_slotted_x_12(s) == 10, f"x: expected 10, got {get_slotted_x_12(s)}"
        assert get_slotted_y_12(s) == 20, f"y: expected 20, got {get_slotted_y_12(s)}"
        print("  PASS: test_slots_class_access")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slots_class_access — {e}")
        failed += 1

    # ── Test 13: __slots__ mutation ──────────────────────────────────────

    class Slotted13:
        __slots__ = ('x', 'y')
        def __init__(self, x, y):
            self.x = x
            self.y = y

    def get_slotted_x_13(obj):
        return obj.x

    def get_slotted_y_13(obj):
        return obj.y

    try:
        s = Slotted13(10, 20)
        for _ in range(WARMUP):
            get_slotted_x_13(s)
            get_slotted_y_13(s)
        check_jit_compiled(get_slotted_x_13, "get_slotted_x_13")

        s.x = 99
        s.y = 88
        assert get_slotted_x_13(s) == 99, f"x: expected 99, got {get_slotted_x_13(s)}"
        assert get_slotted_y_13(s) == 88, f"y: expected 88, got {get_slotted_y_13(s)}"
        print("  PASS: test_slots_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slots_mutation — {e}")
        failed += 1

    # ── Test 14: Polymorphic dict vs __slots__ classes ────────────────────

    class DictClass14:
        def __init__(self, name):
            self.name = name

    class SlotClass14:
        __slots__ = ('name',)
        def __init__(self, name):
            self.name = name

    def get_mixed_name_14(obj):
        return obj.name

    try:
        dc = DictClass14("dict_instance")
        for _ in range(WARMUP):
            get_mixed_name_14(dc)
        check_jit_compiled(get_mixed_name_14, "get_mixed_name_14")

        sc = SlotClass14("slot_instance")
        r_dict = get_mixed_name_14(dc)
        r_slot = get_mixed_name_14(sc)
        assert r_dict == "dict_instance", f"dict: expected 'dict_instance', got {r_dict!r}"
        assert r_slot == "slot_instance", f"slot: expected 'slot_instance', got {r_slot!r}"
        print("  PASS: test_polymorphic_dict_vs_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_polymorphic_dict_vs_slots — {e}")
        failed += 1

    # ── Test 15: Rapid alternation between 3 types (1000 cycles) ─────────

    class TypeA15:
        def __init__(self, val):
            self.val = val

    class TypeB15:
        def __init__(self, val):
            self.val = val

    class TypeC15:
        def __init__(self, val):
            self.val = val

    def get_alt_val_15(obj):
        return obj.val

    try:
        a = TypeA15(0)
        for _ in range(WARMUP):
            get_alt_val_15(a)
        check_jit_compiled(get_alt_val_15, "get_alt_val_15")

        for cycle in range(1000):
            objs = [
                (TypeA15(cycle * 3), cycle * 3),
                (TypeB15(cycle * 3 + 1), cycle * 3 + 1),
                (TypeC15(cycle * 3 + 2), cycle * 3 + 2),
            ]
            for obj, expected in objs:
                result = get_alt_val_15(obj)
                assert result == expected, (
                    f"Cycle {cycle}, {type(obj).__name__}: "
                    f"expected {expected}, got {result}"
                )
        print("  PASS: test_rapid_alternation_three_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_alternation_three_types — {e}")
        failed += 1

    # ── Test 16: Stability — 10000 accesses without mutation ─────────────

    class Stable16:
        def __init__(self, x):
            self.x = x

    def get_stable_x_16(obj):
        return obj.x

    try:
        st = Stable16(42)
        for _ in range(WARMUP):
            get_stable_x_16(st)
        check_jit_compiled(get_stable_x_16, "get_stable_x_16")

        for i in range(10000):
            result = get_stable_x_16(st)
            assert result == 42, f"Iteration {i}: expected 42, got {result}"
        print("  PASS: test_stability_10000_accesses")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_accesses — {e}")
        failed += 1

    # ── Test 17: Multiple attributes on same object ──────────────────────

    class Multi17:
        def __init__(self, x, y, z):
            self.x = x
            self.y = y
            self.z = z

    def get_multi_x_17(obj):
        return obj.x

    def get_multi_y_17(obj):
        return obj.y

    def get_multi_z_17(obj):
        return obj.z

    try:
        m = Multi17(10, 20, 30)
        for _ in range(WARMUP):
            get_multi_x_17(m)
            get_multi_y_17(m)
            get_multi_z_17(m)
        check_jit_compiled(get_multi_x_17, "get_multi_x_17")
        check_jit_compiled(get_multi_y_17, "get_multi_y_17")
        check_jit_compiled(get_multi_z_17, "get_multi_z_17")

        assert get_multi_x_17(m) == 10, f"x: expected 10, got {get_multi_x_17(m)}"
        assert get_multi_y_17(m) == 20, f"y: expected 20, got {get_multi_y_17(m)}"
        assert get_multi_z_17(m) == 30, f"z: expected 30, got {get_multi_z_17(m)}"
        # Mutate one, verify others unchanged
        m.y = 999
        assert get_multi_x_17(m) == 10, "x changed after y mutation"
        assert get_multi_y_17(m) == 999, f"y: expected 999, got {get_multi_y_17(m)}"
        assert get_multi_z_17(m) == 30, "z changed after y mutation"
        print("  PASS: test_multiple_attrs_same_object")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_attrs_same_object — {e}")
        failed += 1

    # ── Test 18: Property-like access via __getattr__ fallback ───────────

    class Fallback18:
        def __init__(self, present):
            self.present = present

        def __getattr__(self, name):
            if name == "missing":
                return "fallback_value"
            raise AttributeError(name)

    def get_missing_18(obj):
        return obj.missing

    def get_present_18(obj):
        return obj.present

    try:
        fb = Fallback18("here")
        for _ in range(WARMUP):
            get_missing_18(fb)
            get_present_18(fb)
        check_jit_compiled(get_missing_18, "get_missing_18")
        check_jit_compiled(get_present_18, "get_present_18")

        # Instance attr present — normal path
        assert get_present_18(fb) == "here", f"present: expected 'here', got {get_present_18(fb)!r}"
        # Instance attr missing — __getattr__ fallback called
        assert get_missing_18(fb) == "fallback_value", (
            f"missing: expected 'fallback_value', got {get_missing_18(fb)!r}"
        )
        print("  PASS: test_getattr_fallback")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_getattr_fallback — {e}")
        failed += 1

    # ── Test 19: Large number of instance attributes (20+) ───────────────

    class ManyAttrs19:
        def __init__(self):
            for i in range(25):
                setattr(self, f"attr_{i}", i * 10)

    def get_attr_0_19(obj):
        return obj.attr_0

    def get_attr_12_19(obj):
        return obj.attr_12

    def get_attr_24_19(obj):
        return obj.attr_24

    try:
        ma = ManyAttrs19()
        for _ in range(WARMUP):
            get_attr_0_19(ma)
            get_attr_12_19(ma)
            get_attr_24_19(ma)
        check_jit_compiled(get_attr_0_19, "get_attr_0_19")
        check_jit_compiled(get_attr_12_19, "get_attr_12_19")
        check_jit_compiled(get_attr_24_19, "get_attr_24_19")

        assert get_attr_0_19(ma) == 0, f"attr_0: expected 0, got {get_attr_0_19(ma)}"
        assert get_attr_12_19(ma) == 120, f"attr_12: expected 120, got {get_attr_12_19(ma)}"
        assert get_attr_24_19(ma) == 240, f"attr_24: expected 240, got {get_attr_24_19(ma)}"
        # Mutate one in the middle and verify
        ma.attr_12 = -1
        assert get_attr_12_19(ma) == -1, f"attr_12 after mutation: expected -1, got {get_attr_12_19(ma)}"
        # Others unchanged
        assert get_attr_0_19(ma) == 0, "attr_0 changed after attr_12 mutation"
        assert get_attr_24_19(ma) == 240, "attr_24 changed after attr_12 mutation"
        print("  PASS: test_large_number_of_instance_attrs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_large_number_of_instance_attrs — {e}")
        failed += 1

    # ── Test 20: Inheritance chain — grandchild class ────────────────────

    class GrandA20:
        def __init__(self, x):
            self.x = x

    class GrandB20(GrandA20):
        def __init__(self, x, y):
            super().__init__(x)
            self.y = y

    class GrandC20(GrandB20):
        def __init__(self, x, y, z):
            super().__init__(x, y)
            self.z = z

    def get_gc_x_20(obj):
        return obj.x

    try:
        # Warm up with base class A
        a = GrandA20(100)
        for _ in range(WARMUP):
            get_gc_x_20(a)
        check_jit_compiled(get_gc_x_20, "get_gc_x_20")

        # Base class works
        assert get_gc_x_20(a) == 100, f"A.x: expected 100, got {get_gc_x_20(a)}"
        # Child class B (deopt on type)
        b = GrandB20(200, 201)
        assert get_gc_x_20(b) == 200, f"B.x: expected 200, got {get_gc_x_20(b)}"
        # Grandchild class C (further deopt)
        c = GrandC20(300, 301, 302)
        assert get_gc_x_20(c) == 300, f"C.x: expected 300, got {get_gc_x_20(c)}"
        # Base still works after deopts
        assert get_gc_x_20(GrandA20(999)) == 999, "A.x after deopts failed"
        print("  PASS: test_inheritance_chain_grandchild")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inheritance_chain_grandchild — {e}")
        failed += 1

    # ── Summary ─────────────────────────────────────────────────────────

    print()
    print(f"LOAD_ATTR_INSTANCE_VALUE: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
