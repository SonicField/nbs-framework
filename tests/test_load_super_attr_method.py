#!/usr/bin/env python3
"""
test_load_super_attr_method.py — Correctness and deopt tests for
LOAD_SUPER_ATTR_METHOD specialisation.

Targets: LOAD_SUPER_ATTR_METHOD.

LOAD_SUPER_ATTR_METHOD specialises the pattern super().method() when the
adaptive interpreter detects that:
1. The super() call uses the zero-argument form (implicit __class__ and self)
2. The method being accessed is a regular method on the parent class
3. The MRO (method resolution order) is stable

Instead of constructing a temporary super() proxy object, looking up
__class__ and self, then performing attribute lookup on the proxy, the
specialisation resolves the method directly from the parent class's type
at compile time and calls it as an unbound method with self.

This is the CPython 3.12 specialisation that replaced the older
LOAD_ATTR pattern for super() method calls.

Deopt triggers:
  - MRO changes (class hierarchy modified at runtime)
  - Parent class method is overridden or deleted
  - Different class at same call site
  - super() used with explicit arguments
  - Non-method attribute accessed via super()

Tests cover:
  - Simple super().method() call (single inheritance)
  - super().__init__() in subclass
  - super().method() with arguments
  - super().method() with return value
  - Diamond inheritance — MRO resolution
  - Multi-level inheritance chain (A -> B -> C)
  - super().method() calling super().method() (chained)
  - Overridden method in middle of chain
  - super().method() with *args, **kwargs
  - super().method() with default arguments
  - Deopt: parent method replaced at runtime
  - Deopt: different subclass at same call site
  - Deopt: MRO change via __bases__ modification
  - Rapid super() calls (1000 iterations)
  - Stability — 10000 super() calls
  - super().method() with property in parent
  - super().__init__() with multiple inheritance
  - Mixin pattern with super()
  - super().method() preserving self identity
  - Equivalence: super().method() vs ParentClass.method(self)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after hierarchy change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_super_attr_method.py
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

    print("=== LOAD_SUPER_ATTR_METHOD Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Simple super().method() — single inheritance ──────────

    class Base1:
        def greet(self):
            return "hello from Base1"

    class Child1(Base1):
        def greet(self):
            return super().greet()

    def call_super_1(obj):
        return obj.greet()

    c1 = Child1()
    for _ in range(WARMUP):
        call_super_1(c1)

    check_jit_compiled(call_super_1, "call_super_1")

    try:
        assert call_super_1(c1) == "hello from Base1"
        assert call_super_1(Child1()) == "hello from Base1"
        print("PASS  Test 1: simple super().method() — single inheritance")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: simple super — {e}")
        failed += 1

    # ── Test 2: super().__init__() in subclass ────────────────────────

    class Base2:
        def __init__(self, x):
            self.x = x

    class Child2(Base2):
        def __init__(self, x, y):
            super().__init__(x)
            self.y = y

    def make_child_2(x, y):
        return Child2(x, y)

    for _ in range(WARMUP):
        make_child_2(1, 2)

    check_jit_compiled(make_child_2, "make_child_2")

    try:
        obj = make_child_2(10, 20)
        assert obj.x == 10
        assert obj.y == 20
        obj2 = make_child_2(0, 0)
        assert obj2.x == 0 and obj2.y == 0
        print("PASS  Test 2: super().__init__() in subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: super __init__ — {e}")
        failed += 1

    # ── Test 3: super().method() with arguments ───────────────────────

    class Base3:
        def add(self, a, b):
            return a + b

    class Child3(Base3):
        def add(self, a, b):
            return super().add(a, b) * 2

    def call_add_3(obj, a, b):
        return obj.add(a, b)

    c3 = Child3()
    for _ in range(WARMUP):
        call_add_3(c3, 1, 2)

    check_jit_compiled(call_add_3, "call_add_3")

    try:
        assert call_add_3(c3, 1, 2) == 6
        assert call_add_3(c3, 10, 20) == 60
        assert call_add_3(c3, -5, 5) == 0
        print("PASS  Test 3: super().method() with arguments")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: super with args — {e}")
        failed += 1

    # ── Test 4: super().method() with return value ────────────────────

    class Base4:
        def compute(self):
            return [1, 2, 3]

    class Child4(Base4):
        def compute(self):
            result = super().compute()
            result.append(4)
            return result

    def call_compute_4(obj):
        return obj.compute()

    c4 = Child4()
    for _ in range(WARMUP):
        call_compute_4(c4)

    check_jit_compiled(call_compute_4, "call_compute_4")

    try:
        assert call_compute_4(c4) == [1, 2, 3, 4]
        # Each call creates a new list
        assert call_compute_4(c4) == [1, 2, 3, 4]
        print("PASS  Test 4: super().method() with return value")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: super return value — {e}")
        failed += 1

    # ── Test 5: Diamond inheritance — MRO resolution ──────────────────

    class A5:
        def who(self):
            return "A5"

    class B5(A5):
        def who(self):
            return "B5+" + super().who()

    class C5(A5):
        def who(self):
            return "C5+" + super().who()

    class D5(B5, C5):
        def who(self):
            return "D5+" + super().who()

    def call_who_5(obj):
        return obj.who()

    d5 = D5()
    for _ in range(WARMUP):
        call_who_5(d5)

    check_jit_compiled(call_who_5, "call_who_5")

    try:
        # MRO: D5 -> B5 -> C5 -> A5
        assert call_who_5(d5) == "D5+B5+C5+A5"
        assert call_who_5(B5()) == "B5+A5"
        assert call_who_5(C5()) == "C5+A5"
        print("PASS  Test 5: diamond inheritance — MRO resolution")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: diamond MRO — {e}")
        failed += 1

    # ── Test 6: Multi-level chain (A -> B -> C) ───────────────────────

    class Level0_6:
        def value(self):
            return 1

    class Level1_6(Level0_6):
        def value(self):
            return super().value() + 10

    class Level2_6(Level1_6):
        def value(self):
            return super().value() + 100

    def call_value_6(obj):
        return obj.value()

    l2 = Level2_6()
    for _ in range(WARMUP):
        call_value_6(l2)

    check_jit_compiled(call_value_6, "call_value_6")

    try:
        assert call_value_6(l2) == 111
        assert call_value_6(Level1_6()) == 11
        assert call_value_6(Level0_6()) == 1
        print("PASS  Test 6: multi-level inheritance chain")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: multi-level — {e}")
        failed += 1

    # ── Test 7: Chained super().method() calls ────────────────────────

    class Base7:
        def step(self, acc):
            return acc + ["base"]

    class Mid7(Base7):
        def step(self, acc):
            return super().step(acc + ["mid"])

    class Top7(Mid7):
        def step(self, acc):
            return super().step(acc + ["top"])

    def call_step_7(obj):
        return obj.step([])

    t7 = Top7()
    for _ in range(WARMUP):
        call_step_7(t7)

    check_jit_compiled(call_step_7, "call_step_7")

    try:
        assert call_step_7(t7) == ["top", "mid", "base"]
        assert call_step_7(Mid7()) == ["mid", "base"]
        print("PASS  Test 7: chained super().method() calls")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: chained super — {e}")
        failed += 1

    # ── Test 8: Overridden method in middle of chain ──────────────────

    class Root8:
        def action(self):
            return "root"

    class Middle8(Root8):
        def action(self):
            return "middle"  # Does NOT call super()

    class Leaf8(Middle8):
        def action(self):
            return super().action()

    def call_action_8(obj):
        return obj.action()

    leaf8 = Leaf8()
    for _ in range(WARMUP):
        call_action_8(leaf8)

    check_jit_compiled(call_action_8, "call_action_8")

    try:
        # super() from Leaf8 goes to Middle8, which doesn't call super()
        assert call_action_8(leaf8) == "middle"
        assert call_action_8(Middle8()) == "middle"
        assert call_action_8(Root8()) == "root"
        print("PASS  Test 8: overridden method in middle of chain")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: middle override — {e}")
        failed += 1

    # ── Test 9: super().method() with *args, **kwargs ─────────────────

    class Base9:
        def flexible(self, *args, **kwargs):
            return (args, kwargs)

    class Child9(Base9):
        def flexible(self, *args, **kwargs):
            return super().flexible(*args, extra="added", **kwargs)

    def call_flex_9(obj, *args, **kwargs):
        return obj.flexible(*args, **kwargs)

    c9 = Child9()
    for _ in range(WARMUP):
        call_flex_9(c9, 1, 2, key="val")

    check_jit_compiled(call_flex_9, "call_flex_9")

    try:
        result = call_flex_9(c9, 1, 2, key="val")
        assert result == ((1, 2), {"extra": "added", "key": "val"})
        result2 = call_flex_9(c9)
        assert result2 == ((), {"extra": "added"})
        print("PASS  Test 9: super().method() with *args, **kwargs")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: varargs super — {e}")
        failed += 1

    # ── Test 10: super().method() with default arguments ──────────────

    class Base10:
        def calc(self, x, y=10, z=100):
            return x + y + z

    class Child10(Base10):
        def calc(self, x, y=10, z=100):
            return super().calc(x, y, z) * 2

    def call_calc_10(obj, x, y=10, z=100):
        return obj.calc(x, y, z)

    c10 = Child10()
    for _ in range(WARMUP):
        call_calc_10(c10, 1)

    check_jit_compiled(call_calc_10, "call_calc_10")

    try:
        assert call_calc_10(c10, 1) == 222         # (1+10+100)*2
        assert call_calc_10(c10, 1, 2) == 206      # (1+2+100)*2
        assert call_calc_10(c10, 1, 2, 3) == 12    # (1+2+3)*2
        print("PASS  Test 10: super().method() with default arguments")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: defaults super — {e}")
        failed += 1

    # ── Test 11: Deopt — parent method replaced at runtime ────────────

    class Base11:
        def method(self):
            return "original"

    class Child11(Base11):
        def method(self):
            return super().method()

    def call_method_11(obj):
        return obj.method()

    c11 = Child11()
    for _ in range(WARMUP):
        call_method_11(c11)

    check_jit_compiled(call_method_11, "call_method_11")

    try:
        assert call_method_11(c11) == "original"
        # Monkey-patch the parent method
        Base11.method = lambda self: "patched"
        assert call_method_11(c11) == "patched"
        # Restore
        Base11.method = lambda self: "original"
        assert call_method_11(c11) == "original"
        print("PASS  Test 11: deopt — parent method replaced at runtime")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: deopt parent patch — {e}")
        failed += 1

    # ── Test 12: Deopt — different subclass at same call site ─────────

    class Base12:
        def identify(self):
            return "base"

    class ChildA12(Base12):
        def identify(self):
            return "A+" + super().identify()

    class ChildB12(Base12):
        def identify(self):
            return "B+" + super().identify()

    def call_id_12(obj):
        return obj.identify()

    ca12 = ChildA12()
    for _ in range(WARMUP):
        call_id_12(ca12)

    check_jit_compiled(call_id_12, "call_id_12")

    try:
        assert call_id_12(ca12) == "A+base"
        # Different subclass (deopt)
        cb12 = ChildB12()
        assert call_id_12(cb12) == "B+base"
        # Original still works
        assert call_id_12(ca12) == "A+base"
        print("PASS  Test 12: deopt — different subclass at same call site")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: deopt diff subclass — {e}")
        failed += 1

    # ── Test 13: Deopt — MRO change via __bases__ modification ────────

    class Root13:
        def val(self):
            return 1

    class Alt13:
        def val(self):
            return 999

    class Mid13(Root13):
        def val(self):
            return super().val() + 10

    class Leaf13(Mid13):
        def val(self):
            return super().val()

    def call_val_13(obj):
        return obj.val()

    leaf13 = Leaf13()
    for _ in range(WARMUP):
        call_val_13(leaf13)

    check_jit_compiled(call_val_13, "call_val_13")

    try:
        assert call_val_13(leaf13) == 11
        # Change MRO: Mid13 now inherits from Alt13 instead of Root13
        Mid13.__bases__ = (Alt13,)
        leaf13_new = Leaf13()
        assert call_val_13(leaf13_new) == 1009  # super().val() = 999 + 10
        # Restore
        Mid13.__bases__ = (Root13,)
        leaf13_restored = Leaf13()
        assert call_val_13(leaf13_restored) == 11
        print("PASS  Test 13: deopt — MRO change via __bases__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: deopt MRO change — {e}")
        failed += 1

    # ── Test 14: Rapid super() calls (1000 iterations) ────────────────

    class Base14:
        def inc(self, x):
            return x + 1

    class Child14(Base14):
        def inc(self, x):
            return super().inc(x)

    def rapid_super_14(obj, n):
        total = 0
        for i in range(n):
            total += obj.inc(i)
        return total

    c14 = Child14()
    for _ in range(WARMUP):
        rapid_super_14(c14, 1)

    check_jit_compiled(rapid_super_14, "rapid_super_14")

    try:
        result = rapid_super_14(c14, 1000)
        expected = sum(i + 1 for i in range(1000))
        assert result == expected, f"got {result}, expected {expected}"
        print("PASS  Test 14: rapid super() calls (1000 iterations)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: rapid super — {e}")
        failed += 1

    # ── Test 15: Stability — 10000 super() calls ─────────────────────

    class Base15:
        def echo(self, x):
            return x

    class Child15(Base15):
        def echo(self, x):
            return super().echo(x)

    def stability_super_15(obj, n):
        for i in range(n):
            result = obj.echo(i)
            assert result == i, f"iteration {i}: got {result}"
        return True

    c15 = Child15()
    for _ in range(WARMUP):
        c15.echo(0)

    try:
        assert stability_super_15(c15, 10000)
        print("PASS  Test 15: stability — 10000 super() calls")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: stability — {e}")
        failed += 1

    # ── Test 16: super().method() accessing property in parent ────────

    class Base16:
        @property
        def info(self):
            return "base_info"

        def get_info(self):
            return self.info

    class Child16(Base16):
        @property
        def info(self):
            return "child_info"

        def get_parent_info(self):
            return super().get_info()

    def call_parent_info_16(obj):
        return obj.get_parent_info()

    c16 = Child16()
    for _ in range(WARMUP):
        call_parent_info_16(c16)

    check_jit_compiled(call_parent_info_16, "call_parent_info_16")

    try:
        # super().get_info() calls self.info on Base16, but self is Child16
        # so self.info resolves to Child16.info (property on child)
        assert call_parent_info_16(c16) == "child_info"
        assert Base16().get_info() == "base_info"
        print("PASS  Test 16: super().method() with property in parent")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: property super — {e}")
        failed += 1

    # ── Test 17: super().__init__() with multiple inheritance ─────────

    class MixA17:
        def __init__(self):
            super().__init__()
            self.a = "from_A"

    class MixB17:
        def __init__(self):
            super().__init__()
            self.b = "from_B"

    class Combined17(MixA17, MixB17):
        def __init__(self):
            super().__init__()
            self.c = "from_C"

    def make_combined_17():
        return Combined17()

    for _ in range(WARMUP):
        make_combined_17()

    check_jit_compiled(make_combined_17, "make_combined_17")

    try:
        obj = make_combined_17()
        # MRO: Combined17 -> MixA17 -> MixB17 -> object
        assert obj.a == "from_A"
        assert obj.b == "from_B"
        assert obj.c == "from_C"
        print("PASS  Test 17: super().__init__() with multiple inheritance")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: multi-init super — {e}")
        failed += 1

    # ── Test 18: Mixin pattern with super() ───────────────────────────

    class LogMixin18:
        def __init__(self):
            super().__init__()
            self.log = []

        def do_action(self):
            self.log.append("action")
            return super().do_action()

    class Worker18:
        def do_action(self):
            return "done"

    class LoggedWorker18(LogMixin18, Worker18):
        pass

    def call_action_18(obj):
        return obj.do_action()

    lw18 = LoggedWorker18()
    for _ in range(WARMUP):
        lw18.log.clear()
        call_action_18(lw18)

    check_jit_compiled(call_action_18, "call_action_18")

    try:
        lw18.log.clear()
        result = call_action_18(lw18)
        assert result == "done"
        assert lw18.log == ["action"]
        # Second call
        result2 = call_action_18(lw18)
        assert result2 == "done"
        assert lw18.log == ["action", "action"]
        print("PASS  Test 18: mixin pattern with super()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: mixin super — {e}")
        failed += 1

    # ── Test 19: super().method() preserving self identity ────────────

    class Base19:
        def get_self(self):
            return self

    class Child19(Base19):
        def get_self(self):
            return super().get_self()

    def call_get_self_19(obj):
        return obj.get_self()

    c19 = Child19()
    for _ in range(WARMUP):
        call_get_self_19(c19)

    check_jit_compiled(call_get_self_19, "call_get_self_19")

    try:
        result = call_get_self_19(c19)
        assert result is c19, "super().method() must preserve self identity"
        c19b = Child19()
        assert call_get_self_19(c19b) is c19b
        assert call_get_self_19(c19) is not c19b
        print("PASS  Test 19: super().method() preserving self identity")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: self identity — {e}")
        failed += 1

    # ── Test 20: Equivalence — super().method() vs Parent.method(self) ─

    class Base20:
        def compute(self, x, y):
            return x * y + id(self) % 100

    class Child20(Base20):
        def via_super(self, x, y):
            return super().compute(x, y)

        def via_explicit(self, x, y):
            return Base20.compute(self, x, y)

    def equiv_super_20(obj, x, y):
        return obj.via_super(x, y)

    def equiv_explicit_20(obj, x, y):
        return obj.via_explicit(x, y)

    c20 = Child20()
    for _ in range(WARMUP):
        equiv_super_20(c20, 3, 4)
        equiv_explicit_20(c20, 3, 4)

    check_jit_compiled(equiv_super_20, "equiv_super_20")
    check_jit_compiled(equiv_explicit_20, "equiv_explicit_20")

    try:
        for x, y in [(1, 1), (3, 4), (10, 20), (0, 0), (-1, 5)]:
            s = equiv_super_20(c20, x, y)
            e = equiv_explicit_20(c20, x, y)
            assert s == e, (
                f"mismatch for ({x}, {y}): super={s}, explicit={e}"
            )
        print("PASS  Test 20: equivalence — super() vs Parent.method(self)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: equivalence — {e}")
        failed += 1

    # ── Summary ───────────────────────────────────────────────────────

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
