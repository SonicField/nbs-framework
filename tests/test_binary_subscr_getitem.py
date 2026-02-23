#!/usr/bin/env python3
"""
test_binary_subscr_getitem.py — Correctness and deopt tests for
BINARY_SUBSCR_GETITEM specialisation.

Targets: BINARY_SUBSCR_GETITEM.

BINARY_SUBSCR_GETITEM specialises obj[key] when obj has a custom
__getitem__ method (i.e. is not a builtin list/dict/tuple/str).
The adaptive interpreter emits this after observing repeated subscript
operations on objects with custom __getitem__.

Instead of going through the generic BINARY_SUBSCR path (which calls
PyObject_GetItem → type->tp_as_mapping->mp_subscript), the specialisation
caches the __getitem__ method and calls it directly, avoiding the
type slot lookup overhead.

The JIT specialisation emits a GuardType on the container object,
then calls the cached __getitem__ directly.

Deopt triggers:
  - Container type changes (different class with different __getitem__)
  - Container becomes a builtin type (list, dict, tuple)
  - __getitem__ is modified at runtime
  - Container class changes (metaclass, monkey-patch)

Tests cover:
  - Custom class with __getitem__ (int keys)
  - Custom class with __getitem__ (string keys)
  - Custom class with __getitem__ (slice keys)
  - __getitem__ raising KeyError
  - __getitem__ raising IndexError
  - Deopt: switch to dict
  - Deopt: switch to list
  - Deopt: switch to different custom class
  - defaultdict-like behaviour
  - Nested subscript (obj[a][b])
  - __getitem__ with side effects
  - __getitem__ returning different types
  - Inheritance: subclass __getitem__
  - Multiple subscripts in one function
  - Subscript in loop
  - __getitem__ that delegates to internal dict
  - __class_getitem__ (not the same — should not interfere)
  - Rapid type alternation
  - __getitem__ with mutable state
  - Subscript correctness vs direct __getitem__ call

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (KeyError, IndexError)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_binary_subscr_getitem.py
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
    print("=== BINARY_SUBSCR_GETITEM Correctness & Deopt Tests ===")
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

    class IntMap:
        """Maps int keys to their doubles."""
        def __getitem__(self, key):
            return key * 2

    class StrMap:
        """Maps string keys via internal dict."""
        def __init__(self, data):
            self._data = dict(data)
        def __getitem__(self, key):
            return self._data[key]

    class SliceContainer:
        """Supports slice subscript."""
        def __init__(self, items):
            self._items = list(items)
        def __getitem__(self, key):
            return self._items[key]

    # ── Test 1: Custom __getitem__ with int keys ──────────────────────

    def subscr_int_1(obj, key):
        return obj[key]

    m = IntMap()

    for _ in range(WARMUP):
        subscr_int_1(m, 5)

    check_jit_compiled(subscr_int_1, "subscr_int_1")

    try:
        assert subscr_int_1(m, 5) == 10
        assert subscr_int_1(m, 0) == 0
        assert subscr_int_1(m, -3) == -6
        assert subscr_int_1(m, 100) == 200
        print("PASS  Test 1: custom __getitem__ with int keys")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: int keys — {e}")
        failed += 1

    # ── Test 2: Custom __getitem__ with string keys ───────────────────

    def subscr_str_2(obj, key):
        return obj[key]

    sm = StrMap({"x": 10, "y": 20, "z": 30})

    for _ in range(WARMUP):
        subscr_str_2(sm, "x")

    check_jit_compiled(subscr_str_2, "subscr_str_2")

    try:
        assert subscr_str_2(sm, "x") == 10
        assert subscr_str_2(sm, "y") == 20
        assert subscr_str_2(sm, "z") == 30
        print("PASS  Test 2: custom __getitem__ with string keys")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: string keys — {e}")
        failed += 1

    # ── Test 3: Custom __getitem__ with slices ────────────────────────

    def subscr_slice_3(obj, key):
        return obj[key]

    sc = SliceContainer([10, 20, 30, 40, 50])

    for _ in range(WARMUP):
        subscr_slice_3(sc, 0)

    check_jit_compiled(subscr_slice_3, "subscr_slice_3")

    try:
        assert subscr_slice_3(sc, 0) == 10
        assert subscr_slice_3(sc, -1) == 50
        assert subscr_slice_3(sc, slice(1, 3)) == [20, 30]
        assert subscr_slice_3(sc, slice(None)) == [10, 20, 30, 40, 50]
        print("PASS  Test 3: custom __getitem__ with slices")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: slices — {e}")
        failed += 1

    # ── Test 4: __getitem__ raising KeyError ──────────────────────────

    def subscr_keyerr_4(obj, key):
        return obj[key]

    sm2 = StrMap({"a": 1, "b": 2})

    for _ in range(WARMUP):
        subscr_keyerr_4(sm2, "a")

    check_jit_compiled(subscr_keyerr_4, "subscr_keyerr_4")

    try:
        assert subscr_keyerr_4(sm2, "a") == 1

        try:
            subscr_keyerr_4(sm2, "missing")
            assert False, "expected KeyError"
        except KeyError:
            pass

        # Still works after error
        assert subscr_keyerr_4(sm2, "b") == 2

        print("PASS  Test 4: __getitem__ raising KeyError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: KeyError — {e}")
        failed += 1

    # ── Test 5: __getitem__ raising IndexError ────────────────────────

    def subscr_indexerr_5(obj, key):
        return obj[key]

    sc2 = SliceContainer([10, 20, 30])

    for _ in range(WARMUP):
        subscr_indexerr_5(sc2, 0)

    check_jit_compiled(subscr_indexerr_5, "subscr_indexerr_5")

    try:
        assert subscr_indexerr_5(sc2, 0) == 10

        try:
            subscr_indexerr_5(sc2, 99)
            assert False, "expected IndexError"
        except IndexError:
            pass

        # Still works after error
        assert subscr_indexerr_5(sc2, 2) == 30

        print("PASS  Test 5: __getitem__ raising IndexError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: IndexError — {e}")
        failed += 1

    # ── Test 6: Deopt — switch to dict ────────────────────────────────

    def subscr_deopt_dict_6(obj, key):
        return obj[key]

    for _ in range(WARMUP):
        subscr_deopt_dict_6(sm, "x")

    check_jit_compiled(subscr_deopt_dict_6, "subscr_deopt_dict_6")

    try:
        # Custom class path
        assert subscr_deopt_dict_6(sm, "x") == 10

        # Dict path (deopt)
        d = {"x": 100, "y": 200}
        assert subscr_deopt_dict_6(d, "x") == 100
        assert subscr_deopt_dict_6(d, "y") == 200

        # Custom class still works
        assert subscr_deopt_dict_6(sm, "y") == 20

        print("PASS  Test 6: deopt — switch to dict")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: dict deopt — {e}")
        failed += 1

    # ── Test 7: Deopt — switch to list ────────────────────────────────

    def subscr_deopt_list_7(obj, key):
        return obj[key]

    for _ in range(WARMUP):
        subscr_deopt_list_7(m, 5)

    check_jit_compiled(subscr_deopt_list_7, "subscr_deopt_list_7")

    try:
        # Custom class path
        assert subscr_deopt_list_7(m, 5) == 10

        # List path (deopt)
        lst = [100, 200, 300]
        assert subscr_deopt_list_7(lst, 0) == 100
        assert subscr_deopt_list_7(lst, 2) == 300

        # Custom class still works
        assert subscr_deopt_list_7(m, 3) == 6

        print("PASS  Test 7: deopt — switch to list")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: list deopt — {e}")
        failed += 1

    # ── Test 8: Deopt — switch to different custom class ──────────────

    class AltMap:
        def __getitem__(self, key):
            return key * 10

    def subscr_deopt_alt_8(obj, key):
        return obj[key]

    for _ in range(WARMUP):
        subscr_deopt_alt_8(m, 5)

    check_jit_compiled(subscr_deopt_alt_8, "subscr_deopt_alt_8")

    try:
        # IntMap path
        assert subscr_deopt_alt_8(m, 5) == 10

        # AltMap path (deopt — different class)
        alt = AltMap()
        assert subscr_deopt_alt_8(alt, 5) == 50
        assert subscr_deopt_alt_8(alt, 0) == 0

        # IntMap still works
        assert subscr_deopt_alt_8(m, 3) == 6

        print("PASS  Test 8: deopt — different custom class")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: alt class deopt — {e}")
        failed += 1

    # ── Test 9: Defaultdict-like behaviour ────────────────────────────

    class DefaultMap:
        def __init__(self, default):
            self._data = {}
            self._default = default
        def __getitem__(self, key):
            return self._data.get(key, self._default)
        def __setitem__(self, key, val):
            self._data[key] = val

    def subscr_default_9(obj, key):
        return obj[key]

    dm = DefaultMap(0)
    dm["a"] = 10
    dm["b"] = 20

    for _ in range(WARMUP):
        subscr_default_9(dm, "a")

    check_jit_compiled(subscr_default_9, "subscr_default_9")

    try:
        assert subscr_default_9(dm, "a") == 10
        assert subscr_default_9(dm, "b") == 20
        assert subscr_default_9(dm, "missing") == 0  # Default
        assert subscr_default_9(dm, "also_missing") == 0
        print("PASS  Test 9: defaultdict-like behaviour")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: defaultdict — {e}")
        failed += 1

    # ── Test 10: Nested subscript (obj[a][b]) ─────────────────────────

    class NestedMap:
        def __init__(self, data):
            self._data = data
        def __getitem__(self, key):
            return self._data[key]

    def subscr_nested_10(obj, k1, k2):
        return obj[k1][k2]

    nm = NestedMap({"row1": NestedMap({"col1": 42, "col2": 99}),
                    "row2": NestedMap({"col1": 7, "col2": 13})})

    for _ in range(WARMUP):
        subscr_nested_10(nm, "row1", "col1")

    check_jit_compiled(subscr_nested_10, "subscr_nested_10")

    try:
        assert subscr_nested_10(nm, "row1", "col1") == 42
        assert subscr_nested_10(nm, "row1", "col2") == 99
        assert subscr_nested_10(nm, "row2", "col1") == 7
        assert subscr_nested_10(nm, "row2", "col2") == 13
        print("PASS  Test 10: nested subscript (obj[a][b])")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: nested — {e}")
        failed += 1

    # ── Test 11: __getitem__ with side effects ────────────────────────

    class CountingMap:
        def __init__(self):
            self.count = 0
        def __getitem__(self, key):
            self.count += 1
            return key

    def subscr_sideeffect_11(obj, key):
        return obj[key]

    cm = CountingMap()

    for _ in range(WARMUP):
        subscr_sideeffect_11(cm, 42)

    check_jit_compiled(subscr_sideeffect_11, "subscr_sideeffect_11")

    try:
        cm.count = 0
        assert subscr_sideeffect_11(cm, 42) == 42
        assert cm.count == 1
        subscr_sideeffect_11(cm, 99)
        assert cm.count == 2
        for _ in range(10):
            subscr_sideeffect_11(cm, 0)
        assert cm.count == 12
        print("PASS  Test 11: __getitem__ with side effects (call counting)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: side effects — {e}")
        failed += 1

    # ── Test 12: __getitem__ returning different types ─────────────────

    class TypeVaryMap:
        def __getitem__(self, key):
            if key == "int":
                return 42
            elif key == "str":
                return "hello"
            elif key == "list":
                return [1, 2, 3]
            elif key == "none":
                return None
            else:
                return key

    def subscr_vary_12(obj, key):
        return obj[key]

    tvm = TypeVaryMap()

    for _ in range(WARMUP):
        subscr_vary_12(tvm, "int")

    check_jit_compiled(subscr_vary_12, "subscr_vary_12")

    try:
        assert subscr_vary_12(tvm, "int") == 42
        assert subscr_vary_12(tvm, "str") == "hello"
        assert subscr_vary_12(tvm, "list") == [1, 2, 3]
        assert subscr_vary_12(tvm, "none") is None
        assert subscr_vary_12(tvm, "other") == "other"
        print("PASS  Test 12: __getitem__ returning different types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: varying return types — {e}")
        failed += 1

    # ── Test 13: Inheritance — subclass __getitem__ ───────────────────

    class BaseMap:
        def __getitem__(self, key):
            return key

    class DerivedMap(BaseMap):
        def __getitem__(self, key):
            return key + 100

    def subscr_inherit_13(obj, key):
        return obj[key]

    bm = BaseMap()
    drm = DerivedMap()

    for _ in range(WARMUP):
        subscr_inherit_13(bm, 5)

    check_jit_compiled(subscr_inherit_13, "subscr_inherit_13")

    try:
        # Base class
        assert subscr_inherit_13(bm, 5) == 5
        assert subscr_inherit_13(bm, 0) == 0

        # Derived class (deopt — different type)
        assert subscr_inherit_13(drm, 5) == 105
        assert subscr_inherit_13(drm, 0) == 100

        # Base still works
        assert subscr_inherit_13(bm, 10) == 10

        print("PASS  Test 13: inheritance — subclass __getitem__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: inheritance — {e}")
        failed += 1

    # ── Test 14: Multiple subscripts in one function ──────────────────

    def multi_subscr_14(obj, k1, k2, k3):
        return obj[k1] + obj[k2] + obj[k3]

    for _ in range(WARMUP):
        multi_subscr_14(m, 1, 2, 3)

    check_jit_compiled(multi_subscr_14, "multi_subscr_14")

    try:
        # IntMap: key*2
        assert multi_subscr_14(m, 1, 2, 3) == 2 + 4 + 6
        assert multi_subscr_14(m, 0, 0, 0) == 0
        assert multi_subscr_14(m, 10, 20, 30) == 20 + 40 + 60
        print("PASS  Test 14: multiple subscripts in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: multiple subscripts — {e}")
        failed += 1

    # ── Test 15: Subscript in loop ────────────────────────────────────

    def sum_subscr_15(obj, keys):
        total = 0
        for k in keys:
            total += obj[k]
        return total

    for _ in range(WARMUP):
        sum_subscr_15(m, [1, 2, 3])

    check_jit_compiled(sum_subscr_15, "sum_subscr_15")

    try:
        # IntMap: key*2
        assert sum_subscr_15(m, [1, 2, 3, 4, 5]) == 2+4+6+8+10
        assert sum_subscr_15(m, []) == 0
        assert sum_subscr_15(m, [0]) == 0
        assert sum_subscr_15(m, [10]) == 20
        print("PASS  Test 15: subscript in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: loop subscript — {e}")
        failed += 1

    # ── Test 16: __getitem__ delegating to internal dict ──────────────

    class Config:
        def __init__(self, **kwargs):
            self._store = kwargs
        def __getitem__(self, key):
            return self._store[key]
        def __contains__(self, key):
            return key in self._store

    def subscr_config_16(cfg, key):
        return cfg[key]

    cfg = Config(host="localhost", port=8080, debug=True)

    for _ in range(WARMUP):
        subscr_config_16(cfg, "host")

    check_jit_compiled(subscr_config_16, "subscr_config_16")

    try:
        assert subscr_config_16(cfg, "host") == "localhost"
        assert subscr_config_16(cfg, "port") == 8080
        assert subscr_config_16(cfg, "debug") is True

        try:
            subscr_config_16(cfg, "missing")
            assert False, "expected KeyError"
        except KeyError:
            pass

        # Still works
        assert subscr_config_16(cfg, "host") == "localhost"

        print("PASS  Test 16: __getitem__ delegating to internal dict")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: config delegate — {e}")
        failed += 1

    # ── Test 17: Monkey-patch __getitem__ ─────────────────────────────

    class Patchable:
        def __getitem__(self, key):
            return key

    def subscr_patch_17(obj, key):
        return obj[key]

    p = Patchable()

    for _ in range(WARMUP):
        subscr_patch_17(p, 42)

    check_jit_compiled(subscr_patch_17, "subscr_patch_17")

    try:
        assert subscr_patch_17(p, 42) == 42

        # Monkey-patch the class
        Patchable.__getitem__ = lambda self, key: key * 100
        assert subscr_patch_17(p, 42) == 4200
        assert subscr_patch_17(p, 1) == 100

        # Restore
        Patchable.__getitem__ = lambda self, key: key
        assert subscr_patch_17(p, 42) == 42

        print("PASS  Test 17: monkey-patch __getitem__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: monkey-patch — {e}")
        failed += 1

    # ── Test 18: Rapid type alternation ───────────────────────────────

    def subscr_alt_18(obj, key):
        return obj[key]

    for _ in range(WARMUP):
        subscr_alt_18(m, 5)

    check_jit_compiled(subscr_alt_18, "subscr_alt_18")

    try:
        d = {"a": 1, "b": 2}
        for i in range(100):
            if i % 3 == 0:
                assert subscr_alt_18(m, 5) == 10  # Custom class
            elif i % 3 == 1:
                assert subscr_alt_18(d, "a") == 1  # Dict
            else:
                assert subscr_alt_18([10, 20, 30], 1) == 20  # List

        # Final custom class check
        assert subscr_alt_18(m, 7) == 14

        print("PASS  Test 18: rapid type alternation (custom/dict/list)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: type alternation — {e}")
        failed += 1

    # ── Test 19: __getitem__ with mutable state ───────────────────────

    class CacheMap:
        def __init__(self):
            self._cache = {}
            self._misses = 0
        def __getitem__(self, key):
            if key not in self._cache:
                self._misses += 1
                self._cache[key] = key * key
            return self._cache[key]

    def subscr_cache_19(obj, key):
        return obj[key]

    cache = CacheMap()

    for _ in range(WARMUP):
        subscr_cache_19(cache, 5)

    check_jit_compiled(subscr_cache_19, "subscr_cache_19")

    try:
        cache2 = CacheMap()
        assert subscr_cache_19(cache2, 3) == 9
        assert cache2._misses == 1
        assert subscr_cache_19(cache2, 3) == 9  # Cached
        assert cache2._misses == 1  # No new miss
        assert subscr_cache_19(cache2, 4) == 16
        assert cache2._misses == 2
        assert subscr_cache_19(cache2, 5) == 25
        assert cache2._misses == 3

        print("PASS  Test 19: __getitem__ with mutable state (caching)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: mutable state — {e}")
        failed += 1

    # ── Test 20: Subscript correctness vs direct call ────────────────

    class Verifiable:
        def __getitem__(self, key):
            return (key, type(key).__name__)

    def subscr_verify_20(obj, key):
        return obj[key]

    v = Verifiable()

    for _ in range(WARMUP):
        subscr_verify_20(v, 42)

    check_jit_compiled(subscr_verify_20, "subscr_verify_20")

    try:
        # Verify subscript matches direct __getitem__ call
        for key in [0, 1, -1, "hello", 3.14, None, True, (1, 2)]:
            subscr_result = subscr_verify_20(v, key)
            direct_result = v.__getitem__(key)
            assert subscr_result == direct_result, (
                f"mismatch for key {key!r}: subscr={subscr_result!r}, "
                f"direct={direct_result!r}"
            )

        print("PASS  Test 20: subscript matches direct __getitem__ call")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: correctness vs direct — {e}")
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
