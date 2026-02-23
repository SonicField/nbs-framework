#!/usr/bin/env python3
"""
test_load_global_module.py — Correctness and deopt tests for
LOAD_GLOBAL_MODULE specialisation.

Targets: LOAD_GLOBAL_MODULE.

LOAD_GLOBAL_MODULE specialises global variable lookups when the name is
found in the module's global namespace (as opposed to builtins). Instead
of doing a full dict lookup on every access, it caches the dict version
and the value pointer, only rechecking when the dict version changes.

The JIT specialisation checks the module dict's ma_version_tag. If it
matches the cached version, the cached value is returned directly without
a dict lookup. If the version has changed (due to mutation of any global),
the specialisation deopts or re-validates.

Deopt triggers:
  - Module dict version changes (any global added, modified, or deleted)
  - Global is deleted (KeyError in globals)
  - Global is shadowed by a new assignment

Tests cover:
  - Basic global variable read
  - Different global types (int, str, list, dict, function, class)
  - Module-level constant patterns
  - Global mutation (value changes between calls)
  - Global deletion (del global_var)
  - Adding new globals at runtime
  - Builtin name (falls through to LOAD_GLOBAL_BUILTIN, not MODULE)
  - Global function reference
  - Global class reference
  - Loop reading same global repeatedly
  - Multiple globals in one function
  - Global reassignment stability
  - Rapid mutation pattern
  - globals() dict manipulation
  - Global None/True/False
  - Module attribute access equivalence
  - Nested function reading enclosing module globals
  - Global list mutation (value mutates, reference unchanged)
  - Import-time vs runtime global
  - Equivalence: direct access vs globals()[name]

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check result)
  2. Correct result after global mutation (version invalidation)
  3. Global semantics preserved (deletion, shadowing, etc.)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_global_module.py
"""

import sys

WARMUP = 15000  # CinderX auto-compilation typically needs 10000+ calls

# Set to True to require JIT compilation when cinderjit is available.
REQUIRE_JIT = True

# --- Module-level globals for testing ---
GLOBAL_INT = 42
GLOBAL_STR = "hello"
GLOBAL_LIST = [1, 2, 3]
GLOBAL_DICT = {"key": "value"}
GLOBAL_NONE = None
GLOBAL_BOOL = True
GLOBAL_CONST = 3.14159
GLOBAL_COUNTER = 0
MUTABLE_GLOBAL = "original"


def global_function():
    return "from_function"


