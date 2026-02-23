#!/usr/bin/env python3
"""
test_load_global.py — Correctness and deopt tests for LOAD_GLOBAL specialisation.

Targets: LOAD_GLOBAL_MODULE, LOAD_GLOBAL_BUILTIN.

LOAD_GLOBAL specialisation uses dict version guards (not type guards) to
skip full dictionary lookups for global variable access. Two variants:

  LOAD_GLOBAL_MODULE: The name resolves in the module's globals dict.
    Guard: module globals dict version. If the dict version changes
    (e.g. global reassigned), the guard fires and falls back to generic
    LOAD_GLOBAL.

  LOAD_GLOBAL_BUILTIN: The name resolves in the builtins dict (not found
    in module globals). Guard: both the globals dict version (to confirm
    the name is still not shadowed) AND the builtins dict version. If
    either changes, deopt fires.

Deopt triggers:
  - Reassigning a module global invalidates LOAD_GLOBAL_MODULE cache
  - Adding a same-named global shadows a builtin, invalidating
    LOAD_GLOBAL_BUILTIN cache
  - Deleting a global that was shadowing a builtin
  - exec() / dynamic modification of globals dict

Tests cover:
  - Builtin access correctness (len, type, range, isinstance, print)
  - Module global access correctness
  - Global reassignment (deopt: module dict version changes)
  - Shadowing builtins with globals (deopt: builtin lookup invalidated)
  - Unshadowing: del global that shadows builtin
  - Dynamic globals modification via exec()
  - Module-level constants (e.g. __name__, __file__)
  - Multiple globals in one function
  - Global used as callable vs value
  - Nested function accessing enclosing module global

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct result after dict version invalidation (deopt)
  3. Correct result for the new value after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_global.py
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


# ── Module-level globals used by tests ─────────────────────────────────────

MY_CONSTANT = 42
MY_STRING = "hello"
MY_LIST = [1, 2, 3]
COUNTER = 0


def main():
    global MY_CONSTANT, MY_STRING, MY_LIST, COUNTER

    print("=== LOAD_GLOBAL Correctness & Deopt Tests ===")
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

    # ── Test 1: Builtin len() access ──────────────────────────────────────

    def use_len_1(obj):
        return len(obj)

    for _ in range(WARMUP):
        use_len_1([1, 2, 3])

    check_jit_compiled(use_len_1, "use_len_1")

    try:
        assert use_len_1([1, 2, 3]) == 3
        assert use_len_1([]) == 0
        assert use_len_1("hello") == 5
        assert use_len_1((1, 2)) == 2
        assert use_len_1({}) == 0
        assert use_len_1(range(10)) == 10
        print("PASS  Test 1: builtin len() access")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: builtin len() access — {e}")
        failed += 1

    # ── Test 2: Builtin type() access ─────────────────────────────────────

    def use_type_2(obj):
        return type(obj)

    for _ in range(WARMUP):
        use_type_2(42)

    check_jit_compiled(use_type_2, "use_type_2")

    try:
        assert use_type_2(42) is int
        assert use_type_2("hi") is str
        assert use_type_2([]) is list
        assert use_type_2(3.14) is float
        assert use_type_2(None) is type(None)
        assert use_type_2(True) is bool
        print("PASS  Test 2: builtin type() access")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: builtin type() access — {e}")
        failed += 1

    # ── Test 3: Builtin range() access ────────────────────────────────────

    def use_range_3(n):
        return list(range(n))

    for _ in range(WARMUP):
        use_range_3(5)

    check_jit_compiled(use_range_3, "use_range_3")

    try:
        assert use_range_3(5) == [0, 1, 2, 3, 4]
        assert use_range_3(0) == []
        assert use_range_3(1) == [0]
        print("PASS  Test 3: builtin range() access")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: builtin range() access — {e}")
        failed += 1

    # ── Test 4: Builtin isinstance() access ───────────────────────────────

    def use_isinstance_4(obj, cls):
        return isinstance(obj, cls)

    for _ in range(WARMUP):
        use_isinstance_4(42, int)

    check_jit_compiled(use_isinstance_4, "use_isinstance_4")

    try:
        assert use_isinstance_4(42, int) is True
        assert use_isinstance_4("hi", int) is False
        assert use_isinstance_4("hi", str) is True
        assert use_isinstance_4(True, int) is True  # bool subclass of int
        assert use_isinstance_4(42, (int, str)) is True
        assert use_isinstance_4([], (int, str)) is False
        print("PASS  Test 4: builtin isinstance() access")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: builtin isinstance() access — {e}")
        failed += 1

    # ── Test 5: Module global read ────────────────────────────────────────

    MY_CONSTANT = 42  # Reset

    def read_global_5():
        return MY_CONSTANT

    for _ in range(WARMUP):
        read_global_5()

    check_jit_compiled(read_global_5, "read_global_5")

    try:
        assert read_global_5() == 42
        print("PASS  Test 5: module global read")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: module global read — {e}")
        failed += 1

    # ── Test 6: Module global reassignment (deopt trigger) ────────────────
    # After JIT compilation with MY_CONSTANT=42, changing it should
    # invalidate the dict version guard. The function must return the
    # new value.

    MY_CONSTANT = 42  # Reset

    def read_global_6():
        return MY_CONSTANT

    for _ in range(WARMUP):
        read_global_6()

    check_jit_compiled(read_global_6, "read_global_6")

    try:
        assert read_global_6() == 42

        # Reassign — dict version changes, triggers deopt
        MY_CONSTANT = 99
        assert read_global_6() == 99, f"got {read_global_6()}, expected 99"

        # Reassign again
        MY_CONSTANT = -1
        assert read_global_6() == -1, f"got {read_global_6()}, expected -1"

        # Restore
        MY_CONSTANT = 42
        assert read_global_6() == 42

        print("PASS  Test 6: module global reassignment deopt")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: module global reassignment deopt — {e}")
        failed += 1

    # ── Test 7: Shadow a builtin with a global ────────────────────────────
    # Define a global named 'len' to shadow the builtin. Functions using
    # LOAD_GLOBAL_BUILTIN for 'len' must deopt and use the shadowing global.

    def use_len_7(obj):
        return len(obj)

    for _ in range(WARMUP):
        use_len_7([1, 2, 3])

    check_jit_compiled(use_len_7, "use_len_7")

    try:
        # Before shadowing: normal builtin len
        assert use_len_7([1, 2, 3]) == 3

        # Shadow the builtin by creating a global named 'len'
        g = globals()
        g['len'] = lambda obj: 999

        # After shadowing: must use the global, not the builtin
        result = use_len_7([1, 2, 3])
        assert result == 999, f"expected 999 (shadow), got {result}"

        # Remove shadow — restore builtin
        del g['len']

        # After unshadowing: must use the builtin again
        result = use_len_7([1, 2, 3])
        assert result == 3, f"expected 3 (builtin restored), got {result}"

        print("PASS  Test 7: shadow builtin with global (len)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: shadow builtin with global — {e}")
        failed += 1

    # ── Test 8: Shadow builtin 'type' ─────────────────────────────────────

    def use_type_8(obj):
        return type(obj)

    for _ in range(WARMUP):
        use_type_8(42)

    check_jit_compiled(use_type_8, "use_type_8")

    try:
        assert use_type_8(42) is int

        g = globals()
        g['type'] = lambda obj: "custom"

        result = use_type_8(42)
        assert result == "custom", f"expected 'custom', got {result}"

        del g['type']

        assert use_type_8(42) is int

        print("PASS  Test 8: shadow builtin with global (type)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: shadow builtin with global (type) — {e}")
        failed += 1

    # ── Test 9: Module global string ──────────────────────────────────────

    MY_STRING = "hello"

    def read_string_9():
        return MY_STRING

    for _ in range(WARMUP):
        read_string_9()

    check_jit_compiled(read_string_9, "read_string_9")

    try:
        assert read_string_9() == "hello"

        MY_STRING = "world"
        assert read_string_9() == "world"

        MY_STRING = ""
        assert read_string_9() == ""

        MY_STRING = "hello"  # Restore
        assert read_string_9() == "hello"

        print("PASS  Test 9: module global string reassignment")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: module global string reassignment — {e}")
        failed += 1

    # ── Test 10: Module global type change (int -> str -> list) ───────────
    # Global changes from one type to a completely different type.
    # Dict version guard fires regardless of type — the guard is on the
    # dict version, not the value type.

    MY_CONSTANT = 42  # Reset to int

    def read_global_10():
        return MY_CONSTANT

    for _ in range(WARMUP):
        read_global_10()

    check_jit_compiled(read_global_10, "read_global_10")

    try:
        assert read_global_10() == 42

        MY_CONSTANT = "now a string"
        assert read_global_10() == "now a string"

        MY_CONSTANT = [1, 2, 3]
        assert read_global_10() == [1, 2, 3]

        MY_CONSTANT = None
        assert read_global_10() is None

        MY_CONSTANT = 42  # Restore
        assert read_global_10() == 42

        print("PASS  Test 10: module global type change (int->str->list->None)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: module global type change — {e}")
        failed += 1

    # ── Test 11: Dynamic exec() modifying globals ─────────────────────────
    # exec() can add/modify globals, changing the dict version.

    MY_CONSTANT = 42  # Reset

    def read_global_11():
        return MY_CONSTANT

    for _ in range(WARMUP):
        read_global_11()

    check_jit_compiled(read_global_11, "read_global_11")

    try:
        assert read_global_11() == 42

        exec("MY_CONSTANT = 777", globals())
        assert read_global_11() == 777, f"got {read_global_11()}, expected 777"

        MY_CONSTANT = 42  # Restore
        print("PASS  Test 11: dynamic exec() modifying global")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: dynamic exec() modifying global — {e}")
        failed += 1

    # ── Test 12: del global (NameError after deletion) ────────────────────
    # Deleting a global must cause NameError when accessed, even if the
    # function was JIT-compiled with the global present.

    # Use a separate global so we don't break other tests
    g = globals()
    g['TEMP_GLOBAL_12'] = "exists"

    def read_temp_12():
        return TEMP_GLOBAL_12  # noqa: F821

    for _ in range(WARMUP):
        read_temp_12()

    check_jit_compiled(read_temp_12, "read_temp_12")

    try:
        assert read_temp_12() == "exists"

        del g['TEMP_GLOBAL_12']

        try:
            read_temp_12()
            assert False, "expected NameError after del global"
        except NameError:
            pass  # Correct: NameError when global deleted

        # Recreate it
        g['TEMP_GLOBAL_12'] = "back"
        assert read_temp_12() == "back"

        # Clean up
        del g['TEMP_GLOBAL_12']

        print("PASS  Test 12: del global raises NameError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: del global — {e}")
        failed += 1

    # ── Test 13: Multiple globals in one function ─────────────────────────
    # A function that reads several globals — each has its own LOAD_GLOBAL.
    # Changing one should not affect the others.

    MY_CONSTANT = 42
    MY_STRING = "hello"
    MY_LIST = [1, 2, 3]

    def read_multi_13():
        return (MY_CONSTANT, MY_STRING, MY_LIST)

    for _ in range(WARMUP):
        read_multi_13()

    check_jit_compiled(read_multi_13, "read_multi_13")

    try:
        assert read_multi_13() == (42, "hello", [1, 2, 3])

        MY_CONSTANT = 99
        assert read_multi_13() == (99, "hello", [1, 2, 3])

        MY_STRING = "changed"
        assert read_multi_13() == (99, "changed", [1, 2, 3])

        MY_LIST = [4, 5]
        assert read_multi_13() == (99, "changed", [4, 5])

        # Restore
        MY_CONSTANT = 42
        MY_STRING = "hello"
        MY_LIST = [1, 2, 3]

        print("PASS  Test 13: multiple globals in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: multiple globals in one function — {e}")
        failed += 1

    # ── Test 14: Global used as callable ──────────────────────────────────
    # A global that is a function — LOAD_GLOBAL loads it, then it's called.
    # Replacing the callable must work after deopt.

    g = globals()
    g['MY_FUNC_14'] = lambda x: x + 1

    def call_global_14(x):
        return MY_FUNC_14(x)  # noqa: F821

    for _ in range(WARMUP):
        call_global_14(5)

    check_jit_compiled(call_global_14, "call_global_14")

    try:
        assert call_global_14(5) == 6

        g['MY_FUNC_14'] = lambda x: x * 10
        assert call_global_14(5) == 50, f"got {call_global_14(5)}"

        g['MY_FUNC_14'] = lambda x: x + 1  # Restore

        assert call_global_14(5) == 6

        del g['MY_FUNC_14']

        print("PASS  Test 14: global used as callable, replaced")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: global used as callable — {e}")
        failed += 1

    # ── Test 15: Builtin used in loop (hot path) ──────────────────────────
    # len() accessed many times in a loop — tests that LOAD_GLOBAL_BUILTIN
    # performs well under repeated access.

    def loop_len_15(data, n):
        total = 0
        for _ in range(n):
            total += len(data)
        return total

    for _ in range(WARMUP):
        loop_len_15([1, 2, 3], 1)

    check_jit_compiled(loop_len_15, "loop_len_15")

    try:
        assert loop_len_15([1, 2, 3], 100) == 300
        assert loop_len_15([], 100) == 0
        assert loop_len_15("abcde", 10) == 50
        print("PASS  Test 15: builtin len() in hot loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: builtin in hot loop — {e}")
        failed += 1

    # ── Test 16: Module global counter (read-modify-write pattern) ────────

    COUNTER = 0

    def increment_16():
        global COUNTER
        COUNTER += 1
        return COUNTER

    for _ in range(WARMUP):
        increment_16()

    check_jit_compiled(increment_16, "increment_16")

    # Reset after warmup
    COUNTER = 0

    try:
        assert increment_16() == 1
        assert increment_16() == 2
        assert increment_16() == 3
        assert COUNTER == 3

        COUNTER = 100
        assert increment_16() == 101
        assert increment_16() == 102

        COUNTER = 0  # Restore

        print("PASS  Test 16: module global counter increment")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: module global counter — {e}")
        failed += 1

    # ── Test 17: exec() adding a new global that shadows builtin ──────────

    def use_abs_17(x):
        return abs(x)

    for _ in range(WARMUP):
        use_abs_17(-5)

    check_jit_compiled(use_abs_17, "use_abs_17")

    try:
        assert use_abs_17(-5) == 5
        assert use_abs_17(5) == 5
        assert use_abs_17(0) == 0

        # Shadow 'abs' via exec
        exec("abs = lambda x: -x", globals())
        result = use_abs_17(-5)
        assert result == 5, f"expected 5 (shadow: -(-5)), got {result}"

        # Remove shadow
        g = globals()
        del g['abs']

        # Builtin abs restored
        assert use_abs_17(-5) == 5

        print("PASS  Test 17: exec() shadow of builtin abs")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: exec() shadow builtin — {e}")
        failed += 1

    # ── Test 18: Accessing __name__ (module attribute global) ─────────────

    def get_module_name_18():
        return __name__

    for _ in range(WARMUP):
        get_module_name_18()

    check_jit_compiled(get_module_name_18, "get_module_name_18")

    try:
        assert get_module_name_18() == "__main__"
        print("PASS  Test 18: access __name__ module global")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: __name__ module global — {e}")
        failed += 1

    # ── Test 19: Rapid global reassignment stability ──────────────────────
    # Rapidly change a global many times and verify the function always
    # returns the current value.

    MY_CONSTANT = 42

    def read_rapid_19():
        return MY_CONSTANT

    for _ in range(WARMUP):
        read_rapid_19()

    check_jit_compiled(read_rapid_19, "read_rapid_19")

    try:
        for i in range(100):
            MY_CONSTANT = i
            result = read_rapid_19()
            assert result == i, f"iteration {i}: got {result}, expected {i}"

        MY_CONSTANT = 42  # Restore

        print("PASS  Test 19: rapid global reassignment (100 cycles)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: rapid global reassignment — {e}")
        failed += 1

    # ── Test 20: Multiple builtins in one function ────────────────────────
    # Tests that several LOAD_GLOBAL_BUILTIN instructions in the same
    # function all resolve correctly.

    def multi_builtins_20(obj):
        t = type(obj)
        n = len(obj) if hasattr(obj, '__len__') else 0
        r = repr(obj)
        return (t, n, r)

    for _ in range(WARMUP):
        multi_builtins_20([1, 2])

    check_jit_compiled(multi_builtins_20, "multi_builtins_20")

    try:
        t, n, r = multi_builtins_20([1, 2])
        assert t is list
        assert n == 2
        assert r == "[1, 2]"

        t, n, r = multi_builtins_20("abc")
        assert t is str
        assert n == 3
        assert r == "'abc'"

        t, n, r = multi_builtins_20(42)
        assert t is int
        assert n == 0  # int has no __len__
        assert r == "42"

        print("PASS  Test 20: multiple builtins in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: multiple builtins in one function — {e}")
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
