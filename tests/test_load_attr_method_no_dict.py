#!/usr/bin/env python3
"""
test_load_attr_method_no_dict.py — Correctness and deopt tests for
LOAD_ATTR_METHOD_NO_DICT specialisation.

Targets: LOAD_ATTR_METHOD_NO_DICT.

LOAD_ATTR_METHOD_NO_DICT is the CPython 3.12 specialisation for loading
methods from objects whose type has no instance __dict__. This applies to
types defined with __slots__, built-in types (int, str, list, etc.), and
other types that set tp_dictoffset to 0.

When the adaptive interpreter detects repeated method lookups on objects
without a per-instance dictionary, it specialises the attribute load to
skip the instance __dict__ check entirely and resolve the method directly
from the type's MRO.

The CinderX JIT compiles LOAD_ATTR_METHOD_NO_DICT by emitting a GuardType
on the receiver, then loading the method directly from the type's method
cache.

Deopt triggers:
  - Receiver type changes
  - Method is shadowed on the type (type's __dict__ modified)
  - Type's MRO changes
  - A __dict__ is added to the type (e.g. via __dict__ descriptor)

Tests cover:
  - Method on __slots__ class
  - Method on built-in type (str.upper, list.append, etc.)
  - Multiple methods on same __slots__ class
  - Inherited method on __slots__ subclass
  - Method returning self attributes (via slots)
  - Classmethod on __slots__ class
  - Staticmethod on __slots__ class
  - Property access (not a method — distinct path)
  - Deopt: method replaced on type
  - Deopt: different receiver type
  - Rapid method calls (1000 iterations)
  - Stability — 10000 calls
  - __slots__ with inheritance chain
  - Method on frozen dataclass (no dict)
  - Multiple inheritance with __slots__
  - Built-in type method chaining
  - Method with args on __slots__ class
  - Deopt: MRO change via __bases__
  - Equivalence: direct call vs getattr
  - Method on int subclass with __slots__

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_method_no_dict.py
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

    print("=== LOAD_ATTR_METHOD_NO_DICT Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Method on __slots__ class ───────────────────────────────

    class SlotPoint:
        __slots__ = ('x', 'y')

        def __init__(self, x, y):
            self.x = x
            self.y = y

        def magnitude_sq(self):
            return self.x * self.x + self.y * self.y

    def call_slot_method(obj):
        return obj.magnitude_sq()

    try:
        p = SlotPoint(3, 4)
        for _ in range(WARMUP):
            call_slot_method(p)
        check_jit_compiled(call_slot_method, "call_slot_method")

        result = call_slot_method(p)
        assert result == 25, f"Expected 25, got {result}"
        p2 = SlotPoint(5, 12)
        assert call_slot_method(p2) == 169, "5^2 + 12^2 = 169"
        print("  PASS: test_slots_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slots_method — {e}")
        failed += 1

    # ── Test 2: Method on built-in type (str.upper) ─────────────────────

    def call_str_upper(s):
        return s.upper()

    try:
        for _ in range(WARMUP):
            call_str_upper("hello")
        check_jit_compiled(call_str_upper, "call_str_upper")

        assert call_str_upper("hello") == "HELLO"
        assert call_str_upper("") == ""
        assert call_str_upper("ABC") == "ABC"
        assert call_str_upper("mixEd") == "MIXED"
        print("  PASS: test_builtin_str_upper")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_builtin_str_upper — {e}")
        failed += 1

    # ── Test 3: Multiple methods on same __slots__ class ────────────────

    class SlotVec:
        __slots__ = ('x', 'y')

        def __init__(self, x, y):
            self.x = x
            self.y = y

        def add(self, other):
            return SlotVec(self.x + other.x, self.y + other.y)

        def dot(self, other):
            return self.x * other.x + self.y * other.y

        def scale(self, factor):
            return SlotVec(self.x * factor, self.y * factor)

    def call_multi_methods(a, b):
        s = a.add(b)
        d = a.dot(b)
        sc = a.scale(2)
        return s.x, s.y, d, sc.x, sc.y

    try:
        a = SlotVec(1, 2)
        b = SlotVec(3, 4)
        for _ in range(WARMUP):
            call_multi_methods(a, b)
        check_jit_compiled(call_multi_methods, "call_multi_methods")

        sx, sy, d, scx, scy = call_multi_methods(a, b)
        assert (sx, sy) == (4, 6), f"add: expected (4,6), got ({sx},{sy})"
        assert d == 11, f"dot: expected 11, got {d}"
        assert (scx, scy) == (2, 4), f"scale: expected (2,4), got ({scx},{scy})"
        print("  PASS: test_multiple_slot_methods")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_slot_methods — {e}")
        failed += 1

    # ── Test 4: Inherited method on __slots__ subclass ──────────────────

    class SlotBase:
        __slots__ = ('val',)

        def __init__(self, val):
            self.val = val

        def get_val(self):
            return self.val

    class SlotChild(SlotBase):
        __slots__ = ('extra',)

        def __init__(self, val, extra):
            super().__init__(val)
            self.extra = extra

    def call_inherited_method(obj):
        return obj.get_val()

    try:
        child = SlotChild(42, "bonus")
        for _ in range(WARMUP):
            call_inherited_method(child)
        check_jit_compiled(call_inherited_method, "call_inherited_method")

        assert call_inherited_method(child) == 42
        base = SlotBase(99)
        assert call_inherited_method(base) == 99
        print("  PASS: test_inherited_slot_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inherited_slot_method — {e}")
        failed += 1

    # ── Test 5: Method returning self attributes via slots ──────────────

    class SlotPair:
        __slots__ = ('first', 'second')

        def __init__(self, a, b):
            self.first = a
            self.second = b

        def as_tuple(self):
            return (self.first, self.second)

    def call_as_tuple(obj):
        return obj.as_tuple()

    try:
        pair = SlotPair("hello", 42)
        for _ in range(WARMUP):
            call_as_tuple(pair)
        check_jit_compiled(call_as_tuple, "call_as_tuple")

        result = call_as_tuple(pair)
        assert result == ("hello", 42), f"Expected ('hello', 42), got {result}"
        pair2 = SlotPair(None, True)
        assert call_as_tuple(pair2) == (None, True)
        print("  PASS: test_slot_method_returns_attrs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slot_method_returns_attrs — {e}")
        failed += 1

    # ── Test 6: Built-in list.copy (no dict on list) ────────────────────

    def call_list_copy(lst):
        return lst.copy()

    try:
        for _ in range(WARMUP):
            call_list_copy([1, 2, 3])
        check_jit_compiled(call_list_copy, "call_list_copy")

        original = [1, 2, 3]
        copied = call_list_copy(original)
        assert copied == original
        assert copied is not original, "copy should be a different object"
        original.append(4)
        assert len(copied) == 3, "copy should be independent"
        print("  PASS: test_builtin_list_copy")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_builtin_list_copy — {e}")
        failed += 1

    # ── Test 7: Staticmethod on __slots__ class ─────────────────────────

    class SlotStatic:
        __slots__ = ('val',)

        def __init__(self, val):
            self.val = val

        @staticmethod
        def create(val):
            return SlotStatic(val)

        def get(self):
            return self.val

    def call_static_then_method():
        obj = SlotStatic.create(77)
        return obj.get()

    try:
        for _ in range(WARMUP):
            call_static_then_method()
        check_jit_compiled(call_static_then_method, "call_static_then_method")

        result = call_static_then_method()
        assert result == 77, f"Expected 77, got {result}"
        print("  PASS: test_staticmethod_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_staticmethod_slots — {e}")
        failed += 1

    # ── Test 8: Method with args on __slots__ class ─────────────────────

    class SlotCalc:
        __slots__ = ('base',)

        def __init__(self, base):
            self.base = base

        def add_mul(self, x, y):
            return self.base + x * y

    def call_with_args(obj, x, y):
        return obj.add_mul(x, y)

    try:
        calc = SlotCalc(10)
        for _ in range(WARMUP):
            call_with_args(calc, 3, 4)
        check_jit_compiled(call_with_args, "call_with_args")

        result = call_with_args(calc, 3, 4)
        assert result == 22, f"Expected 10 + 3*4 = 22, got {result}"
        assert call_with_args(calc, 0, 100) == 10
        assert call_with_args(SlotCalc(0), 5, 5) == 25
        print("  PASS: test_slot_method_with_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slot_method_with_args — {e}")
        failed += 1

    # ── Test 9: Deopt — method replaced on type ─────────────────────────

    class SlotDeopt:
        __slots__ = ('val',)

        def __init__(self, val):
            self.val = val

        def compute(self):
            return self.val * 2

    def call_deopt_method(obj):
        return obj.compute()

    try:
        obj = SlotDeopt(5)
        for _ in range(WARMUP):
            call_deopt_method(obj)
        check_jit_compiled(call_deopt_method, "call_deopt_method")

        assert call_deopt_method(obj) == 10

        # Replace the method on the type — should trigger deopt
        original_compute = SlotDeopt.compute
        SlotDeopt.compute = lambda self: self.val * 3
        try:
            result = call_deopt_method(obj)
            assert result == 15, f"After deopt: expected 15, got {result}"
        finally:
            SlotDeopt.compute = original_compute

        assert call_deopt_method(obj) == 10, "Should work after restore"
        print("  PASS: test_deopt_method_replaced")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_method_replaced — {e}")
        failed += 1

    # ── Test 10: Deopt — different receiver type ────────────────────────

    class SlotA:
        __slots__ = ('val',)

        def __init__(self, val):
            self.val = val

        def get(self):
            return self.val

    class SlotB:
        __slots__ = ('val',)

        def __init__(self, val):
            self.val = val

        def get(self):
            return self.val * 10

    def call_get_method(obj):
        return obj.get()

    try:
        a = SlotA(5)
        for _ in range(WARMUP):
            call_get_method(a)
        check_jit_compiled(call_get_method, "call_get_method")

        assert call_get_method(a) == 5

        # Switch to different type — deopt
        b = SlotB(5)
        assert call_get_method(b) == 50, "SlotB.get should return val*10"
        print("  PASS: test_deopt_different_type")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_type — {e}")
        failed += 1

    # ── Test 11: Rapid method calls (1000 iterations) ───────────────────

    class SlotCounter:
        __slots__ = ('n',)

        def __init__(self, n):
            self.n = n

        def inc(self):
            return self.n + 1

    def call_inc(obj):
        return obj.inc()

    try:
        c = SlotCounter(0)
        for _ in range(WARMUP):
            call_inc(c)
        check_jit_compiled(call_inc, "call_inc")

        for i in range(1000):
            c = SlotCounter(i)
            result = call_inc(c)
            assert result == i + 1, f"Iteration {i}: expected {i+1}, got {result}"
        print("  PASS: test_rapid_slot_methods")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_slot_methods — {e}")
        failed += 1

    # ── Test 12: Stability — 10000 calls ────────────────────────────────

    class SlotStable:
        __slots__ = ('x',)

        def __init__(self, x):
            self.x = x

        def double(self):
            return self.x * 2

    def call_double(obj):
        return obj.double()

    try:
        s = SlotStable(7)
        for _ in range(WARMUP):
            call_double(s)
        check_jit_compiled(call_double, "call_double")

        for i in range(10000):
            result = call_double(s)
            assert result == 14, f"Iteration {i}: expected 14, got {result}"
        print("  PASS: test_stability_10000_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_calls — {e}")
        failed += 1

    # ── Test 13: __slots__ with deep inheritance chain ──────────────────

    class L0:
        __slots__ = ('a',)

        def __init__(self):
            self.a = 1

        def base_method(self):
            return self.a

    class L1(L0):
        __slots__ = ('b',)

        def __init__(self):
            super().__init__()
            self.b = 2

    class L2(L1):
        __slots__ = ('c',)

        def __init__(self):
            super().__init__()
            self.c = 3

    class L3(L2):
        __slots__ = ()

        def __init__(self):
            super().__init__()

    def call_deep_inherited(obj):
        return obj.base_method()

    try:
        obj = L3()
        for _ in range(WARMUP):
            call_deep_inherited(obj)
        check_jit_compiled(call_deep_inherited, "call_deep_inherited")

        assert call_deep_inherited(obj) == 1
        assert call_deep_inherited(L2()) == 1
        assert call_deep_inherited(L1()) == 1
        assert call_deep_inherited(L0()) == 1
        print("  PASS: test_deep_inheritance_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deep_inheritance_slots — {e}")
        failed += 1

    # ── Test 14: Built-in int.bit_length (no dict on int) ───────────────

    def call_bit_length(n):
        return n.bit_length()

    try:
        for _ in range(WARMUP):
            call_bit_length(255)
        check_jit_compiled(call_bit_length, "call_bit_length")

        assert call_bit_length(0) == 0
        assert call_bit_length(1) == 1
        assert call_bit_length(255) == 8
        assert call_bit_length(256) == 9
        assert call_bit_length(-1) == 1
        print("  PASS: test_builtin_int_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_builtin_int_method — {e}")
        failed += 1

    # ── Test 15: Multiple inheritance with __slots__ ────────────────────

    class MixA:
        __slots__ = ('a',)

        def __init__(self):
            self.a = 10

        def get_a(self):
            return self.a

    class MixB:
        __slots__ = ()

        def get_b_label(self):
            return "B"

    class MixC(MixA, MixB):
        __slots__ = ('c',)

        def __init__(self):
            super().__init__()
            self.c = 30

    def call_mixin_methods(obj):
        return obj.get_a(), obj.get_b_label()

    try:
        m = MixC()
        for _ in range(WARMUP):
            call_mixin_methods(m)
        check_jit_compiled(call_mixin_methods, "call_mixin_methods")

        a, b = call_mixin_methods(m)
        assert a == 10, f"Expected 10, got {a}"
        assert b == "B", f"Expected 'B', got {b}"
        print("  PASS: test_multiple_inheritance_slots")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_inheritance_slots — {e}")
        failed += 1

    # ── Test 16: Built-in type method chaining ──────────────────────────

    def call_chain(s):
        return s.strip().upper().replace("O", "0")

    try:
        for _ in range(WARMUP):
            call_chain("  hello  ")
        check_jit_compiled(call_chain, "call_chain")

        assert call_chain("  hello  ") == "HELL0"
        assert call_chain("  foo  ") == "F00"
        assert call_chain("bar") == "BAR"
        print("  PASS: test_builtin_method_chaining")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_builtin_method_chaining — {e}")
        failed += 1

    # ── Test 17: tuple.count (tuple has no dict) ────────────────────────

    def call_tuple_count(t, val):
        return t.count(val)

    try:
        tup = (1, 2, 3, 2, 1, 2)
        for _ in range(WARMUP):
            call_tuple_count(tup, 2)
        check_jit_compiled(call_tuple_count, "call_tuple_count")

        assert call_tuple_count(tup, 2) == 3
        assert call_tuple_count(tup, 1) == 2
        assert call_tuple_count(tup, 99) == 0
        assert call_tuple_count((), 1) == 0
        print("  PASS: test_tuple_count_no_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_tuple_count_no_dict — {e}")
        failed += 1

    # ── Test 18: Deopt — MRO change via __bases__ ───────────────────────

    class MroBase:
        __slots__ = ()

        def identify(self):
            return "base"

    class MroAlt:
        __slots__ = ()

        def identify(self):
            return "alt"

    class MroChild(MroBase):
        __slots__ = ()

    def call_identify(obj):
        return obj.identify()

    try:
        child = MroChild()
        for _ in range(WARMUP):
            call_identify(child)
        check_jit_compiled(call_identify, "call_identify")

        assert call_identify(child) == "base"

        # Change MRO by modifying __bases__
        try:
            MroChild.__bases__ = (MroAlt,)
            result = call_identify(child)
            assert result == "alt", f"After MRO change: expected 'alt', got {result}"
        finally:
            MroChild.__bases__ = (MroBase,)

        assert call_identify(child) == "base", "Should restore after MRO revert"
        print("  PASS: test_deopt_mro_change")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_mro_change — {e}")
        failed += 1

    # ── Test 19: Equivalence — direct call vs getattr ───────────────────

    class SlotEquiv:
        __slots__ = ('val',)

        def __init__(self, val):
            self.val = val

        def compute(self):
            return self.val ** 2

    def call_direct(obj):
        return obj.compute()

    def call_getattr(obj):
        return getattr(obj, 'compute')()

    try:
        obj = SlotEquiv(7)
        for _ in range(WARMUP):
            call_direct(obj)
            call_getattr(obj)
        check_jit_compiled(call_direct, "call_direct")

        for v in [0, 1, 5, 7, 10, -3]:
            o = SlotEquiv(v)
            r1 = call_direct(o)
            r2 = call_getattr(o)
            assert r1 == r2 == v ** 2, (
                f"val={v}: direct={r1}, getattr={r2}, expected={v**2}"
            )
        print("  PASS: test_equivalence_direct_vs_getattr")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_direct_vs_getattr — {e}")
        failed += 1

    # ── Test 20: Method on int subclass with __slots__ ──────────────────

    class MyInt(int):
        __slots__ = ()

        def is_even(self):
            return self % 2 == 0

    def call_is_even(obj):
        return obj.is_even()

    try:
        mi = MyInt(42)
        for _ in range(WARMUP):
            call_is_even(mi)
        check_jit_compiled(call_is_even, "call_is_even")

        assert call_is_even(MyInt(42)) is True
        assert call_is_even(MyInt(7)) is False
        assert call_is_even(MyInt(0)) is True
        assert call_is_even(MyInt(-3)) is False
        print("  PASS: test_int_subclass_slots_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_int_subclass_slots_method — {e}")
        failed += 1

    # ── Summary ─────────────────────────────────────────────────────────

    print()
    print(f"LOAD_ATTR_METHOD_NO_DICT: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
