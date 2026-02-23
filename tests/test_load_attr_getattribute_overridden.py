#!/usr/bin/env python3
"""
test_load_attr_getattribute_overridden.py — Correctness and deopt tests for
LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN specialisation.

Targets: LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN.

LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN specialises attribute load operations
(obj.attr) when the type has a custom __getattribute__ method that overrides
object.__getattribute__. Instead of going through the generic LOAD_ATTR
path, the specialisation calls the custom __getattribute__ directly.

__getattribute__ intercepts ALL attribute access (unlike __getattr__ which
only intercepts missing attributes). This is a relatively rare pattern but
used in ORMs, proxies, and instrumentation frameworks.

The adaptive specialiser emits LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN after
observing repeated attribute access on instances whose type defines a
non-default __getattribute__.

Deopt triggers:
  - Object type changes (different class)
  - __getattribute__ is removed or modified on the class
  - Object switches to a type without __getattribute__ override

Tests cover:
  - Basic __getattribute__ override (passthrough)
  - __getattribute__ transforming attribute values
  - __getattribute__ with access logging
  - __getattribute__ raising AttributeError
  - __getattribute__ calling object.__getattribute__
  - __getattribute__ with conditional logic
  - Deopt: switch to class without __getattribute__
  - Deopt: switch to different __getattribute__ class
  - Deopt: switch to class with __getattr__ instead
  - __getattribute__ in loop
  - __getattribute__ with side effects (counting)
  - __getattribute__ on subclass (inherited)
  - __getattribute__ on subclass (overridden)
  - __getattribute__ returning different types
  - Multiple attribute loads in one function
  - __getattribute__ returning None
  - __getattribute__ with exception handling
  - Proxy pattern via __getattribute__
  - Monkey-patch __getattribute__ at runtime
  - Rapid type alternation

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (AttributeError, custom exceptions)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_getattribute_overridden.py
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
    print("=== LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN Correctness & Deopt Tests ===")
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

    class Passthrough:
        """__getattribute__ that delegates to object.__getattribute__."""
        def __init__(self):
            object.__setattr__(self, 'x', 10)
            object.__setattr__(self, 'y', 20)

        def __getattribute__(self, name):
            return object.__getattribute__(self, name)

    class Prefixed:
        """__getattribute__ that prefixes string values."""
        def __init__(self):
            object.__setattr__(self, 'name', 'alice')
            object.__setattr__(self, 'role', 'admin')
            object.__setattr__(self, 'count', 42)

        def __getattribute__(self, name):
            val = object.__getattribute__(self, name)
            if isinstance(val, str):
                return f"[{name}]:{val}"
            return val

    class Plain:
        """Normal class without __getattribute__."""
        def __init__(self, x, y):
            self.x = x
            self.y = y

    # ── Test 1: Basic __getattribute__ passthrough ─────────────────────

    def load_pass_1(obj):
        return obj.x

    pt = Passthrough()

    for _ in range(WARMUP):
        load_pass_1(pt)

    check_jit_compiled(load_pass_1, "load_pass_1")

    try:
        assert load_pass_1(pt) == 10
        object.__setattr__(pt, 'x', 99)
        assert load_pass_1(pt) == 99
        object.__setattr__(pt, 'x', -1)
        assert load_pass_1(pt) == -1
        print("PASS  Test 1: basic __getattribute__ passthrough")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: passthrough — {e}")
        failed += 1

    # ── Test 2: __getattribute__ transforming values ───────────────────

    def load_prefixed_2(obj):
        return obj.name

    pf = Prefixed()

    for _ in range(WARMUP):
        load_prefixed_2(pf)

    check_jit_compiled(load_prefixed_2, "load_prefixed_2")

    try:
        assert load_prefixed_2(pf) == "[name]:alice"
        object.__setattr__(pf, 'name', 'bob')
        assert load_prefixed_2(pf) == "[name]:bob"
        print("PASS  Test 2: __getattribute__ transforming values")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: transform — {e}")
        failed += 1

    # ── Test 3: __getattribute__ with access logging ───────────────────

    class Logged:
        def __init__(self):
            object.__setattr__(self, 'x', 42)
            object.__setattr__(self, '_log', [])

        def __getattribute__(self, name):
            if name != '_log':
                log = object.__getattribute__(self, '_log')
                log.append(name)
            return object.__getattribute__(self, name)

    def load_logged_3(obj):
        return obj.x

    lg = Logged()

    for _ in range(WARMUP):
        load_logged_3(lg)

    check_jit_compiled(load_logged_3, "load_logged_3")

    try:
        lg._log.clear()
        assert load_logged_3(lg) == 42
        assert lg._log == ['x']
        load_logged_3(lg)
        load_logged_3(lg)
        assert lg._log == ['x', 'x', 'x']
        print("PASS  Test 3: __getattribute__ with access logging")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: logging — {e}")
        failed += 1

    # ── Test 4: __getattribute__ raising AttributeError ────────────────

    class Restricted:
        def __init__(self):
            object.__setattr__(self, 'public', 42)
            object.__setattr__(self, '_private', 99)

        def __getattribute__(self, name):
            if name.startswith('_'):
                raise AttributeError(f"access to '{name}' denied")
            return object.__getattribute__(self, name)

    def load_restricted_4(obj):
        return obj.public

    rst = Restricted()

    for _ in range(WARMUP):
        load_restricted_4(rst)

    check_jit_compiled(load_restricted_4, "load_restricted_4")

    try:
        assert load_restricted_4(rst) == 42

        # Private access should fail
        def load_private(obj):
            return obj._private

        try:
            load_private(rst)
            assert False, "expected AttributeError"
        except AttributeError as e:
            assert "denied" in str(e)

        # Public still works after error
        assert load_restricted_4(rst) == 42

        print("PASS  Test 4: __getattribute__ raising AttributeError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: AttributeError — {e}")
        failed += 1

    # ── Test 5: __getattribute__ calling super().__getattribute__ ──────

    class SuperCaller:
        def __init__(self):
            object.__setattr__(self, 'x', 10)

        def __getattribute__(self, name):
            val = super().__getattribute__(name)
            return val

    def load_super_5(obj):
        return obj.x

    sc = SuperCaller()

    for _ in range(WARMUP):
        load_super_5(sc)

    check_jit_compiled(load_super_5, "load_super_5")

    try:
        assert load_super_5(sc) == 10
        object.__setattr__(sc, 'x', 99)
        assert load_super_5(sc) == 99
        print("PASS  Test 5: __getattribute__ via super()")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: super — {e}")
        failed += 1

    # ── Test 6: __getattribute__ with conditional logic ────────────────

    class Conditional:
        def __init__(self):
            object.__setattr__(self, 'x', 10)
            object.__setattr__(self, 'y', 20)
            object.__setattr__(self, 'z', 30)

        def __getattribute__(self, name):
            val = object.__getattribute__(self, name)
            if name == 'x':
                return val * 2
            elif name == 'y':
                return val + 100
            return val

    def load_cond_6(obj):
        return obj.x

    def load_cond_y_6(obj):
        return obj.y

    def load_cond_z_6(obj):
        return obj.z

    cn = Conditional()

    for _ in range(WARMUP):
        load_cond_6(cn)

    check_jit_compiled(load_cond_6, "load_cond_6")

    try:
        assert load_cond_6(cn) == 20  # 10 * 2
        assert load_cond_y_6(cn) == 120  # 20 + 100
        assert load_cond_z_6(cn) == 30  # unchanged
        print("PASS  Test 6: __getattribute__ with conditional logic")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: conditional — {e}")
        failed += 1

    # ── Test 7: Deopt — switch to class without __getattribute__ ───────

    def load_deopt_7(obj):
        return obj.x

    for _ in range(WARMUP):
        load_deopt_7(pt)

    check_jit_compiled(load_deopt_7, "load_deopt_7")

    try:
        object.__setattr__(pt, 'x', 10)
        assert load_deopt_7(pt) == 10

        # Plain class (deopt — no __getattribute__)
        p = Plain(42, 0)
        assert load_deopt_7(p) == 42

        # Passthrough still works
        assert load_deopt_7(pt) == 10

        print("PASS  Test 7: deopt — class without __getattribute__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: deopt plain — {e}")
        failed += 1

    # ── Test 8: Deopt — different __getattribute__ class ───────────────

    class Doubled:
        def __init__(self):
            object.__setattr__(self, 'x', 5)

        def __getattribute__(self, name):
            val = object.__getattribute__(self, name)
            if isinstance(val, int):
                return val * 2
            return val

    class Tripled:
        def __init__(self):
            object.__setattr__(self, 'x', 5)

        def __getattribute__(self, name):
            val = object.__getattribute__(self, name)
            if isinstance(val, int):
                return val * 3
            return val

    def load_deopt_8(obj):
        return obj.x

    db = Doubled()

    for _ in range(WARMUP):
        load_deopt_8(db)

    check_jit_compiled(load_deopt_8, "load_deopt_8")

    try:
        assert load_deopt_8(db) == 10  # 5 * 2

        tr = Tripled()
        assert load_deopt_8(tr) == 15  # 5 * 3

        assert load_deopt_8(db) == 10

        print("PASS  Test 8: deopt — different __getattribute__ class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt diff — {e}")
        failed += 1

    # ── Test 9: Deopt — class with __getattr__ instead ─────────────────

    class WithGetattr:
        def __init__(self):
            self.x = 42

        def __getattr__(self, name):
            return f"missing:{name}"

    def load_deopt_9(obj):
        return obj.x

    for _ in range(WARMUP):
        load_deopt_9(pt)

    check_jit_compiled(load_deopt_9, "load_deopt_9")

    try:
        object.__setattr__(pt, 'x', 10)
        assert load_deopt_9(pt) == 10

        wg = WithGetattr()
        assert load_deopt_9(wg) == 42  # x exists, __getattr__ not called

        assert load_deopt_9(pt) == 10

        print("PASS  Test 9: deopt — __getattr__ instead of __getattribute__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt __getattr__ — {e}")
        failed += 1

    # ── Test 10: __getattribute__ in loop ──────────────────────────────

    def loop_load_10(obj, n):
        total = 0
        for _ in range(n):
            total += obj.x
        return total

    for _ in range(WARMUP):
        loop_load_10(pt, 5)

    check_jit_compiled(loop_load_10, "loop_load_10")

    try:
        object.__setattr__(pt, 'x', 3)
        assert loop_load_10(pt, 100) == 300
        object.__setattr__(pt, 'x', 7)
        assert loop_load_10(pt, 10) == 70
        assert loop_load_10(pt, 0) == 0
        print("PASS  Test 10: __getattribute__ in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: loop — {e}")
        failed += 1

    # ── Test 11: __getattribute__ with side effects (counting) ─────────

    class AccessCounter:
        def __init__(self):
            object.__setattr__(self, 'x', 42)
            object.__setattr__(self, '_access_count', 0)

        def __getattribute__(self, name):
            if name == '_access_count':
                return object.__getattribute__(self, '_access_count')
            object.__setattr__(
                self, '_access_count',
                object.__getattribute__(self, '_access_count') + 1
            )
            return object.__getattribute__(self, name)

    def load_counted_11(obj):
        return obj.x

    ac = AccessCounter()

    for _ in range(WARMUP):
        load_counted_11(ac)

    check_jit_compiled(load_counted_11, "load_counted_11")

    try:
        ac._access_count = 0
        assert load_counted_11(ac) == 42
        assert ac._access_count == 1
        load_counted_11(ac)
        load_counted_11(ac)
        assert ac._access_count == 3
        print("PASS  Test 11: __getattribute__ with access counting")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: counting — {e}")
        failed += 1

    # ── Test 12: __getattribute__ on subclass (inherited) ──────────────

    class InheritedGA(Passthrough):
        def __init__(self):
            super().__init__()
            object.__setattr__(self, 'z', 30)

    def load_inherited_12(obj):
        return obj.x

    iga = InheritedGA()

    for _ in range(WARMUP):
        load_inherited_12(iga)

    check_jit_compiled(load_inherited_12, "load_inherited_12")

    try:
        assert load_inherited_12(iga) == 10
        object.__setattr__(iga, 'x', 99)
        assert load_inherited_12(iga) == 99

        # Base also works
        assert load_inherited_12(pt) == 7  # from test 10

        print("PASS  Test 12: inherited __getattribute__ from subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: inherited — {e}")
        failed += 1

    # ── Test 13: __getattribute__ on subclass (overridden) ─────────────

    class OverriddenGA(Passthrough):
        def __getattribute__(self, name):
            val = object.__getattribute__(self, name)
            if isinstance(val, int):
                return val + 1000
            return val

    def load_overridden_13(obj):
        return obj.x

    oga = OverriddenGA()

    for _ in range(WARMUP):
        load_overridden_13(pt)

    check_jit_compiled(load_overridden_13, "load_overridden_13")

    try:
        # Passthrough: returns raw value
        object.__setattr__(pt, 'x', 5)
        assert load_overridden_13(pt) == 5

        # Overridden: adds 1000
        assert load_overridden_13(oga) == 1010  # 10 + 1000

        # Passthrough still works
        assert load_overridden_13(pt) == 5

        print("PASS  Test 13: overridden __getattribute__ on subclass")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: overridden — {e}")
        failed += 1

    # ── Test 14: __getattribute__ returning different types ────────────

    class TypeSwitch:
        def __init__(self):
            object.__setattr__(self, '_mode', 'int')

        def __getattribute__(self, name):
            if name == '_mode':
                return object.__getattribute__(self, '_mode')
            mode = object.__getattribute__(self, '_mode')
            if mode == 'int':
                return 42
            elif mode == 'str':
                return "hello"
            elif mode == 'list':
                return [1, 2, 3]
            elif mode == 'none':
                return None
            return object.__getattribute__(self, name)

    def load_typeswitch_14(obj):
        return obj.x

    ts = TypeSwitch()

    for _ in range(WARMUP):
        load_typeswitch_14(ts)

    check_jit_compiled(load_typeswitch_14, "load_typeswitch_14")

    try:
        assert load_typeswitch_14(ts) == 42
        object.__setattr__(ts, '_mode', 'str')
        assert load_typeswitch_14(ts) == "hello"
        object.__setattr__(ts, '_mode', 'list')
        assert load_typeswitch_14(ts) == [1, 2, 3]
        object.__setattr__(ts, '_mode', 'none')
        assert load_typeswitch_14(ts) is None
        print("PASS  Test 14: __getattribute__ returning different types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: type switch — {e}")
        failed += 1

    # ── Test 15: Multiple attribute loads in one function ──────────────

    def load_multi_15(obj):
        return obj.x, obj.y

    for _ in range(WARMUP):
        load_multi_15(pt)

    check_jit_compiled(load_multi_15, "load_multi_15")

    try:
        object.__setattr__(pt, 'x', 10)
        object.__setattr__(pt, 'y', 20)
        assert load_multi_15(pt) == (10, 20)
        object.__setattr__(pt, 'x', 100)
        object.__setattr__(pt, 'y', 200)
        assert load_multi_15(pt) == (100, 200)
        print("PASS  Test 15: multiple attribute loads in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: multi load — {e}")
        failed += 1

    # ── Test 16: __getattribute__ returning None ───────────────────────

    class AlwaysNone:
        def __getattribute__(self, name):
            return None

    def load_none_16(obj):
        return obj.x

    an = AlwaysNone()

    for _ in range(WARMUP):
        load_none_16(pt)

    check_jit_compiled(load_none_16, "load_none_16")

    try:
        object.__setattr__(pt, 'x', 10)
        assert load_none_16(pt) == 10

        # AlwaysNone returns None for everything
        assert load_none_16(an) is None

        assert load_none_16(pt) == 10

        print("PASS  Test 16: __getattribute__ returning None")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: None — {e}")
        failed += 1

    # ── Test 17: __getattribute__ with exception handling ──────────────

    class SafeAccess:
        def __init__(self):
            object.__setattr__(self, '_data', {'x': 10, 'y': 20})

        def __getattribute__(self, name):
            if name.startswith('_'):
                return object.__getattribute__(self, name)
            data = object.__getattribute__(self, '_data')
            if name not in data:
                raise AttributeError(f"no attribute '{name}'")
            return data[name]

    def load_safe_17(obj):
        return obj.x

    sa = SafeAccess()

    for _ in range(WARMUP):
        load_safe_17(sa)

    check_jit_compiled(load_safe_17, "load_safe_17")

    try:
        assert load_safe_17(sa) == 10

        def load_missing(obj):
            return obj.z

        try:
            load_missing(sa)
            assert False, "expected AttributeError"
        except AttributeError as e:
            assert "no attribute" in str(e)

        # Still works after error
        assert load_safe_17(sa) == 10

        print("PASS  Test 17: __getattribute__ with exception handling")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: exception — {e}")
        failed += 1

    # ── Test 18: Proxy pattern via __getattribute__ ────────────────────

    class Proxy:
        def __init__(self, target):
            object.__setattr__(self, '_target', target)

        def __getattribute__(self, name):
            if name == '_target':
                return object.__getattribute__(self, '_target')
            target = object.__getattribute__(self, '_target')
            return getattr(target, name)

    def load_proxy_18(obj):
        return obj.x

    p_target = Plain(42, 99)
    proxy = Proxy(p_target)

    for _ in range(WARMUP):
        load_proxy_18(proxy)

    check_jit_compiled(load_proxy_18, "load_proxy_18")

    try:
        assert load_proxy_18(proxy) == 42
        p_target.x = 100
        assert load_proxy_18(proxy) == 100

        # Change target
        p_target2 = Plain(7, 8)
        object.__setattr__(proxy, '_target', p_target2)
        assert load_proxy_18(proxy) == 7

        print("PASS  Test 18: proxy pattern via __getattribute__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: proxy — {e}")
        failed += 1

    # ── Test 19: Monkey-patch __getattribute__ at runtime ──────────────

    class Patchable:
        def __init__(self):
            object.__setattr__(self, 'x', 42)

        def __getattribute__(self, name):
            return object.__getattribute__(self, name)

    def load_patch_19(obj):
        return obj.x

    pat = Patchable()

    for _ in range(WARMUP):
        load_patch_19(pat)

    check_jit_compiled(load_patch_19, "load_patch_19")

    try:
        assert load_patch_19(pat) == 42

        # Monkey-patch to double values
        def doubling_getattribute(self, name):
            val = object.__getattribute__(self, name)
            if isinstance(val, int):
                return val * 2
            return val

        Patchable.__getattribute__ = doubling_getattribute
        assert load_patch_19(pat) == 84

        # Restore
        Patchable.__getattribute__ = lambda self, name: object.__getattribute__(self, name)
        assert load_patch_19(pat) == 42

        print("PASS  Test 19: monkey-patch __getattribute__ at runtime")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: monkey-patch — {e}")
        failed += 1

    # ── Test 20: Rapid type alternation ────────────────────────────────

    def load_rapid_20(obj):
        return obj.x

    for _ in range(WARMUP):
        load_rapid_20(pt)

    check_jit_compiled(load_rapid_20, "load_rapid_20")

    try:
        object.__setattr__(pt, 'x', 10)
        p_plain = Plain(99, 0)

        for i in range(50):
            if i % 3 == 0:
                assert load_rapid_20(pt) == 10  # Passthrough
            elif i % 3 == 1:
                assert load_rapid_20(db) == 10  # Doubled: 5*2
            else:
                assert load_rapid_20(p_plain) == 99  # Plain

        # Final check
        assert load_rapid_20(pt) == 10

        print("PASS  Test 20: rapid type alternation (50 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: type alternation — {e}")
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