class GlobalClass:
    value = 99


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
    global GLOBAL_INT, GLOBAL_STR, GLOBAL_LIST, GLOBAL_DICT
    global GLOBAL_NONE, GLOBAL_BOOL, GLOBAL_CONST, GLOBAL_COUNTER
    global MUTABLE_GLOBAL

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

    passed = 0
    failed = 0

    # ------------------------------------------------------------------
    # Test 1: Basic global int read
    # ------------------------------------------------------------------
    try:
        def read_global_int():
            return GLOBAL_INT

        for _ in range(WARMUP):
            read_global_int()
        check_jit_compiled(read_global_int, "read_global_int")

        assert read_global_int() == 42
        print("  PASS: test_basic_global_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_global_int — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Different global types
    # ------------------------------------------------------------------
    try:
        def read_globals():
            return (GLOBAL_INT, GLOBAL_STR, GLOBAL_CONST)

        for _ in range(WARMUP):
            read_globals()
        check_jit_compiled(read_globals, "read_globals")

        assert read_globals() == (42, "hello", 3.14159)
        print("  PASS: test_different_global_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_different_global_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Global mutation (value changes between calls)
    # ------------------------------------------------------------------
    try:
        def read_mutable():
            return MUTABLE_GLOBAL

        MUTABLE_GLOBAL = "original"
        for _ in range(WARMUP):
            read_mutable()
        check_jit_compiled(read_mutable, "read_mutable")

        assert read_mutable() == "original"

        # Mutate the global — dict version changes, cached value invalid
        MUTABLE_GLOBAL = "modified"
        assert read_mutable() == "modified"

        MUTABLE_GLOBAL = "modified_again"
        assert read_mutable() == "modified_again"

        # Restore
        MUTABLE_GLOBAL = "original"
        assert read_mutable() == "original"
        print("  PASS: test_global_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_mutation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Global deletion
    # ------------------------------------------------------------------
    try:
        # Use a temporary global for deletion test
        globals()["TEMP_GLOBAL"] = "exists"

        def read_temp():
            return TEMP_GLOBAL  # noqa: F821

        for _ in range(WARMUP):
            read_temp()
        check_jit_compiled(read_temp, "read_temp")

        assert read_temp() == "exists"

        # Delete the global
        del globals()["TEMP_GLOBAL"]

        raised = False
        try:
            read_temp()
        except NameError:
            raised = True
        assert raised, "Expected NameError after deleting global"

        # Re-create it
        globals()["TEMP_GLOBAL"] = "recreated"
        assert read_temp() == "recreated"

        # Clean up
        del globals()["TEMP_GLOBAL"]
        print("  PASS: test_global_deletion")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_deletion — {e}")
        # Clean up on failure
        globals().pop("TEMP_GLOBAL", None)
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Adding new globals at runtime
    # ------------------------------------------------------------------
    try:
        def read_dynamic():
            return DYNAMIC_GLOBAL  # noqa: F821

        # Create the global before warming up
        globals()["DYNAMIC_GLOBAL"] = 100

        for _ in range(WARMUP):
            read_dynamic()
        check_jit_compiled(read_dynamic, "read_dynamic")

        assert read_dynamic() == 100

        # Change it
        globals()["DYNAMIC_GLOBAL"] = 200
        assert read_dynamic() == 200

        # Clean up
        del globals()["DYNAMIC_GLOBAL"]
        print("  PASS: test_dynamic_global")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dynamic_global — {e}")
        globals().pop("DYNAMIC_GLOBAL", None)
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Global function reference
    # ------------------------------------------------------------------
    try:
        def call_global_fn():
            return global_function()

        for _ in range(WARMUP):
            call_global_fn()
        check_jit_compiled(call_global_fn, "call_global_fn")

        assert call_global_fn() == "from_function"
        print("  PASS: test_global_function_ref")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_function_ref — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Global class reference
    # ------------------------------------------------------------------
    try:
        def use_global_class():
            return GlobalClass.value

        for _ in range(WARMUP):
            use_global_class()
        check_jit_compiled(use_global_class, "use_global_class")

        assert use_global_class() == 99
        print("  PASS: test_global_class_ref")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_class_ref — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Loop reading same global repeatedly
    # ------------------------------------------------------------------
    try:
        def read_global_in_loop(n):
            total = 0
            for _ in range(n):
                total += GLOBAL_INT
            return total

        for _ in range(WARMUP):
            read_global_in_loop(10)
        check_jit_compiled(read_global_in_loop, "read_global_in_loop")

        assert read_global_in_loop(10) == 420   # 42 * 10
        assert read_global_in_loop(0) == 0
        assert read_global_in_loop(1) == 42
        assert read_global_in_loop(100) == 4200
        print("  PASS: test_global_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Multiple globals in one function
    # ------------------------------------------------------------------
    try:
        def read_multiple_globals():
            return GLOBAL_INT + len(GLOBAL_STR) + len(GLOBAL_LIST)

        for _ in range(WARMUP):
            read_multiple_globals()
        check_jit_compiled(read_multiple_globals, "read_multiple_globals")

        # 42 + 5 + 3 = 50
        assert read_multiple_globals() == 50
        print("  PASS: test_multiple_globals")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_globals — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Global reassignment stability
    # ------------------------------------------------------------------
    try:
        def read_counter():
            return GLOBAL_COUNTER

        GLOBAL_COUNTER = 0
        for _ in range(WARMUP):
            read_counter()
        check_jit_compiled(read_counter, "read_counter")

        assert read_counter() == 0

        # Reassign many times — each changes dict version
        for i in range(1, 101):
            GLOBAL_COUNTER = i
            assert read_counter() == i, f"Failed at counter={i}"

        GLOBAL_COUNTER = 0  # restore
        print("  PASS: test_reassignment_stability")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_reassignment_stability — {e}")
        GLOBAL_COUNTER = 0
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Rapid mutation pattern
    # ------------------------------------------------------------------
    try:
        def read_mutable_rapid():
            return MUTABLE_GLOBAL

        MUTABLE_GLOBAL = "start"
        for _ in range(WARMUP):
            read_mutable_rapid()
        check_jit_compiled(read_mutable_rapid, "read_mutable_rapid")

        for cycle in range(50):
            MUTABLE_GLOBAL = f"val_{cycle}"
            assert read_mutable_rapid() == f"val_{cycle}", (
                f"Failed at cycle {cycle}"
            )

        MUTABLE_GLOBAL = "original"
        assert read_mutable_rapid() == "original"
        print("  PASS: test_rapid_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_mutation — {e}")
        MUTABLE_GLOBAL = "original"
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: globals() dict manipulation
    # ------------------------------------------------------------------
    try:
        def read_manipulated():
            return GLOBAL_INT

        for _ in range(WARMUP):
            read_manipulated()
        check_jit_compiled(read_manipulated, "read_manipulated")

        assert read_manipulated() == 42

        # Modify via globals() dict — should invalidate version
        old_val = globals()["GLOBAL_INT"]
        globals()["GLOBAL_INT"] = 999
        assert read_manipulated() == 999

        # Restore
        globals()["GLOBAL_INT"] = old_val
        assert read_manipulated() == 42
        GLOBAL_INT = 42  # ensure restored
        print("  PASS: test_globals_dict_manipulation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_globals_dict_manipulation — {e}")
        GLOBAL_INT = 42
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Global None/True/False
    # ------------------------------------------------------------------
    try:
        def read_none():
            return GLOBAL_NONE

        def read_bool():
            return GLOBAL_BOOL

        for _ in range(WARMUP):
            read_none()
            read_bool()
        check_jit_compiled(read_none, "read_none")

        assert read_none() is None
        assert read_bool() is True
        print("  PASS: test_global_none_bool")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_none_bool — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Nested function reading enclosing module globals
    # ------------------------------------------------------------------
    try:
        def outer():
            def inner():
                return GLOBAL_INT
            return inner()

        for _ in range(WARMUP):
            outer()
        check_jit_compiled(outer, "outer")

        assert outer() == 42
        print("  PASS: test_nested_function_globals")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_nested_function_globals — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Global list mutation (value mutates, reference unchanged)
    # ------------------------------------------------------------------
    try:
        def read_global_list():
            return GLOBAL_LIST

        for _ in range(WARMUP):
            read_global_list()
        check_jit_compiled(read_global_list, "read_global_list")

        # Same reference, mutated in-place — global ref unchanged
        original_id = id(GLOBAL_LIST)
        assert read_global_list() == [1, 2, 3]

        GLOBAL_LIST.append(4)
        assert read_global_list() == [1, 2, 3, 4]
        assert id(read_global_list()) == original_id  # same object

        # Restore
        GLOBAL_LIST.pop()
        assert read_global_list() == [1, 2, 3]
        print("  PASS: test_global_list_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_list_mutation — {e}")
        while len(GLOBAL_LIST) > 3:
            GLOBAL_LIST.pop()
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Global dict read
    # ------------------------------------------------------------------
    try:
        def read_global_dict():
            return GLOBAL_DICT["key"]

        for _ in range(WARMUP):
            read_global_dict()
        check_jit_compiled(read_global_dict, "read_global_dict")

        assert read_global_dict() == "value"

        # Mutate dict in-place — global ref unchanged but content changes
        GLOBAL_DICT["key"] = "new_value"
        assert read_global_dict() == "new_value"

        # Restore
        GLOBAL_DICT["key"] = "value"
        print("  PASS: test_global_dict_read")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_dict_read — {e}")
        GLOBAL_DICT["key"] = "value"
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Builtin shadowing by module global
    # ------------------------------------------------------------------
    try:
        # len is a builtin — using it as a global name shadows it
        def use_len():
            return len(GLOBAL_LIST)

        for _ in range(WARMUP):
            use_len()
        check_jit_compiled(use_len, "use_len")

        assert use_len() == 3  # len([1,2,3])

        # Shadow len in module globals
        globals()["len"] = lambda x: -1  # pathological shadow

        assert use_len() == -1  # now uses our shadow

        # Remove shadow — falls back to builtin
        del globals()["len"]
        assert use_len() == 3  # back to builtin len
        print("  PASS: test_builtin_shadowing")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_builtin_shadowing — {e}")
        globals().pop("len", None)  # ensure cleanup
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Global type changes
    # ------------------------------------------------------------------
    try:
        def read_changing_type():
            return MUTABLE_GLOBAL

        MUTABLE_GLOBAL = 42
        for _ in range(WARMUP):
            read_changing_type()
        check_jit_compiled(read_changing_type, "read_changing_type")

        assert read_changing_type() == 42

        # Change type to string
        MUTABLE_GLOBAL = "now a string"
        assert read_changing_type() == "now a string"

        # Change type to list
        MUTABLE_GLOBAL = [1, 2, 3]
        assert read_changing_type() == [1, 2, 3]

        # Change type to None
        MUTABLE_GLOBAL = None
        assert read_changing_type() is None

        # Restore
        MUTABLE_GLOBAL = "original"
        print("  PASS: test_global_type_changes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_global_type_changes — {e}")
        MUTABLE_GLOBAL = "original"
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Many globals added (dict resize)
    # ------------------------------------------------------------------
    try:
        def read_stable():
            return GLOBAL_INT

        for _ in range(WARMUP):
            read_stable()
        check_jit_compiled(read_stable, "read_stable")

        assert read_stable() == 42

        # Add many globals — may trigger dict resize, version change
        temp_keys = []
        for i in range(50):
            key = f"_TEMP_BULK_{i}"
            globals()[key] = i
            temp_keys.append(key)

        # GLOBAL_INT should still be readable
        assert read_stable() == 42

        # Clean up
        for key in temp_keys:
            del globals()[key]

        assert read_stable() == 42
        print("  PASS: test_many_globals_added")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_many_globals_added — {e}")
        for key in list(globals()):
            if key.startswith("_TEMP_BULK_"):
                del globals()[key]
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — direct access vs globals()[name]
    # ------------------------------------------------------------------
    try:
        def direct_read():
            return GLOBAL_INT

        for _ in range(WARMUP):
            direct_read()
        check_jit_compiled(direct_read, "direct_read")

        assert direct_read() == globals()["GLOBAL_INT"]

        # Mutate and verify both paths agree
        old = GLOBAL_INT
        GLOBAL_INT = 777
        assert direct_read() == globals()["GLOBAL_INT"]
        assert direct_read() == 777

        # Restore
        GLOBAL_INT = old
        assert direct_read() == globals()["GLOBAL_INT"]
        print("  PASS: test_equivalence_direct_vs_globals")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_direct_vs_globals — {e}")
        GLOBAL_INT = 42
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nLOAD_GLOBAL_MODULE: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
