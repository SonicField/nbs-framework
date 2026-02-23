#!/usr/bin/env python3
"""
test_load_attr_with_hint.py — Correctness and deopt tests for
LOAD_ATTR_WITH_HINT specialisation.

Targets: LOAD_ATTR_WITH_HINT.

LOAD_ATTR_WITH_HINT specialises attribute load operations (obj.attr) when
the object's type uses "hints" — cached indices into the object's __dict__
for fast attribute access without hash table lookup. CPython 3.12's adaptive
specialiser emits this after observing repeated attribute loads on instances
whose type has a stable dict layout.

Instead of going through the generic LOAD_ATTR path (which calls
PyObject_GetAttr → type->tp_getattro → PyObject_GenericGetAttr →
_PyObject_GenericGetAttrWithDict with full hash lookup), the specialisation
uses the cached hint index to load directly from the dict's values array.

Deopt triggers:
  - Object type changes (different class)
  - __dict__ layout changes (hint invalidation)
  - Object has __getattr__ or __getattribute__ override
  - Object is an instance of a different class than expected
  - Attribute does not exist (AttributeError)

Tests cover:
  - Basic attribute load on plain class
  - Load different value types
  - Load from object with many attributes
  - Load attribute set in __init__
  - Load multiple attributes in one function
  - Deopt: switch to different class
  - Deopt: switch to object with __getattr__
  - Deopt: switch to object with __getattribute__
  - Deopt: switch to object with __slots__
  - Load in loop (same attribute repeatedly)
  - Load after attribute mutation
  - Load from subclass
  - Load from object after __dict__ manipulation
  - Load non-existent attribute (AttributeError)
  - Dynamic attribute addition then load
  - Rapid type alternation
  - Load None-valued attribute
  - Nested attribute load (obj.inner.attr)
  - Load after delete and re-create
  - Correctness vs getattr() equivalence

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (AttributeError)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_with_hint.py
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
    print("=== LOAD_ATTR_WITH_HINT Correctness & Deopt Tests ===")
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

    class Point:
        def __init__(self, x, y):
            self.x = x
            self.y = y

    class Config:
        def __init__(self, host, port, debug):
            self.host = host
            self.port = port
            self.debug = debug

    # ── Test 1: Basic attribute load ───────────────────────────────────

    def load_attr_1(obj):
        return obj.x

    p = Point(42, 99)

    for _ in range(WARMUP):
        load_attr_1(p)

    check_jit_compiled(load_attr_1, "load_attr_1")

    try:
        assert load_attr_1(p) == 42
        p.x = 100
        assert load_attr_1(p) == 100
        p.x = -1
        assert load_attr_1(p) == -1
        print("PASS  Test 1: basic attribute load")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic load — {e}")
        failed += 1

    # ── Test 2: Load different value types ─────────────────────────────

    def load_types_2(obj):
        return obj.x

    p2 = Point(0, 0)

    for _ in range(WARMUP):
        load_types_2(p2)

    check_jit_compiled(load_types_2, "load_types_2")

    try:
        p2.x = 42
        assert load_types_2(p2) == 42
        p2.x = "hello"
        assert load_types_2(p2) == "hello"
        p2.x = None
        assert load_types_2(p2) is None
        p2.x = [1, 2, 3]
        assert load_types_2(p2) == [1, 2, 3]
        p2.x = {"a": 1}
        assert load_types_2(p2) == {"a": 1}
        p2.x = (True, False)
        assert load_types_2(p2) == (True, False)
        print("PASS  Test 2: load different value types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: value types — {e}")
        failed += 1

    # ── Test 3: Load from object with many attributes ──────────────────

    class ManyAttrs:
        def __init__(self):
            self.a = 1
            self.b = 2
            self.c = 3
            self.d = 4
            self.e = 5
            self.f = 6
            self.g = 7
            self.h = 8

    def load_many_3(obj):
        return obj.e

    ma = ManyAttrs()

    for _ in range(WARMUP):
        load_many_3(ma)

    check_jit_compiled(load_many_3, "load_many_3")

    try:
        assert load_many_3(ma) == 5
        ma.e = 99
        assert load_many_3(ma) == 99
        # Other attrs unchanged
        assert ma.a == 1
        assert ma.h == 8
        print("PASS  Test 3: load from object with many attributes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: many attrs — {e}")
        failed += 1

    # ── Test 4: Load attribute set in __init__ ─────────────────────────

    def load_init_4(cfg):
        return cfg.host

    cfg = Config("localhost", 8080, False)

    for _ in range(WARMUP):
        load_init_4(cfg)

    check_jit_compiled(load_init_4, "load_init_4")

    try:
        assert load_init_4(cfg) == "localhost"
        cfg.host = "example.com"
        assert load_init_4(cfg) == "example.com"
        print("PASS  Test 4: load attribute set in __init__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: __init__ attr — {e}")
        failed += 1

    # ── Test 5: Load multiple attributes in one function ───────────────

    def load_multi_5(obj):
        return obj.x, obj.y

    p5 = Point(10, 20)

    for _ in range(WARMUP):
        load_multi_5(p5)

    check_jit_compiled(load_multi_5, "load_multi_5")

    try:
        assert load_multi_5(p5) == (10, 20)
        p5.x = 100
        p5.y = 200
        assert load_multi_5(p5) == (100, 200)
        print("PASS  Test 5: load multiple attributes in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: multi load — {e}")
        failed += 1

    # ── Test 6: Deopt — switch to different class ──────────────────────

    class PointA:
        def __init__(self):
            self.x = 0

    class PointB:
        def __init__(self):
            self.x = 0

    def load_deopt_6(obj):
        return obj.x

    pa = PointA()
    pa.x = 10

    for _ in range(WARMUP):
        load_deopt_6(pa)

    check_jit_compiled(load_deopt_6, "load_deopt_6")

    try:
        assert load_deopt_6(pa) == 10

        pb = PointB()
        pb.x = 20
        assert load_deopt_6(pb) == 20

        assert load_deopt_6(pa) == 10

        print("PASS  Test 6: deopt — switch to different class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: deopt class — {e}")
        failed += 1

    # ── Test 7: Deopt — object with __getattr__ ───────────────────────

    class WithGetattr:
        def __init__(self):
            self.x = 42
        def __getattr__(self, name):
            return f"fallback:{name}"

    def load_deopt_getattr_7(obj):
        return obj.x

    for _ in range(WARMUP):
        load_deopt_getattr_7(pa)

    check_jit_compiled(load_deopt_getattr_7, "load_deopt_getattr_7")

    try:
        assert load_deopt_getattr_7(pa) == 10

        wg = WithGetattr()
        assert load_deopt_getattr_7(wg) == 42  # x exists, __getattr__ not called

        assert load_deopt_getattr_7(pa) == 10

        print("PASS  Test 7: deopt — object with __getattr__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: deopt __getattr__ — {e}")
        failed += 1

    # ── Test 8: Deopt — object with __getattribute__ ──────────────────

    class WithGetattribute:
        def __init__(self):
            object.__setattr__(self, 'x', 0)
        def __getattribute__(self, name):
            val = object.__getattribute__(self, name)
            if name == 'x':
                return val + 1000
            return val

    def load_deopt_getattribute_8(obj):
        return obj.x

    for _ in range(WARMUP):
        load_deopt_getattribute_8(pa)

    check_jit_compiled(load_deopt_getattribute_8, "load_deopt_getattribute_8")

    try:
        assert load_deopt_getattribute_8(pa) == 10

        wga = WithGetattribute()
        assert load_deopt_getattribute_8(wga) == 1000  # 0 + 1000

        assert load_deopt_getattribute_8(pa) == 10

        print("PASS  Test 8: deopt — object with __getattribute__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt __getattribute__ — {e}")
        failed += 1

    # ── Test 9: Deopt — object with __slots__ ──────────────────────────

    class Slotted:
        __slots__ = ['x', 'y']
        def __init__(self):
            self.x = 0
            self.y = 0

    def load_deopt_slots_9(obj):
        return obj.x

    for _ in range(WARMUP):
        load_deopt_slots_9(pa)

    check_jit_compiled(load_deopt_slots_9, "load_deopt_slots_9")

    try:
        assert load_deopt_slots_9(pa) == 10

        s = Slotted()
        s.x = 77
        assert load_deopt_slots_9(s) == 77

        assert load_deopt_slots_9(pa) == 10

        print("PASS  Test 9: deopt — object with __slots__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt __slots__ — {e}")
        failed += 1

    # ── Test 10: Load in loop ──────────────────────────────────────────

    def load_loop_10(obj, n):
        total = 0
        for _ in range(n):
            total += obj.x
        return total

    p10 = Point(3, 0)

    for _ in range(WARMUP):
        load_loop_10(p10, 5)

    check_jit_compiled(load_loop_10, "load_loop_10")

    try:
        assert load_loop_10(p10, 100) == 300
        p10.x = 7
        assert load_loop_10(p10, 10) == 70
        assert load_loop_10(p10, 0) == 0
        print("PASS  Test 10: load in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: loop load — {e}")
        failed += 1

    # ── Test 11: Load after attribute mutation ─────────────────────────

    def load_mutated_11(obj):
        return obj.x

    p11 = Point(0, 0)

    for _ in range(WARMUP):
        load_mutated_11(p11)

    check_jit_compiled(load_mutated_11, "load_mutated_11")

    try:
        for i in range(100):
            p11.x = i
            assert load_mutated_11(p11) == i
        print("PASS  Test 11: load after 100 mutations")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: mutation — {e}")
        failed += 1

    # ── Test 12: Load from subclass ────────────────────────────────────

    class Point3D(Point):
        def __init__(self, x, y, z):
            super().__init__(x, y)
            self.z = z

    def load_subclass_12(obj):
        return obj.x

    p12 = Point3D(10, 20, 30)

    for _ in range(WARMUP):
        load_subclass_12(p12)

    check_jit_compiled(load_subclass_12, "load_subclass_12")

    try:
        assert load_subclass_12(p12) == 10

        p_base = Point(99, 0)
        assert load_subclass_12(p_base) == 99

        assert load_subclass_12(p12) == 10

        print("PASS  Test 12: load from subclass and base class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: subclass — {e}")
        failed += 1

    # ── Test 13: Load after __dict__ manipulation ──────────────────────

    def load_dict_13(obj):
        return obj.x

    p13 = Point(0, 0)

    for _ in range(WARMUP):
        load_dict_13(p13)

    check_jit_compiled(load_dict_13, "load_dict_13")

    try:
        p13.x = 10
        assert load_dict_13(p13) == 10

        p13.__dict__['x'] = 999
        assert load_dict_13(p13) == 999

        p13.x = 20
        assert load_dict_13(p13) == 20

        print("PASS  Test 13: load after __dict__ manipulation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: __dict__ — {e}")
        failed += 1

    # ── Test 14: Load non-existent attribute (AttributeError) ─────────

    def load_missing_14(obj):
        return obj.x

    class Empty:
        pass

    for _ in range(WARMUP):
        load_missing_14(pa)

    check_jit_compiled(load_missing_14, "load_missing_14")

    try:
        assert load_missing_14(pa) == 10

        try:
            load_missing_14(Empty())
            assert False, "expected AttributeError"
        except AttributeError:
            pass

        # Still works after error
        assert load_missing_14(pa) == 10

        print("PASS  Test 14: AttributeError for non-existent attribute")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: AttributeError — {e}")
        failed += 1

    # ── Test 15: Dynamic attribute addition then load ──────────────────

    def load_dynamic_15(obj):
        return obj.new_attr

    class Expandable:
        pass

    exp = Expandable()
    exp.new_attr = "created"

    for _ in range(WARMUP):
        load_dynamic_15(exp)

    check_jit_compiled(load_dynamic_15, "load_dynamic_15")

    try:
        assert load_dynamic_15(exp) == "created"
        exp.new_attr = 99
        assert load_dynamic_15(exp) == 99

        exp2 = Expandable()
        exp2.new_attr = "also created"
        assert load_dynamic_15(exp2) == "also created"

        print("PASS  Test 15: dynamic attribute addition then load")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: dynamic attr — {e}")
        failed += 1

    # ── Test 16: Rapid type alternation ────────────────────────────────

    def load_rapid_16(obj):
        return obj.x

    for _ in range(WARMUP):
        load_rapid_16(pa)

    check_jit_compiled(load_rapid_16, "load_rapid_16")

    try:
        pb2 = PointB()
        pb2.x = 50
        s2 = Slotted()
        s2.x = 77

        for i in range(50):
            if i % 3 == 0:
                assert load_rapid_16(pa) == 10
            elif i % 3 == 1:
                assert load_rapid_16(pb2) == 50
            else:
                assert load_rapid_16(s2) == 77

        print("PASS  Test 16: rapid type alternation (50 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: type alternation — {e}")
        failed += 1

    # ── Test 17: Load None-valued attribute ────────────────────────────

    def load_none_17(obj):
        return obj.x

    p17 = Point(None, 0)

    for _ in range(WARMUP):
        load_none_17(p17)

    check_jit_compiled(load_none_17, "load_none_17")

    try:
        assert load_none_17(p17) is None
        p17.x = 0
        assert load_none_17(p17) == 0
        p17.x = None
        assert load_none_17(p17) is None
        print("PASS  Test 17: load None-valued attribute")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: None value — {e}")
        failed += 1

    # ── Test 18: Nested attribute load ─────────────────────────────────

    class Container:
        def __init__(self):
            self.inner = Point(42, 99)

    def load_nested_18(obj):
        return obj.inner.x

    c = Container()

    for _ in range(WARMUP):
        load_nested_18(c)

    check_jit_compiled(load_nested_18, "load_nested_18")

    try:
        assert load_nested_18(c) == 42
        c.inner.x = 10
        assert load_nested_18(c) == 10
        c.inner = Point(77, 88)
        assert load_nested_18(c) == 77
        print("PASS  Test 18: nested attribute load (obj.inner.x)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: nested — {e}")
        failed += 1

    # ── Test 19: Load after delete and re-create ───────────────────────

    def load_recreate_19(obj):
        return obj.x

    p19 = Point(10, 20)

    for _ in range(WARMUP):
        load_recreate_19(p19)

    check_jit_compiled(load_recreate_19, "load_recreate_19")

    try:
        assert load_recreate_19(p19) == 10

        del p19.x
        try:
            load_recreate_19(p19)
            assert False, "expected AttributeError after delete"
        except AttributeError:
            pass

        p19.x = 99
        assert load_recreate_19(p19) == 99

        print("PASS  Test 19: load after delete and re-create")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: delete/re-create — {e}")
        failed += 1

    # ── Test 20: Correctness vs getattr() equivalence ──────────────────

    def load_via_dot_20(obj):
        return obj.x

    p20 = Point(0, 0)

    for _ in range(WARMUP):
        load_via_dot_20(p20)

    check_jit_compiled(load_via_dot_20, "load_via_dot_20")

    try:
        for val in [0, 1, -1, "hello", 3.14, None, True, (1, 2), [3, 4]]:
            p20.x = val
            dot_result = load_via_dot_20(p20)
            getattr_result = getattr(p20, 'x')
            assert dot_result == getattr_result, (
                f"mismatch for val {val!r}: dot={dot_result!r}, "
                f"getattr={getattr_result!r}"
            )

        print("PASS  Test 20: dot load matches getattr()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: dot vs getattr — {e}")
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
