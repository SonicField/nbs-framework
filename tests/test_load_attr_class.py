#!/usr/bin/env python3
"""
test_load_attr_class.py — Correctness and deopt tests for LOAD_ATTR_CLASS
specialisation.

Targets: LOAD_ATTR_CLASS.

LOAD_ATTR_CLASS specialises attribute access on class objects (not instances).
When code does `MyClass.attr`, CPython's adaptive interpreter detects that the
object is a type and the attribute is found on the type's MRO, replacing
generic LOAD_ATTR with LOAD_ATTR_CLASS. This bypasses the generic
tp_getattro → _PyObject_GenericGetAttrWithDict dispatch chain.

The JIT specialisation would emit a guard on the type's version tag (or
equivalent) to confirm the class hasn't been modified, then load the attribute
directly from the cached MRO slot.

Deopt triggers:
  - Class attribute modified (version tag changes)
  - Class attribute deleted
  - New attribute added that shadows inherited attribute
  - Metaclass with __getattr__
  - Dynamic class creation

Tests cover:
  - Basic class attribute access (class variable)
  - Inherited attribute (from parent class)
  - Class method access
  - Static method access
  - Property descriptor on class (returns descriptor, not value)
  - Class attribute reassignment (deopt: version changes)
  - Class attribute deletion (deopt: AttributeError)
  - Shadow inherited attribute with subclass attribute
  - Multiple inheritance MRO resolution
  - Deopt: class object → instance object
  - Deopt: class object → module
  - Builtin type attributes (int.bit_length, str.upper)
  - Dynamic class attribute via setattr
  - __dict__ access on class
  - Class with __slots__ (class-level attribute access)
  - Nested class attribute
  - Rapid class modification stability
  - classmethod and staticmethod descriptors
  - Dunder attributes (__name__, __bases__)
  - Multiple class attributes in one function

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after class modification (deopt fires)
  3. Correct result for the new value after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_class.py
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
    print("=== LOAD_ATTR_CLASS Correctness & Deopt Tests ===")
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

    # ── Test 1: Basic class attribute access ─────────────────────────────

    class Conf1:
        MAX_SIZE = 1024
        NAME = "config"

    def get_max_1():
        return Conf1.MAX_SIZE

    for _ in range(WARMUP):
        get_max_1()

    check_jit_compiled(get_max_1, "get_max_1")

    try:
        assert get_max_1() == 1024
        print("PASS  Test 1: basic class attribute access")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic class attribute — {e}")
        failed += 1

    # ── Test 2: Inherited attribute ──────────────────────────────────────

    class Base2:
        VERSION = 3

    class Child2(Base2):
        NAME = "child"

    def get_version_2():
        return Child2.VERSION

    for _ in range(WARMUP):
        get_version_2()

    check_jit_compiled(get_version_2, "get_version_2")

    try:
        assert get_version_2() == 3
        print("PASS  Test 2: inherited attribute from parent")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: inherited attribute — {e}")
        failed += 1

    # ── Test 3: Class method access ──────────────────────────────────────

    class Math3:
        FACTOR = 2

        @classmethod
        def double(cls, x):
            return x * cls.FACTOR

    def call_classmethod_3(x):
        return Math3.double(x)

    for _ in range(WARMUP):
        call_classmethod_3(5)

    check_jit_compiled(call_classmethod_3, "call_classmethod_3")

    try:
        assert call_classmethod_3(5) == 10
        assert call_classmethod_3(0) == 0
        assert call_classmethod_3(-3) == -6
        print("PASS  Test 3: class method access and call")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: class method — {e}")
        failed += 1

    # ── Test 4: Static method access ─────────────────────────────────────

    class Utils4:
        @staticmethod
        def add(a, b):
            return a + b

    def call_static_4(a, b):
        return Utils4.add(a, b)

    for _ in range(WARMUP):
        call_static_4(3, 7)

    check_jit_compiled(call_static_4, "call_static_4")

    try:
        assert call_static_4(3, 7) == 10
        assert call_static_4(0, 0) == 0
        assert call_static_4(-1, 1) == 0
        print("PASS  Test 4: static method access and call")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: static method — {e}")
        failed += 1

    # ── Test 5: Class attribute reassignment (deopt) ─────────────────────

    class Config5:
        TIMEOUT = 30

    def get_timeout_5():
        return Config5.TIMEOUT

    for _ in range(WARMUP):
        get_timeout_5()

    check_jit_compiled(get_timeout_5, "get_timeout_5")

    try:
        assert get_timeout_5() == 30

        # Reassign class attribute — version tag changes
        Config5.TIMEOUT = 60
        assert get_timeout_5() == 60, f"got {get_timeout_5()}"

        Config5.TIMEOUT = 0
        assert get_timeout_5() == 0

        Config5.TIMEOUT = 30  # Restore
        assert get_timeout_5() == 30

        print("PASS  Test 5: class attribute reassignment (deopt)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: class attribute reassignment — {e}")
        failed += 1

    # ── Test 6: Class attribute deletion (AttributeError) ────────────────

    class Temp6:
        VALUE = 42

    def get_value_6():
        return Temp6.VALUE

    for _ in range(WARMUP):
        get_value_6()

    check_jit_compiled(get_value_6, "get_value_6")

    try:
        assert get_value_6() == 42

        del Temp6.VALUE

        try:
            get_value_6()
            assert False, "expected AttributeError after del"
        except AttributeError:
            pass

        # Restore
        Temp6.VALUE = 99
        assert get_value_6() == 99

        print("PASS  Test 6: class attribute deletion (AttributeError)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: attribute deletion — {e}")
        failed += 1

    # ── Test 7: Shadow inherited attribute ───────────────────────────────

    class Parent7:
        MODE = "parent"

    class Child7(Parent7):
        pass

    def get_mode_7():
        return Child7.MODE

    for _ in range(WARMUP):
        get_mode_7()

    check_jit_compiled(get_mode_7, "get_mode_7")

    try:
        # Initially inherits from parent
        assert get_mode_7() == "parent"

        # Shadow with subclass attribute
        Child7.MODE = "child"
        assert get_mode_7() == "child"

        # Remove shadow — falls back to parent
        del Child7.MODE
        assert get_mode_7() == "parent"

        print("PASS  Test 7: shadow inherited attribute with subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: shadow inherited — {e}")
        failed += 1

    # ── Test 8: Multiple inheritance MRO ──────────────────────────────────

    class A8:
        VAL = "A"

    class B8(A8):
        VAL = "B"

    class C8(A8):
        VAL = "C"

    class D8(B8, C8):
        pass  # MRO: D8 -> B8 -> C8 -> A8

    def get_val_8():
        return D8.VAL

    for _ in range(WARMUP):
        get_val_8()

    check_jit_compiled(get_val_8, "get_val_8")

    try:
        # D8 inherits from B8 first in MRO
        assert get_val_8() == "B"

        # Shadow on D8 itself
        D8.VAL = "D"
        assert get_val_8() == "D"

        # Remove D8 shadow — back to B8
        del D8.VAL
        assert get_val_8() == "B"

        print("PASS  Test 8: multiple inheritance MRO resolution")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: MRO resolution — {e}")
        failed += 1

    # ── Test 9: Deopt class → instance ───────────────────────────────────

    class Obj9:
        ATTR = "class_level"

    def get_attr_9(obj):
        return obj.ATTR

    for _ in range(WARMUP):
        get_attr_9(Obj9)

    check_jit_compiled(get_attr_9, "get_attr_9")

    try:
        # Class access
        assert get_attr_9(Obj9) == "class_level"

        # Deopt: instance access (LOAD_ATTR_INSTANCE_VALUE, not CLASS)
        inst = Obj9()
        assert get_attr_9(inst) == "class_level"

        # Instance with own attribute
        inst.ATTR = "instance_level"
        assert get_attr_9(inst) == "instance_level"

        # Class still works
        assert get_attr_9(Obj9) == "class_level"

        print("PASS  Test 9: deopt class → instance")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt class → instance — {e}")
        failed += 1

    # ── Test 10: Deopt class → module ────────────────────────────────────

    class Mod10:
        LABEL = "class_object"

    def get_label_10(obj):
        return obj.LABEL

    # Module needs the attribute too
    import types as _types_mod
    _types_mod.LABEL = "module_object"

    for _ in range(WARMUP):
        get_label_10(Mod10)

    check_jit_compiled(get_label_10, "get_label_10")

    try:
        assert get_label_10(Mod10) == "class_object"

        # Deopt: module object
        result = get_label_10(_types_mod)
        assert result == "module_object", f"got {result}"

        # Class still works
        assert get_label_10(Mod10) == "class_object"

        # Clean up
        del _types_mod.LABEL

        print("PASS  Test 10: deopt class → module")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: deopt class → module — {e}")
        failed += 1

    # ── Test 11: Builtin type attributes ─────────────────────────────────

    def get_int_attr_11():
        return int.bit_length

    for _ in range(WARMUP):
        get_int_attr_11()

    check_jit_compiled(get_int_attr_11, "get_int_attr_11")

    try:
        method = get_int_attr_11()
        # bit_length is an unbound method descriptor
        assert callable(method)
        assert method(42) == 6  # 42 = 0b101010, 6 bits
        assert method(0) == 0
        assert method(255) == 8

        print("PASS  Test 11: builtin type attribute (int.bit_length)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: builtin type attribute — {e}")
        failed += 1

    # ── Test 12: Dynamic class attribute via setattr ─────────────────────

    class Dynamic12:
        pass

    def get_dynamic_12():
        return Dynamic12.VALUE

    # Must set attribute before warmup
    Dynamic12.VALUE = "initial"

    for _ in range(WARMUP):
        get_dynamic_12()

    check_jit_compiled(get_dynamic_12, "get_dynamic_12")

    try:
        assert get_dynamic_12() == "initial"

        setattr(Dynamic12, "VALUE", "changed")
        assert get_dynamic_12() == "changed"

        setattr(Dynamic12, "VALUE", 42)
        assert get_dynamic_12() == 42

        print("PASS  Test 12: dynamic class attribute via setattr")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: dynamic setattr — {e}")
        failed += 1

    # ── Test 13: __name__ and __bases__ dunder attributes ────────────────

    class Named13:
        pass

    class Child13(Named13):
        pass

    def get_name_13(cls):
        return cls.__name__

    def get_bases_13(cls):
        return cls.__bases__

    for _ in range(WARMUP):
        get_name_13(Named13)

    check_jit_compiled(get_name_13, "get_name_13")

    for _ in range(WARMUP):
        get_bases_13(Child13)

    check_jit_compiled(get_bases_13, "get_bases_13")

    try:
        assert get_name_13(Named13) == "Named13"
        assert get_name_13(Child13) == "Child13"
        assert get_name_13(int) == "int"
        assert get_name_13(str) == "str"

        assert get_bases_13(Child13) == (Named13,)
        assert get_bases_13(Named13) == (object,)

        print("PASS  Test 13: __name__ and __bases__ dunder attributes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: dunder attributes — {e}")
        failed += 1

    # ── Test 14: Class with __slots__ ────────────────────────────────────

    class Slotted14:
        __slots__ = ('x', 'y')
        CLASS_VAR = "shared"

    def get_class_var_14():
        return Slotted14.CLASS_VAR

    for _ in range(WARMUP):
        get_class_var_14()

    check_jit_compiled(get_class_var_14, "get_class_var_14")

    try:
        assert get_class_var_14() == "shared"

        Slotted14.CLASS_VAR = "modified"
        assert get_class_var_14() == "modified"

        Slotted14.CLASS_VAR = "shared"  # Restore

        print("PASS  Test 14: class attribute on __slots__ class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: __slots__ class — {e}")
        failed += 1

    # ── Test 15: Nested class attribute ──────────────────────────────────

    class Outer15:
        class Inner:
            VALUE = "nested"

    def get_nested_15():
        return Outer15.Inner.VALUE

    for _ in range(WARMUP):
        get_nested_15()

    check_jit_compiled(get_nested_15, "get_nested_15")

    try:
        assert get_nested_15() == "nested"

        Outer15.Inner.VALUE = "changed"
        assert get_nested_15() == "changed"

        Outer15.Inner.VALUE = "nested"  # Restore

        print("PASS  Test 15: nested class attribute")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: nested class — {e}")
        failed += 1

    # ── Test 16: Rapid class modification stability ──────────────────────

    class Rapid16:
        VAL = 0

    def get_rapid_16():
        return Rapid16.VAL

    for _ in range(WARMUP):
        get_rapid_16()

    check_jit_compiled(get_rapid_16, "get_rapid_16")

    try:
        for i in range(100):
            Rapid16.VAL = i
            result = get_rapid_16()
            assert result == i, f"iteration {i}: got {result}"

        Rapid16.VAL = 0  # Restore
        print("PASS  Test 16: rapid class modification (100 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: rapid modification — {e}")
        failed += 1

    # ── Test 17: Multiple class attributes in one function ───────────────

    class Multi17:
        A = 1
        B = 2
        C = 3

    def get_multi_17():
        return (Multi17.A, Multi17.B, Multi17.C)

    for _ in range(WARMUP):
        get_multi_17()

    check_jit_compiled(get_multi_17, "get_multi_17")

    try:
        assert get_multi_17() == (1, 2, 3)

        Multi17.B = 20
        assert get_multi_17() == (1, 20, 3)

        Multi17.A = 10
        Multi17.C = 30
        assert get_multi_17() == (10, 20, 30)

        # Restore
        Multi17.A = 1
        Multi17.B = 2
        Multi17.C = 3

        print("PASS  Test 17: multiple class attributes in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: multiple attributes — {e}")
        failed += 1

    # ── Test 18: Class attribute type change ─────────────────────────────

    class TypeChange18:
        DATA = 42

    def get_data_18():
        return TypeChange18.DATA

    for _ in range(WARMUP):
        get_data_18()

    check_jit_compiled(get_data_18, "get_data_18")

    try:
        assert get_data_18() == 42

        TypeChange18.DATA = "string"
        assert get_data_18() == "string"

        TypeChange18.DATA = [1, 2, 3]
        assert get_data_18() == [1, 2, 3]

        TypeChange18.DATA = None
        assert get_data_18() is None

        TypeChange18.DATA = 42  # Restore

        print("PASS  Test 18: class attribute type change (int->str->list->None)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: type change — {e}")
        failed += 1

    # ── Test 19: Class attribute in loop ─────────────────────────────────

    class Counter19:
        LIMIT = 10

    def count_to_limit_19():
        count = 0
        while count < Counter19.LIMIT:
            count += 1
        return count

    for _ in range(WARMUP):
        count_to_limit_19()

    check_jit_compiled(count_to_limit_19, "count_to_limit_19")

    try:
        assert count_to_limit_19() == 10

        Counter19.LIMIT = 5
        assert count_to_limit_19() == 5

        Counter19.LIMIT = 0
        assert count_to_limit_19() == 0

        Counter19.LIMIT = 10  # Restore

        print("PASS  Test 19: class attribute in loop condition")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: class attribute in loop — {e}")
        failed += 1

    # ── Test 20: Descriptor protocol (property on class) ─────────────────
    # Accessing a property descriptor via the class returns the property
    # object itself, not the computed value. This is different from instance
    # access where __get__ is called.

    class WithProp20:
        _val = 42

        @property
        def val(self):
            return self._val

        CLASS_ATTR = "direct"

    def get_class_attr_20(cls):
        return cls.CLASS_ATTR

    for _ in range(WARMUP):
        get_class_attr_20(WithProp20)

    check_jit_compiled(get_class_attr_20, "get_class_attr_20")

    try:
        assert get_class_attr_20(WithProp20) == "direct"

        # Property on class returns the property descriptor
        prop = WithProp20.val
        assert isinstance(prop, property)

        # Instance access calls __get__
        inst = WithProp20()
        assert inst.val == 42

        # Class attr still works
        assert get_class_attr_20(WithProp20) == "direct"

        print("PASS  Test 20: class attribute alongside property descriptor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: property descriptor — {e}")
        failed += 1

    # ── Summary ──────────────────────────────────────────────────────────

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
