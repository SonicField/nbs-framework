#!/usr/bin/env python3
"""
test_contains_op_dict.py — Correctness and deopt tests for CONTAINS_OP_DICT
specialisation.

Targets: CONTAINS_OP_DICT.

CONTAINS_OP_DICT specialises the 'in' operator when the right operand is a
dict. CPython's adaptive interpreter detects that the container is a dict and
replaces generic CONTAINS_OP with CONTAINS_OP_DICT, which calls
PyDict_Contains directly instead of going through the generic
PySequence_Contains → tp_as_sequence → sq_contains dispatch chain.

The JIT specialisation emits GuardType(TDictExact) on the container operand,
allowing the Simplify pass to use the direct dict lookup path.

Deopt triggers:
  - Function JIT-compiled with dict container, then called with list/set/str
  - Custom __contains__ objects
  - Dict subclass (OrderedDict, defaultdict)

Tests cover:
  - Basic dict containment (key present, key absent)
  - String keys, int keys, tuple keys, None key
  - Empty dict
  - 'not in' operator (uses CONTAINS_OP with inverted flag)
  - Deopt: dict-compiled → list container
  - Deopt: dict-compiled → set container
  - Deopt: dict-compiled → string container
  - Deopt: dict-compiled → custom __contains__
  - Dict mutation between containment checks
  - Large dict containment
  - Mixed key types in same dict
  - Bool keys (True/False as dict keys — hash collision with 1/0)
  - Containment in loop (hot path)
  - Multiple containment checks in one function

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Correct result for both original and new types after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_contains_op_dict.py
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
    print("=== CONTAINS_OP_DICT Correctness & Deopt Tests ===")
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

    # ── Test 1: Basic dict containment (string keys) ─────────────────────

    def contains_str_1(key, d):
        return key in d

    test_dict = {"a": 1, "b": 2, "c": 3}

    for _ in range(WARMUP):
        contains_str_1("a", test_dict)

    check_jit_compiled(contains_str_1, "contains_str_1")

    try:
        assert contains_str_1("a", test_dict) is True
        assert contains_str_1("b", test_dict) is True
        assert contains_str_1("c", test_dict) is True
        assert contains_str_1("d", test_dict) is False
        assert contains_str_1("", test_dict) is False
        assert contains_str_1("ab", test_dict) is False
        print("PASS  Test 1: basic dict containment (string keys)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: basic dict containment — {e}")
        failed += 1

    # ── Test 2: Int keys ─────────────────────────────────────────────────

    def contains_int_2(key, d):
        return key in d

    int_dict = {1: "one", 2: "two", 3: "three", 0: "zero"}

    for _ in range(WARMUP):
        contains_int_2(1, int_dict)

    check_jit_compiled(contains_int_2, "contains_int_2")

    try:
        assert contains_int_2(1, int_dict) is True
        assert contains_int_2(0, int_dict) is True
        assert contains_int_2(3, int_dict) is True
        assert contains_int_2(4, int_dict) is False
        assert contains_int_2(-1, int_dict) is False
        assert contains_int_2(99, int_dict) is False
        print("PASS  Test 2: dict containment (int keys)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: int keys — {e}")
        failed += 1

    # ── Test 3: Empty dict ───────────────────────────────────────────────

    def contains_empty_3(key, d):
        return key in d

    for _ in range(WARMUP):
        contains_empty_3("x", {})

    check_jit_compiled(contains_empty_3, "contains_empty_3")

    try:
        assert contains_empty_3("x", {}) is False
        assert contains_empty_3(0, {}) is False
        assert contains_empty_3(None, {}) is False
        assert contains_empty_3("", {}) is False
        print("PASS  Test 3: empty dict containment")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: empty dict — {e}")
        failed += 1

    # ── Test 4: None key ─────────────────────────────────────────────────

    def contains_none_4(key, d):
        return key in d

    none_dict = {None: "null", "a": 1}

    for _ in range(WARMUP):
        contains_none_4(None, none_dict)

    check_jit_compiled(contains_none_4, "contains_none_4")

    try:
        assert contains_none_4(None, none_dict) is True
        assert contains_none_4("a", none_dict) is True
        assert contains_none_4("b", none_dict) is False
        print("PASS  Test 4: None as dict key")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: None key — {e}")
        failed += 1

    # ── Test 5: Tuple keys ───────────────────────────────────────────────

    def contains_tuple_5(key, d):
        return key in d

    tuple_dict = {(1, 2): "a", (3, 4): "b", (): "empty"}

    for _ in range(WARMUP):
        contains_tuple_5((1, 2), tuple_dict)

    check_jit_compiled(contains_tuple_5, "contains_tuple_5")

    try:
        assert contains_tuple_5((1, 2), tuple_dict) is True
        assert contains_tuple_5((3, 4), tuple_dict) is True
        assert contains_tuple_5((), tuple_dict) is True
        assert contains_tuple_5((1, 3), tuple_dict) is False
        assert contains_tuple_5((2, 1), tuple_dict) is False
        print("PASS  Test 5: tuple keys")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: tuple keys — {e}")
        failed += 1

    # ── Test 6: 'not in' operator ────────────────────────────────────────

    def not_contains_6(key, d):
        return key not in d

    for _ in range(WARMUP):
        not_contains_6("x", test_dict)

    check_jit_compiled(not_contains_6, "not_contains_6")

    try:
        assert not_contains_6("a", test_dict) is False
        assert not_contains_6("d", test_dict) is True
        assert not_contains_6("", test_dict) is True
        assert not_contains_6("b", test_dict) is False
        print("PASS  Test 6: 'not in' operator")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: 'not in' — {e}")
        failed += 1

    # ── Test 7: Deopt dict → list ────────────────────────────────────────

    def contains_deopt_7(key, container):
        return key in container

    for _ in range(WARMUP):
        contains_deopt_7("a", test_dict)

    check_jit_compiled(contains_deopt_7, "contains_deopt_7")

    try:
        assert contains_deopt_7("a", test_dict) is True
        assert contains_deopt_7("d", test_dict) is False

        # Deopt: list container
        assert contains_deopt_7("a", ["a", "b", "c"]) is True
        assert contains_deopt_7("d", ["a", "b", "c"]) is False

        # Dict still works after deopt
        assert contains_deopt_7("b", test_dict) is True

        print("PASS  Test 7: deopt dict → list")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: deopt dict → list — {e}")
        failed += 1

    # ── Test 8: Deopt dict → set ─────────────────────────────────────────

    def contains_deopt_8(key, container):
        return key in container

    for _ in range(WARMUP):
        contains_deopt_8("a", test_dict)

    check_jit_compiled(contains_deopt_8, "contains_deopt_8")

    try:
        assert contains_deopt_8("a", test_dict) is True

        # Deopt: set container
        assert contains_deopt_8("a", {"a", "b", "c"}) is True
        assert contains_deopt_8("d", {"a", "b", "c"}) is False

        # Dict still works
        assert contains_deopt_8("c", test_dict) is True

        print("PASS  Test 8: deopt dict → set")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: deopt dict → set — {e}")
        failed += 1

    # ── Test 9: Deopt dict → string ──────────────────────────────────────

    def contains_deopt_9(key, container):
        return key in container

    for _ in range(WARMUP):
        contains_deopt_9("a", test_dict)

    check_jit_compiled(contains_deopt_9, "contains_deopt_9")

    try:
        assert contains_deopt_9("a", test_dict) is True

        # Deopt: string container (substring check)
        assert contains_deopt_9("a", "abc") is True
        assert contains_deopt_9("d", "abc") is False
        assert contains_deopt_9("ab", "abc") is True

        # Dict still works
        assert contains_deopt_9("b", test_dict) is True

        print("PASS  Test 9: deopt dict → string")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: deopt dict → string — {e}")
        failed += 1

    # ── Test 10: Deopt dict → custom __contains__ ────────────────────────

    class AlwaysContains:
        def __contains__(self, item):
            return True

    class NeverContains:
        def __contains__(self, item):
            return False

    def contains_deopt_10(key, container):
        return key in container

    for _ in range(WARMUP):
        contains_deopt_10("a", test_dict)

    check_jit_compiled(contains_deopt_10, "contains_deopt_10")

    try:
        assert contains_deopt_10("a", test_dict) is True

        # Deopt: custom __contains__
        assert contains_deopt_10("anything", AlwaysContains()) is True
        assert contains_deopt_10("anything", NeverContains()) is False

        # Dict still works
        assert contains_deopt_10("d", test_dict) is False

        print("PASS  Test 10: deopt dict → custom __contains__")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: deopt custom __contains__ — {e}")
        failed += 1

    # ── Test 11: Dict mutation between checks ────────────────────────────

    def contains_mutate_11(key, d):
        return key in d

    mutable = {"x": 1, "y": 2}

    for _ in range(WARMUP):
        contains_mutate_11("x", mutable)

    check_jit_compiled(contains_mutate_11, "contains_mutate_11")

    try:
        assert contains_mutate_11("x", mutable) is True
        assert contains_mutate_11("z", mutable) is False

        # Mutate: add key
        mutable["z"] = 3
        assert contains_mutate_11("z", mutable) is True

        # Mutate: remove key
        del mutable["x"]
        assert contains_mutate_11("x", mutable) is False

        # Mutate: clear
        mutable.clear()
        assert contains_mutate_11("y", mutable) is False
        assert contains_mutate_11("z", mutable) is False

        print("PASS  Test 11: dict mutation between checks")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: dict mutation — {e}")
        failed += 1

    # ── Test 12: Large dict ──────────────────────────────────────────────

    def contains_large_12(key, d):
        return key in d

    large_dict = {f"key_{i}": i for i in range(10000)}

    for _ in range(WARMUP):
        contains_large_12("key_0", large_dict)

    check_jit_compiled(contains_large_12, "contains_large_12")

    try:
        assert contains_large_12("key_0", large_dict) is True
        assert contains_large_12("key_9999", large_dict) is True
        assert contains_large_12("key_5000", large_dict) is True
        assert contains_large_12("key_10000", large_dict) is False
        assert contains_large_12("missing", large_dict) is False
        print("PASS  Test 12: large dict (10000 keys)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: large dict — {e}")
        failed += 1

    # ── Test 13: Mixed key types ─────────────────────────────────────────

    def contains_mixed_13(key, d):
        return key in d

    mixed_dict = {1: "int", "a": "str", (1, 2): "tuple", None: "none"}

    for _ in range(WARMUP):
        contains_mixed_13(1, mixed_dict)

    check_jit_compiled(contains_mixed_13, "contains_mixed_13")

    try:
        assert contains_mixed_13(1, mixed_dict) is True
        assert contains_mixed_13("a", mixed_dict) is True
        assert contains_mixed_13((1, 2), mixed_dict) is True
        assert contains_mixed_13(None, mixed_dict) is True
        assert contains_mixed_13(2, mixed_dict) is False
        assert contains_mixed_13("b", mixed_dict) is False
        print("PASS  Test 13: mixed key types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: mixed key types — {e}")
        failed += 1

    # ── Test 14: Bool keys (hash collision with int 0/1) ─────────────────
    # In Python, True == 1 and False == 0, and hash(True) == hash(1).
    # So {True: "t"} and {1: "t"} have the same key.

    def contains_bool_14(key, d):
        return key in d

    bool_dict = {True: "yes", False: "no"}

    for _ in range(WARMUP):
        contains_bool_14(True, bool_dict)

    check_jit_compiled(contains_bool_14, "contains_bool_14")

    try:
        assert contains_bool_14(True, bool_dict) is True
        assert contains_bool_14(False, bool_dict) is True
        # 1 == True and 0 == False, so they are the same key
        assert contains_bool_14(1, bool_dict) is True
        assert contains_bool_14(0, bool_dict) is True
        assert contains_bool_14(2, bool_dict) is False
        print("PASS  Test 14: bool keys (hash collision with int 0/1)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: bool keys — {e}")
        failed += 1

    # ── Test 15: Containment in hot loop ─────────────────────────────────

    def loop_contains_15(keys, d):
        count = 0
        for k in keys:
            if k in d:
                count += 1
        return count

    lookup_dict = {"a": 1, "c": 3, "e": 5}

    for _ in range(WARMUP):
        loop_contains_15(["a", "b"], lookup_dict)

    check_jit_compiled(loop_contains_15, "loop_contains_15")

    try:
        assert loop_contains_15(["a", "b", "c", "d", "e"], lookup_dict) == 3
        assert loop_contains_15([], lookup_dict) == 0
        assert loop_contains_15(["x", "y", "z"], lookup_dict) == 0
        assert loop_contains_15(["a", "a", "a"], lookup_dict) == 3
        print("PASS  Test 15: containment in hot loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: containment loop — {e}")
        failed += 1

    # ── Test 16: Multiple containment checks in one function ─────────────

    def multi_contains_16(k1, k2, k3, d):
        return (k1 in d, k2 in d, k3 in d)

    for _ in range(WARMUP):
        multi_contains_16("a", "b", "c", test_dict)

    check_jit_compiled(multi_contains_16, "multi_contains_16")

    try:
        assert multi_contains_16("a", "b", "c", test_dict) == (True, True, True)
        assert multi_contains_16("a", "d", "c", test_dict) == (True, False, True)
        assert multi_contains_16("x", "y", "z", test_dict) == (False, False, False)
        print("PASS  Test 16: multiple containment checks in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: multiple containment — {e}")
        failed += 1

    # ── Test 17: Containment used in conditional assignment ──────────────

    def get_or_default_17(key, d, default):
        if key in d:
            return d[key]
        return default

    for _ in range(WARMUP):
        get_or_default_17("a", test_dict, -1)

    check_jit_compiled(get_or_default_17, "get_or_default_17")

    try:
        assert get_or_default_17("a", test_dict, -1) == 1
        assert get_or_default_17("d", test_dict, -1) == -1
        assert get_or_default_17("b", test_dict, 99) == 2
        assert get_or_default_17("z", test_dict, 99) == 99
        print("PASS  Test 17: containment in conditional assignment")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: conditional assignment — {e}")
        failed += 1

    # ── Test 18: Rapid container type alternation ────────────────────────

    def contains_rapid_18(key, container):
        return key in container

    for _ in range(WARMUP):
        contains_rapid_18("a", test_dict)

    check_jit_compiled(contains_rapid_18, "contains_rapid_18")

    try:
        containers = [
            (test_dict, "a", True),
            (["a", "b"], "a", True),
            ({"a", "b"}, "a", True),
            ("abc", "a", True),
            (test_dict, "d", False),
            (["x"], "a", False),
            (set(), "a", False),
            ("xyz", "a", False),
        ]
        for i in range(10):
            for container, key, expected in containers:
                result = contains_rapid_18(key, container)
                assert result is expected, (
                    f"cycle {i}, container={type(container).__name__}, "
                    f"key={key!r}: got {result}, expected {expected}"
                )

        print("PASS  Test 18: rapid container type alternation (10 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: rapid type alternation — {e}")
        failed += 1

    # ── Test 19: Unhashable key raises TypeError ─────────────────────────

    def contains_unhashable_19(key, d):
        return key in d

    for _ in range(WARMUP):
        contains_unhashable_19("a", test_dict)

    check_jit_compiled(contains_unhashable_19, "contains_unhashable_19")

    try:
        assert contains_unhashable_19("a", test_dict) is True

        # Unhashable key raises TypeError
        try:
            contains_unhashable_19([1, 2], test_dict)
            assert False, "expected TypeError for unhashable list key"
        except TypeError:
            pass

        try:
            contains_unhashable_19({"nested": 1}, test_dict)
            assert False, "expected TypeError for unhashable dict key"
        except TypeError:
            pass

        # Normal keys still work after TypeError
        assert contains_unhashable_19("b", test_dict) is True
        assert contains_unhashable_19("z", test_dict) is False

        print("PASS  Test 19: unhashable key raises TypeError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: unhashable key — {e}")
        failed += 1

    # ── Test 20: 'in' with dict.keys() / dict.values() ──────────────────
    # These are dict views, not dicts — should deopt from CONTAINS_OP_DICT.

    def contains_view_20(key, container):
        return key in container

    for _ in range(WARMUP):
        contains_view_20("a", test_dict)

    check_jit_compiled(contains_view_20, "contains_view_20")

    try:
        assert contains_view_20("a", test_dict) is True

        # Dict keys view (deopt from dict)
        assert contains_view_20("a", test_dict.keys()) is True
        assert contains_view_20("d", test_dict.keys()) is False

        # Dict values view (deopt, different semantics — checks values)
        assert contains_view_20(1, test_dict.values()) is True
        assert contains_view_20(99, test_dict.values()) is False

        # Dict still works
        assert contains_view_20("c", test_dict) is True

        print("PASS  Test 20: 'in' with dict views (keys/values deopt)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: dict views — {e}")
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
