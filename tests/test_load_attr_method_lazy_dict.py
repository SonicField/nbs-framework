#!/usr/bin/env python3
"""
test_load_attr_method_lazy_dict.py — Correctness and deopt tests for
LOAD_ATTR_METHOD_LAZY_DICT specialisation.

Targets: LOAD_ATTR_METHOD_LAZY_DICT.

LOAD_ATTR_METHOD_LAZY_DICT is the CPython 3.12 specialisation for loading
methods from objects whose type has a lazily-initialised instance __dict__.
This applies to regular Python classes (those without __slots__) where the
instance __dict__ may or may not have been created yet.

When the adaptive interpreter detects repeated method lookups on objects
with a lazy dict, it specialises the load to check whether the dict exists
and whether it shadows the method, then loads directly from the type's
method cache if no shadowing is found.

The key difference from LOAD_ATTR_METHOD_NO_DICT: here the object CAN have
a __dict__, so the specialisation must check for instance-level shadowing
on every call. LOAD_ATTR_METHOD_NO_DICT skips this check entirely because
the type guarantees no per-instance dict.

The CinderX JIT compiles LOAD_ATTR_METHOD_LAZY_DICT by emitting a GuardType
on the receiver, then checking the dict slot and loading from the type's
method cache.

Deopt triggers:
  - Receiver type changes
  - Method is shadowed by an instance attribute
  - Method is replaced on the type
  - Type's MRO changes

Tests cover:
  - Method on regular class (no instance attrs)
  - Method on class with instance attrs (dict exists, no shadow)
  - Method after instance attr added (dict created)
  - Method shadowed by instance attr (deopt)
  - Method on subclass
  - Method after deleting shadow attr (un-shadow)
  - Multiple methods on same class
  - Inherited method with lazy dict
  - Deopt: method replaced on type
  - Deopt: different receiver type
  - Rapid method calls (1000 iterations)
  - Stability — 10000 calls
  - Method with args
  - Deep inheritance chain
  - Property vs method distinction
  - Method on class with __init__ setting attrs
  - Diamond inheritance method resolution
  - Deopt: MRO change via __bases__
  - Equivalence: obj.method() vs Type.method(obj)
  - Method call after __dict__ manipulation

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after shadowing/type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_method_lazy_dict.py
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

    print("=== LOAD_ATTR_METHOD_LAZY_DICT Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Method on regular class (no instance attrs) ─────────────

    class Simple:
        def greet(self):
            return "hello"

    def call_greet(obj):
        return obj.greet()

    try:
        s = Simple()
        for _ in range(WARMUP):
            call_greet(s)
        check_jit_compiled(call_greet, "call_greet")

        assert call_greet(s) == "hello"
        assert call_greet(Simple()) == "hello"
        print("  PASS: test_method_no_instance_attrs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_no_instance_attrs — {e}")
        failed += 1

    # ── Test 2: Method on class with instance attrs (no shadow) ─────────

    class WithAttrs:
        def __init__(self, x, y):
            self.x = x
            self.y = y

        def sum(self):
            return self.x + self.y

    def call_sum(obj):
        return obj.sum()

    try:
        w = WithAttrs(3, 4)
        for _ in range(WARMUP):
            call_sum(w)
        check_jit_compiled(call_sum, "call_sum")

        assert call_sum(w) == 7
        assert call_sum(WithAttrs(10, 20)) == 30
        print("  PASS: test_method_with_instance_attrs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_instance_attrs — {e}")
        failed += 1

    # ── Test 3: Method after instance attr added (dict created) ─────────

    class LazyDict:
        def compute(self):
            return 42

    def call_compute(obj):
        return obj.compute()

    try:
        obj = LazyDict()
        # No __dict__ content yet (lazy)
        for _ in range(WARMUP):
            call_compute(obj)
        check_jit_compiled(call_compute, "call_compute")

        assert call_compute(obj) == 42

        # Add an instance attr — creates/populates __dict__
        obj.extra = "bonus"
        # Method should still work (extra doesn't shadow compute)
        assert call_compute(obj) == 42
        assert obj.extra == "bonus"
        print("  PASS: test_method_after_dict_created")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_after_dict_created — {e}")
        failed += 1

    # ── Test 4: Method shadowed by instance attr (deopt) ────────────────

    class Shadowable:
        def action(self):
            return "from_class"

    def call_action(obj):
        return obj.action()

    try:
        obj = Shadowable()
        for _ in range(WARMUP):
            call_action(obj)
        check_jit_compiled(call_action, "call_action")

        assert call_action(obj) == "from_class"

        # Shadow the method with an instance attribute (a callable)
        obj.action = lambda: "from_instance"
        result = call_action(obj)
        assert result == "from_instance", (
            f"Shadowed: expected 'from_instance', got {result}"
        )
        print("  PASS: test_method_shadowed_by_instance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_shadowed_by_instance — {e}")
        failed += 1

    # ── Test 5: Method on subclass ──────────────────────────────────────

    class Parent:
        def identify(self):
            return "parent"

    class Child(Parent):
        def identify(self):
            return "child"

    def call_identify(obj):
        return obj.identify()

    try:
        c = Child()
        for _ in range(WARMUP):
            call_identify(c)
        check_jit_compiled(call_identify, "call_identify")

        assert call_identify(c) == "child"
        assert call_identify(Parent()) == "parent"
        print("  PASS: test_method_on_subclass")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_on_subclass — {e}")
        failed += 1

    # ── Test 6: Method after deleting shadow attr (un-shadow) ───────────

    class Unshadow:
        def value(self):
            return "method"

    def call_value(obj):
        return obj.value()

    try:
        obj = Unshadow()
        for _ in range(WARMUP):
            call_value(obj)
        check_jit_compiled(call_value, "call_value")

        assert call_value(obj) == "method"

        # Shadow it
        obj.value = lambda: "shadow"
        assert call_value(obj) == "shadow"

        # Un-shadow by deleting the instance attr
        del obj.value
        result = call_value(obj)
        assert result == "method", (
            f"After un-shadow: expected 'method', got {result}"
        )
        print("  PASS: test_unshadow_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_unshadow_method — {e}")
        failed += 1

    # ── Test 7: Multiple methods on same class ──────────────────────────

    class Multi:
        def __init__(self, val):
            self.val = val

        def double(self):
            return self.val * 2

        def triple(self):
            return self.val * 3

        def negate(self):
            return -self.val

    def call_multi(obj):
        return obj.double(), obj.triple(), obj.negate()

    try:
        m = Multi(5)
        for _ in range(WARMUP):
            call_multi(m)
        check_jit_compiled(call_multi, "call_multi")

        d, t, n = call_multi(m)
        assert d == 10, f"double: {d}"
        assert t == 15, f"triple: {t}"
        assert n == -5, f"negate: {n}"
        print("  PASS: test_multiple_methods")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_methods — {e}")
        failed += 1

    # ── Test 8: Inherited method with lazy dict ─────────────────────────

    class Base:
        def base_method(self):
            return "base"

    class Mid(Base):
        pass

    class Leaf(Mid):
        def __init__(self):
            self.leaf_attr = 99

    def call_base_method(obj):
        return obj.base_method()

    try:
        leaf = Leaf()
        for _ in range(WARMUP):
            call_base_method(leaf)
        check_jit_compiled(call_base_method, "call_base_method")

        assert call_base_method(leaf) == "base"
        assert leaf.leaf_attr == 99
        # Instance attr doesn't shadow base_method
        assert call_base_method(leaf) == "base"
        print("  PASS: test_inherited_method_lazy_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inherited_method_lazy_dict — {e}")
        failed += 1

    # ── Test 9: Deopt — method replaced on type ─────────────────────────

    class TypeReplace:
        def compute(self):
            return 10

    def call_type_replace(obj):
        return obj.compute()

    try:
        obj = TypeReplace()
        for _ in range(WARMUP):
            call_type_replace(obj)
        check_jit_compiled(call_type_replace, "call_type_replace")

        assert call_type_replace(obj) == 10

        original = TypeReplace.compute
        TypeReplace.compute = lambda self: 20
        try:
            result = call_type_replace(obj)
            assert result == 20, f"After replace: expected 20, got {result}"
        finally:
            TypeReplace.compute = original

        assert call_type_replace(obj) == 10
        print("  PASS: test_deopt_method_replaced_on_type")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_method_replaced_on_type — {e}")
        failed += 1

    # ── Test 10: Deopt — different receiver type ────────────────────────

    class TypeA:
        def __init__(self, v):
            self.v = v

        def get(self):
            return self.v

    class TypeB:
        def __init__(self, v):
            self.v = v

        def get(self):
            return self.v * 100

    def call_get(obj):
        return obj.get()

    try:
        a = TypeA(5)
        for _ in range(WARMUP):
            call_get(a)
        check_jit_compiled(call_get, "call_get")

        assert call_get(a) == 5
        b = TypeB(5)
        assert call_get(b) == 500
        print("  PASS: test_deopt_different_type")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_type — {e}")
        failed += 1

    # ── Test 11: Rapid method calls (1000 iterations) ───────────────────

    class Rapid:
        def __init__(self, n):
            self.n = n

        def inc(self):
            return self.n + 1

    def call_inc(obj):
        return obj.inc()

    try:
        r = Rapid(0)
        for _ in range(WARMUP):
            call_inc(r)
        check_jit_compiled(call_inc, "call_inc")

        for i in range(1000):
            r = Rapid(i)
            result = call_inc(r)
            assert result == i + 1, f"Iteration {i}: expected {i+1}, got {result}"
        print("  PASS: test_rapid_method_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_method_calls — {e}")
        failed += 1

    # ── Test 12: Stability — 10000 calls ────────────────────────────────

    class Stable:
        def __init__(self, x):
            self.x = x

        def sq(self):
            return self.x * self.x

    def call_sq(obj):
        return obj.sq()

    try:
        s = Stable(7)
        for _ in range(WARMUP):
            call_sq(s)
        check_jit_compiled(call_sq, "call_sq")

        for i in range(10000):
            result = call_sq(s)
            assert result == 49, f"Iteration {i}: expected 49, got {result}"
        print("  PASS: test_stability_10000_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_calls — {e}")
        failed += 1

    # ── Test 13: Method with args ───────────────────────────────────────

    class WithArgs:
        def __init__(self, base):
            self.base = base

        def add(self, x, y):
            return self.base + x + y

    def call_add(obj, x, y):
        return obj.add(x, y)

    try:
        w = WithArgs(100)
        for _ in range(WARMUP):
            call_add(w, 1, 2)
        check_jit_compiled(call_add, "call_add")

        assert call_add(w, 1, 2) == 103
        assert call_add(w, 0, 0) == 100
        assert call_add(WithArgs(0), 10, 20) == 30
        print("  PASS: test_method_with_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_args — {e}")
        failed += 1

    # ── Test 14: Deep inheritance chain ──────────────────────────────────

    class D0:
        def __init__(self):
            self.d0_attr = 0

        def root(self):
            return "root"

    class D1(D0):
        def __init__(self):
            super().__init__()
            self.d1_attr = 1

    class D2(D1):
        def __init__(self):
            super().__init__()
            self.d2_attr = 2

    class D3(D2):
        def __init__(self):
            super().__init__()
            self.d3_attr = 3

    def call_root(obj):
        return obj.root()

    try:
        d = D3()
        for _ in range(WARMUP):
            call_root(d)
        check_jit_compiled(call_root, "call_root")

        assert call_root(d) == "root"
        assert d.d3_attr == 3
        assert d.d0_attr == 0
        assert call_root(D0()) == "root"
        print("  PASS: test_deep_inheritance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deep_inheritance — {e}")
        failed += 1

    # ── Test 15: Property vs method distinction ─────────────────────────

    class WithProp:
        def __init__(self, val):
            self._val = val

        @property
        def val(self):
            return self._val

        def get_val(self):
            return self._val

    def call_get_val(obj):
        return obj.get_val()

    try:
        wp = WithProp(42)
        for _ in range(WARMUP):
            call_get_val(wp)
        check_jit_compiled(call_get_val, "call_get_val")

        assert call_get_val(wp) == 42
        assert wp.val == 42  # property access
        assert call_get_val(wp) == wp.val
        print("  PASS: test_property_vs_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_property_vs_method — {e}")
        failed += 1

    # ── Test 16: Method on class with __init__ setting attrs ────────────

    class Initialised:
        def __init__(self, a, b, c):
            self.a = a
            self.b = b
            self.c = c

        def total(self):
            return self.a + self.b + self.c

    def call_total(obj):
        return obj.total()

    try:
        init = Initialised(10, 20, 30)
        for _ in range(WARMUP):
            call_total(init)
        check_jit_compiled(call_total, "call_total")

        assert call_total(init) == 60
        assert call_total(Initialised(1, 1, 1)) == 3
        assert call_total(Initialised(0, 0, 0)) == 0
        print("  PASS: test_method_with_init_attrs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_init_attrs — {e}")
        failed += 1

    # ── Test 17: Diamond inheritance method resolution ──────────────────

    class DiamondA:
        def who(self):
            return "A"

    class DiamondB(DiamondA):
        def who(self):
            return "B"

    class DiamondC(DiamondA):
        def who(self):
            return "C"

    class DiamondD(DiamondB, DiamondC):
        pass  # MRO: D -> B -> C -> A

    def call_who(obj):
        return obj.who()

    try:
        d = DiamondD()
        for _ in range(WARMUP):
            call_who(d)
        check_jit_compiled(call_who, "call_who")

        assert call_who(d) == "B", f"MRO should resolve to B, got {call_who(d)}"
        assert call_who(DiamondC()) == "C"
        assert call_who(DiamondA()) == "A"
        print("  PASS: test_diamond_inheritance")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_diamond_inheritance — {e}")
        failed += 1

    # ── Test 18: Deopt — MRO change via __bases__ ───────────────────────

    class MroBase:
        def tag(self):
            return "base"

    class MroAlt:
        def tag(self):
            return "alt"

    class MroChild(MroBase):
        pass

    def call_tag(obj):
        return obj.tag()

    try:
        child = MroChild()
        for _ in range(WARMUP):
            call_tag(child)
        check_jit_compiled(call_tag, "call_tag")

        assert call_tag(child) == "base"

        try:
            MroChild.__bases__ = (MroAlt,)
            result = call_tag(child)
            assert result == "alt", f"After MRO change: expected 'alt', got {result}"
        finally:
            MroChild.__bases__ = (MroBase,)

        assert call_tag(child) == "base"
        print("  PASS: test_deopt_mro_change")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_mro_change — {e}")
        failed += 1

    # ── Test 19: Equivalence — obj.method() vs Type.method(obj) ─────────

    class Equiv:
        def __init__(self, val):
            self.val = val

        def compute(self):
            return self.val ** 2

    def call_bound(obj):
        return obj.compute()

    def call_unbound(obj):
        return Equiv.compute(obj)

    try:
        e = Equiv(6)
        for _ in range(WARMUP):
            call_bound(e)
            call_unbound(e)
        check_jit_compiled(call_bound, "call_bound")

        for v in [0, 1, 3, 6, 10, -4]:
            o = Equiv(v)
            r1 = call_bound(o)
            r2 = call_unbound(o)
            assert r1 == r2 == v ** 2, (
                f"val={v}: bound={r1}, unbound={r2}, expected={v**2}"
            )
        print("  PASS: test_equivalence_bound_vs_unbound")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_bound_vs_unbound — {e}")
        failed += 1

    # ── Test 20: Method call after __dict__ manipulation ────────────────

    class DictManip:
        def __init__(self, x):
            self.x = x

        def get_x(self):
            return self.x

    def call_get_x(obj):
        return obj.get_x()

    try:
        obj = DictManip(10)
        for _ in range(WARMUP):
            call_get_x(obj)
        check_jit_compiled(call_get_x, "call_get_x")

        assert call_get_x(obj) == 10

        # Directly manipulate __dict__
        obj.__dict__['x'] = 99
        assert call_get_x(obj) == 99, "Should see updated x via __dict__"

        # Add extra attrs via __dict__
        obj.__dict__['extra'] = "hello"
        assert call_get_x(obj) == 99, "Method should still work"
        assert obj.extra == "hello"

        # Clear dict and re-set
        obj.__dict__.clear()
        obj.x = 55
        assert call_get_x(obj) == 55
        print("  PASS: test_method_after_dict_manipulation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_after_dict_manipulation — {e}")
        failed += 1

    # ── Summary ─────────────────────────────────────────────────────────

    print()
    print(f"LOAD_ATTR_METHOD_LAZY_DICT: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
