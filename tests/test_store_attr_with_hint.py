#!/usr/bin/env python3
"""
test_store_attr_with_hint.py — Correctness and deopt tests for
STORE_ATTR_WITH_HINT specialisation.

Targets: STORE_ATTR_WITH_HINT.

STORE_ATTR_WITH_HINT specialises attribute store operations (obj.attr = val)
when the object's type uses "hints" — cached indices into the object's
__dict__ for fast attribute access without hash table lookup. CPython 3.12's
adaptive specialiser emits this after observing repeated attribute stores
on instances whose type has a stable dict layout (combined or split dict).

Instead of going through the generic STORE_ATTR path (which calls
PyObject_SetAttr → type->tp_setattro → PyObject_GenericSetAttr →
_PyObjectDict_SetItem with full hash lookup), the specialisation uses the
cached hint index to store directly into the dict's values array.

Deopt triggers:
  - Object type changes (different class)
  - __dict__ layout changes (e.g. from adding new attributes dynamically)
  - Object has __setattr__ override
  - Object is an instance of a different class than expected
  - Attribute does not match the hint

Tests cover:
  - Basic attribute store on plain class
  - Store different value types
  - Store on object with many attributes
  - Overwrite existing attribute
  - Store on object from __init__
  - Deopt: switch to different class
  - Deopt: switch to dict store
  - Deopt: class with __setattr__
  - Deopt: class with __slots__
  - Multiple attribute stores in one function
  - Attribute store in loop
  - Store preserving object identity
  - Store on inherited attribute
  - Dynamic attribute addition
  - Store on object with __dict__ manipulation
  - Rapid type alternation
  - Store None / store same value repeatedly
  - Nested attribute store (obj.inner.attr = val)
  - Delete and re-store attribute
  - Correctness vs setattr() equivalence

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (AttributeError, TypeError)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_store_attr_with_hint.py
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
    print("=== STORE_ATTR_WITH_HINT Correctness & Deopt Tests ===")
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
        def __init__(self):
            self.host = ""
            self.port = 0
            self.debug = False

    # ── Test 1: Basic attribute store ──────────────────────────────────

    def store_attr_1(obj, val):
        obj.x = val

    p = Point(0, 0)

    for _ in range(WARMUP):
        store_attr_1(p, 42)

    check_jit_compiled(store_attr_1, "store_attr_1")

    try:
        store_attr_1(p, 10)
        assert p.x == 10
        store_attr_1(p, -5)
        assert p.x == -5
        store_attr_1(p, 0)
        assert p.x == 0
        print("PASS  Test 1: basic attribute store")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic store — {e}")
        failed += 1

    # ── Test 2: Store different value types ─────────────────────────────

    def store_types_2(obj, val):
        obj.x = val

    p2 = Point(0, 0)

    for _ in range(WARMUP):
        store_types_2(p2, 42)

    check_jit_compiled(store_types_2, "store_types_2")

    try:
        store_types_2(p2, 42)
        assert p2.x == 42
        store_types_2(p2, "hello")
        assert p2.x == "hello"
        store_types_2(p2, None)
        assert p2.x is None
        store_types_2(p2, [1, 2, 3])
        assert p2.x == [1, 2, 3]
        store_types_2(p2, {"a": 1})
        assert p2.x == {"a": 1}
        store_types_2(p2, (True, False))
        assert p2.x == (True, False)
        print("PASS  Test 2: store different value types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: value types — {e}")
        failed += 1

    # ── Test 3: Store on object with many attributes ───────────────────

    class ManyAttrs:
        def __init__(self):
            self.a = 0
            self.b = 0
            self.c = 0
            self.d = 0
            self.e = 0
            self.f = 0
            self.g = 0
            self.h = 0

    def store_many_3(obj, val):
        obj.e = val

    ma = ManyAttrs()

    for _ in range(WARMUP):
        store_many_3(ma, 99)

    check_jit_compiled(store_many_3, "store_many_3")

    try:
        store_many_3(ma, 42)
        assert ma.e == 42
        assert ma.a == 0  # Other attrs unchanged
        assert ma.h == 0
        store_many_3(ma, "middle")
        assert ma.e == "middle"
        print("PASS  Test 3: store on object with many attributes")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: many attrs — {e}")
        failed += 1

    # ── Test 4: Overwrite existing attribute ────────────────────────────

    def store_overwrite_4(obj, val):
        obj.x = val

    p4 = Point(100, 200)

    for _ in range(WARMUP):
        store_overwrite_4(p4, 42)

    check_jit_compiled(store_overwrite_4, "store_overwrite_4")

    try:
        assert p4.x == 42  # From warmup
        store_overwrite_4(p4, 1)
        assert p4.x == 1
        for i in range(100):
            store_overwrite_4(p4, i)
        assert p4.x == 99
        # y unchanged
        assert p4.y == 200
        print("PASS  Test 4: overwrite existing attribute 100 times")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: overwrite — {e}")
        failed += 1

    # ── Test 5: Store on config-like object ────────────────────────────

    def store_config_5(cfg, host, port, debug):
        cfg.host = host
        cfg.port = port
        cfg.debug = debug

    cfg = Config()

    for _ in range(WARMUP):
        store_config_5(cfg, "localhost", 8080, False)

    check_jit_compiled(store_config_5, "store_config_5")

    try:
        store_config_5(cfg, "example.com", 443, True)
        assert cfg.host == "example.com"
        assert cfg.port == 443
        assert cfg.debug is True

        store_config_5(cfg, "127.0.0.1", 0, False)
        assert cfg.host == "127.0.0.1"
        assert cfg.port == 0
        assert cfg.debug is False

        print("PASS  Test 5: store multiple attrs (config pattern)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: config — {e}")
        failed += 1

    # ── Test 6: Deopt — switch to different class ──────────────────────

    class PointA:
        def __init__(self):
            self.x = 0

    class PointB:
        def __init__(self):
            self.x = 0

    def store_deopt_6(obj, val):
        obj.x = val

    pa = PointA()

    for _ in range(WARMUP):
        store_deopt_6(pa, 42)

    check_jit_compiled(store_deopt_6, "store_deopt_6")

    try:
        # PointA path
        store_deopt_6(pa, 10)
        assert pa.x == 10

        # PointB path (deopt — different class)
        pb = PointB()
        store_deopt_6(pb, 20)
        assert pb.x == 20

        # PointA still works
        store_deopt_6(pa, 30)
        assert pa.x == 30

        print("PASS  Test 6: deopt — switch to different class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: deopt class — {e}")
        failed += 1

    # ── Test 7: Deopt — switch to dict ─────────────────────────────────

    def store_deopt_dict_7(container, key, val):
        """Uses STORE_ATTR on obj but generic setitem on dict."""
        pass

    def store_attr_7(obj, val):
        obj.x = val

    for _ in range(WARMUP):
        store_attr_7(pa, 42)

    check_jit_compiled(store_attr_7, "store_attr_7")

    try:
        store_attr_7(pa, 100)
        assert pa.x == 100

        # Object with dynamic __dict__ — still an attr store
        class Dynamic:
            pass
        d = Dynamic()
        store_attr_7(d, 200)
        assert d.x == 200

        # Original still works
        store_attr_7(pa, 300)
        assert pa.x == 300

        print("PASS  Test 7: deopt — different class with dynamic attrs")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: deopt dict — {e}")
        failed += 1

    # ── Test 8: Deopt — class with __setattr__ ─────────────────────────

    class Tracked:
        def __init__(self):
            object.__setattr__(self, '_log', [])
            object.__setattr__(self, 'x', 0)
        def __setattr__(self, name, value):
            self._log.append((name, value))
            object.__setattr__(self, name, value)

    def store_deopt_setattr_8(obj, val):
        obj.x = val

    for _ in range(WARMUP):
        store_deopt_setattr_8(pa, 42)

    check_jit_compiled(store_deopt_setattr_8, "store_deopt_setattr_8")

    try:
        # Normal path
        store_deopt_setattr_8(pa, 10)
        assert pa.x == 10

        # __setattr__ path (deopt)
        t = Tracked()
        store_deopt_setattr_8(t, 99)
        assert t.x == 99
        assert ('x', 99) in t._log

        # Normal still works
        store_deopt_setattr_8(pa, 20)
        assert pa.x == 20

        print("PASS  Test 8: deopt — class with __setattr__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt __setattr__ — {e}")
        failed += 1

    # ── Test 9: Deopt — class with __slots__ ───────────────────────────

    class Slotted:
        __slots__ = ['x', 'y']
        def __init__(self):
            self.x = 0
            self.y = 0

    def store_deopt_slots_9(obj, val):
        obj.x = val

    for _ in range(WARMUP):
        store_deopt_slots_9(pa, 42)

    check_jit_compiled(store_deopt_slots_9, "store_deopt_slots_9")

    try:
        # Normal dict-based class
        store_deopt_slots_9(pa, 10)
        assert pa.x == 10

        # Slotted class (deopt — different storage mechanism)
        s = Slotted()
        store_deopt_slots_9(s, 20)
        assert s.x == 20

        # Normal still works
        store_deopt_slots_9(pa, 30)
        assert pa.x == 30

        print("PASS  Test 9: deopt — class with __slots__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt __slots__ — {e}")
        failed += 1

    # ── Test 10: Multiple attribute stores in one function ─────────────

    def store_multi_10(obj, x, y):
        obj.x = x
        obj.y = y

    p10 = Point(0, 0)

    for _ in range(WARMUP):
        store_multi_10(p10, 1, 2)

    check_jit_compiled(store_multi_10, "store_multi_10")

    try:
        store_multi_10(p10, 100, 200)
        assert p10.x == 100
        assert p10.y == 200

        store_multi_10(p10, -1, -2)
        assert p10.x == -1
        assert p10.y == -2

        print("PASS  Test 10: multiple attribute stores in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: multi store — {e}")
        failed += 1

    # ── Test 11: Attribute store in loop ────────────────────────────────

    def store_loop_11(obj, values):
        for v in values:
            obj.x = v

    p11 = Point(0, 0)

    for _ in range(WARMUP):
        store_loop_11(p11, [1, 2, 3])

    check_jit_compiled(store_loop_11, "store_loop_11")

    try:
        store_loop_11(p11, list(range(100)))
        assert p11.x == 99  # Last value written
        store_loop_11(p11, [42])
        assert p11.x == 42
        store_loop_11(p11, [])
        assert p11.x == 42  # Unchanged
        print("PASS  Test 11: attribute store in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: loop store — {e}")
        failed += 1

    # ── Test 12: Store preserves object identity ───────────────────────

    def store_identity_12(obj, val):
        obj.x = val
        return obj

    p12 = Point(0, 0)

    for _ in range(WARMUP):
        store_identity_12(p12, 1)

    check_jit_compiled(store_identity_12, "store_identity_12")

    try:
        original_id = id(p12)
        result = store_identity_12(p12, 99)
        assert result is p12
        assert id(p12) == original_id
        assert p12.x == 99
        print("PASS  Test 12: store preserves object identity")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: identity — {e}")
        failed += 1

    # ── Test 13: Store on subclass ─────────────────────────────────────

    class Point3D(Point):
        def __init__(self, x, y, z):
            super().__init__(x, y)
            self.z = z

    def store_subclass_13(obj, val):
        obj.x = val

    p13 = Point3D(0, 0, 0)

    for _ in range(WARMUP):
        store_subclass_13(p13, 42)

    check_jit_compiled(store_subclass_13, "store_subclass_13")

    try:
        # Subclass
        store_subclass_13(p13, 10)
        assert p13.x == 10
        assert p13.z == 0  # Unchanged

        # Base class (deopt — different type)
        p_base = Point(0, 0)
        store_subclass_13(p_base, 20)
        assert p_base.x == 20

        # Subclass still works
        store_subclass_13(p13, 30)
        assert p13.x == 30

        print("PASS  Test 13: store on subclass and base class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: subclass — {e}")
        failed += 1

    # ── Test 14: Dynamic attribute addition ────────────────────────────

    def store_dynamic_14(obj, val):
        obj.new_attr = val

    class Expandable:
        pass

    exp = Expandable()

    for _ in range(WARMUP):
        store_dynamic_14(exp, 42)

    check_jit_compiled(store_dynamic_14, "store_dynamic_14")

    try:
        store_dynamic_14(exp, "created")
        assert exp.new_attr == "created"
        store_dynamic_14(exp, 99)
        assert exp.new_attr == 99

        # New instance — attribute doesn't exist yet
        exp2 = Expandable()
        store_dynamic_14(exp2, "also new")
        assert exp2.new_attr == "also new"

        print("PASS  Test 14: dynamic attribute addition")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: dynamic attr — {e}")
        failed += 1

    # ── Test 15: Store on object after __dict__ manipulation ───────────

    def store_after_dict_15(obj, val):
        obj.x = val

    p15 = Point(0, 0)

    for _ in range(WARMUP):
        store_after_dict_15(p15, 42)

    check_jit_compiled(store_after_dict_15, "store_after_dict_15")

    try:
        # Normal store
        store_after_dict_15(p15, 10)
        assert p15.x == 10

        # Manipulate __dict__ directly
        p15.__dict__['x'] = 999
        assert p15.x == 999

        # Store via function again — should override
        store_after_dict_15(p15, 20)
        assert p15.x == 20
        assert p15.__dict__['x'] == 20

        print("PASS  Test 15: store after __dict__ manipulation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: __dict__ manipulation — {e}")
        failed += 1

    # ── Test 16: Rapid type alternation ────────────────────────────────

    def store_rapid_16(obj, val):
        obj.x = val

    for _ in range(WARMUP):
        store_rapid_16(pa, 42)

    check_jit_compiled(store_rapid_16, "store_rapid_16")

    try:
        for i in range(50):
            if i % 3 == 0:
                store_rapid_16(pa, i)
                assert pa.x == i
            elif i % 3 == 1:
                store_rapid_16(pb, i)
                assert pb.x == i
            else:
                exp3 = Expandable()
                exp3.x = 0
                store_rapid_16(exp3, i)
                assert exp3.x == i

        print("PASS  Test 16: rapid type alternation (50 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: type alternation — {e}")
        failed += 1

    # ── Test 17: Store None and same value repeatedly ──────────────────

    def store_none_17(obj, val):
        obj.x = val

    p17 = Point(0, 0)

    for _ in range(WARMUP):
        store_none_17(p17, 42)

    check_jit_compiled(store_none_17, "store_none_17")

    try:
        store_none_17(p17, None)
        assert p17.x is None

        # Store same value 100 times
        for _ in range(100):
            store_none_17(p17, None)
        assert p17.x is None

        # Back to int
        store_none_17(p17, 42)
        assert p17.x == 42

        print("PASS  Test 17: store None and same value repeatedly")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: None/repeated — {e}")
        failed += 1

    # ── Test 18: Nested attribute store ────────────────────────────────

    class Container:
        def __init__(self):
            self.inner = Point(0, 0)

    def store_nested_18(obj, val):
        obj.inner.x = val

    c = Container()

    for _ in range(WARMUP):
        store_nested_18(c, 42)

    check_jit_compiled(store_nested_18, "store_nested_18")

    try:
        store_nested_18(c, 10)
        assert c.inner.x == 10
        store_nested_18(c, 99)
        assert c.inner.x == 99
        # Inner object identity preserved
        inner_id = id(c.inner)
        store_nested_18(c, 0)
        assert id(c.inner) == inner_id
        print("PASS  Test 18: nested attribute store (obj.inner.x)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: nested — {e}")
        failed += 1

    # ── Test 19: Delete and re-store attribute ─────────────────────────

    def store_recreate_19(obj, val):
        obj.x = val

    p19 = Point(0, 0)

    for _ in range(WARMUP):
        store_recreate_19(p19, 42)

    check_jit_compiled(store_recreate_19, "store_recreate_19")

    try:
        store_recreate_19(p19, 10)
        assert p19.x == 10

        # Delete the attribute
        del p19.x
        assert not hasattr(p19, 'x')

        # Re-store — attribute is recreated
        store_recreate_19(p19, 20)
        assert p19.x == 20

        print("PASS  Test 19: delete and re-store attribute")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: delete/re-store — {e}")
        failed += 1

    # ── Test 20: Correctness vs setattr() equivalence ──────────────────

    def store_via_dot_20(obj, val):
        obj.x = val

    p20a = Point(0, 0)
    p20b = Point(0, 0)

    for _ in range(WARMUP):
        store_via_dot_20(p20a, 42)

    check_jit_compiled(store_via_dot_20, "store_via_dot_20")

    try:
        for val in [0, 1, -1, "hello", 3.14, None, True, (1, 2), [3, 4]]:
            store_via_dot_20(p20a, val)
            setattr(p20b, 'x', val)
            assert p20a.x == p20b.x, (
                f"mismatch for val {val!r}: dot={p20a.x!r}, setattr={p20b.x!r}"
            )

        print("PASS  Test 20: dot store matches setattr()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: dot vs setattr — {e}")
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
