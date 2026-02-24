#!/usr/bin/env python3
"""
test_call_no_kw_list_append.py — Correctness and deopt tests for CALL_NO_KW_LIST_APPEND specialisation.

Targets: CALL_NO_KW_LIST_APPEND.

CALL_NO_KW_LIST_APPEND is the CPython 3.12 specialisation for list.append()
calls. When the adaptive interpreter detects repeated calls to the append
method on a list object with exactly one argument and no keyword arguments,
it specialises the call to bypass generic method dispatch.

The specialisation directly invokes the C-level list append without going
through the full CALL machinery — no tp_call, no bound method creation,
no argument tuple packing.

The CinderX JIT compiles CALL_NO_KW_LIST_APPEND by emitting a GuardType
on the list object, then calling the internal list append path directly.

Deopt triggers:
  - Receiver is not a list (type changes)
  - append method is shadowed on the instance or subclass
  - list.append is replaced in the list type's dict

Tests cover:
  - Simple list.append() with int
  - Append str, float, None, bool
  - Append to empty list
  - Append complex objects (dicts, lists, tuples)
  - Append in a loop (accumulation)
  - Append preserves identity (appended object is the same)
  - Append returns None
  - Multiple appends in sequence
  - Deopt: append method shadowed on subclass
  - Deopt: receiver changes from list to subclass
  - Rapid appends (1000 iterations)
  - Stability — 10000 appends
  - Append nested structures
  - Append self (circular reference)
  - Append after clear
  - list subclass with overridden append
  - Append heterogeneous types in sequence
  - Append large number of items
  - Equivalence: append vs +=
  - Append preserves list order

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_no_kw_list_append.py
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

    print("=== CALL_NO_KW_LIST_APPEND Correctness & Deopt Tests ===")
    print()

    passed = 0
    failed = 0

    # ── Test 1: Simple list.append() with int ───────────────────────────

    def do_append_int(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_int(warmup_list, i)
        check_jit_compiled(do_append_int, "do_append_int")

        test_list = [1, 2, 3]
        do_append_int(test_list, 42)
        assert test_list == [1, 2, 3, 42], f"Expected [1,2,3,42], got {test_list}"
        print("  PASS: test_append_int")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_int — {e}")
        failed += 1

    # ── Test 2: Append str, float, None, bool ───────────────────────────

    def do_append_val(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_val(warmup_list, "x")
        check_jit_compiled(do_append_val, "do_append_val")

        test_list = []
        do_append_val(test_list, "hello")
        do_append_val(test_list, 3.14)
        do_append_val(test_list, None)
        do_append_val(test_list, True)
        assert test_list == ["hello", 3.14, None, True], f"Got {test_list}"
        print("  PASS: test_append_various_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_various_types — {e}")
        failed += 1

    # ── Test 3: Append to empty list ────────────────────────────────────

    def do_append_empty(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_empty(warmup_list, i)
        check_jit_compiled(do_append_empty, "do_append_empty")

        test_list = []
        do_append_empty(test_list, "first")
        assert test_list == ["first"], f"Expected ['first'], got {test_list}"
        assert len(test_list) == 1
        print("  PASS: test_append_to_empty")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_to_empty — {e}")
        failed += 1

    # ── Test 4: Append complex objects ──────────────────────────────────

    def do_append_obj(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_obj(warmup_list, {})
        check_jit_compiled(do_append_obj, "do_append_obj")

        test_list = []
        d = {"a": 1}
        inner_list = [10, 20]
        t = (1, 2, 3)
        do_append_obj(test_list, d)
        do_append_obj(test_list, inner_list)
        do_append_obj(test_list, t)
        assert test_list == [{"a": 1}, [10, 20], (1, 2, 3)], f"Got {test_list}"
        print("  PASS: test_append_complex_objects")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_complex_objects — {e}")
        failed += 1

    # ── Test 5: Append in a loop (accumulation) ────────────────────────

    def do_append_loop(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_loop(warmup_list, i)
        check_jit_compiled(do_append_loop, "do_append_loop")

        result = []
        for i in range(100):
            do_append_loop(result, i * 2)
        assert len(result) == 100, f"Expected 100 items, got {len(result)}"
        assert result[0] == 0, f"First element should be 0"
        assert result[99] == 198, f"Last element should be 198"
        assert result == [i * 2 for i in range(100)]
        print("  PASS: test_append_accumulation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_accumulation — {e}")
        failed += 1

    # ── Test 6: Append preserves identity ───────────────────────────────

    def do_append_identity(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_identity(warmup_list, None)
        check_jit_compiled(do_append_identity, "do_append_identity")

        sentinel = object()
        test_list = []
        do_append_identity(test_list, sentinel)
        assert test_list[0] is sentinel, "Appended object should be the same identity"
        print("  PASS: test_append_preserves_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_preserves_identity — {e}")
        failed += 1

    # ── Test 7: Append returns None ─────────────────────────────────────

    def do_append_retval(lst, val):
        return lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_retval(warmup_list, i)
        check_jit_compiled(do_append_retval, "do_append_retval")

        test_list = []
        result = do_append_retval(test_list, 42)
        assert result is None, f"append() should return None, got {result!r}"
        assert test_list == [42]
        print("  PASS: test_append_returns_none")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_returns_none — {e}")
        failed += 1

    # ── Test 8: Multiple appends in sequence ────────────────────────────

    def do_multi_append(lst, a, b, c):
        lst.append(a)
        lst.append(b)
        lst.append(c)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_multi_append(warmup_list, 1, 2, 3)
        check_jit_compiled(do_multi_append, "do_multi_append")

        test_list = []
        do_multi_append(test_list, "x", "y", "z")
        assert test_list == ["x", "y", "z"], f"Got {test_list}"
        print("  PASS: test_multiple_appends")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_appends — {e}")
        failed += 1

    # ── Test 9: Deopt — append method shadowed on subclass ──────────────

    class MyList(list):
        def append(self, val):
            super().append(val * 2)

    def do_append_deopt(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_deopt(warmup_list, i)
        check_jit_compiled(do_append_deopt, "do_append_deopt")

        # Normal list works correctly
        normal = []
        do_append_deopt(normal, 5)
        assert normal == [5], f"Normal list: expected [5], got {normal}"

        # Subclass with overridden append should deopt and use the override
        custom = MyList()
        do_append_deopt(custom, 5)
        assert custom == [10], f"MyList: expected [10] (5*2), got {list(custom)}"
        print("  PASS: test_deopt_subclass_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_subclass_append — {e}")
        failed += 1

    # ── Test 10: Deopt — receiver changes from list to subclass ─────────

    class TrackedList(list):
        def __init__(self):
            super().__init__()
            self.append_count = 0

        def append(self, val):
            self.append_count += 1
            super().append(val)

    def do_append_switch(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_switch(warmup_list, i)
        check_jit_compiled(do_append_switch, "do_append_switch")

        # After warmup on plain list, switch to TrackedList
        tracked = TrackedList()
        do_append_switch(tracked, "a")
        do_append_switch(tracked, "b")
        assert list(tracked) == ["a", "b"], f"Got {list(tracked)}"
        assert tracked.append_count == 2, (
            f"Expected 2 tracked appends, got {tracked.append_count}"
        )
        print("  PASS: test_deopt_receiver_type_change")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_receiver_type_change — {e}")
        failed += 1

    # ── Test 11: Rapid appends (1000 iterations) ───────────────────────

    def do_append_rapid(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_rapid(warmup_list, i)
        check_jit_compiled(do_append_rapid, "do_append_rapid")

        result = []
        for i in range(1000):
            do_append_rapid(result, i)
        assert len(result) == 1000
        assert result == list(range(1000))
        print("  PASS: test_rapid_appends")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_appends — {e}")
        failed += 1

    # ── Test 12: Stability — 10000 appends ──────────────────────────────

    def do_append_stable(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_stable(warmup_list, i)
        check_jit_compiled(do_append_stable, "do_append_stable")

        result = []
        for i in range(10000):
            do_append_stable(result, i)
        assert len(result) == 10000
        assert result[0] == 0
        assert result[9999] == 9999
        print("  PASS: test_stability_10000_appends")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_appends — {e}")
        failed += 1

    # ── Test 13: Append nested structures ───────────────────────────────

    def do_append_nested(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_nested(warmup_list, [])
        check_jit_compiled(do_append_nested, "do_append_nested")

        result = []
        do_append_nested(result, {"key": [1, 2, {"inner": True}]})
        assert result[0] == {"key": [1, 2, {"inner": True}]}
        assert result[0]["key"][2]["inner"] is True
        print("  PASS: test_append_nested_structures")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_nested_structures — {e}")
        failed += 1

    # ── Test 14: Append self (circular reference) ───────────────────────

    def do_append_self(lst):
        lst.append(lst)

    try:
        warmup_list = []
        for _ in range(WARMUP):
            wl = [1]
            do_append_self(wl)
        check_jit_compiled(do_append_self, "do_append_self")

        test_list = [1, 2]
        do_append_self(test_list)
        assert len(test_list) == 3
        assert test_list[2] is test_list, "Last element should be the list itself"
        assert test_list[0] == 1
        assert test_list[1] == 2
        print("  PASS: test_append_self_circular")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_self_circular — {e}")
        failed += 1

    # ── Test 15: Append after clear ─────────────────────────────────────

    def do_append_after_clear(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_after_clear(warmup_list, i)
        check_jit_compiled(do_append_after_clear, "do_append_after_clear")

        test_list = [1, 2, 3, 4, 5]
        test_list.clear()
        assert test_list == []
        do_append_after_clear(test_list, 99)
        assert test_list == [99], f"Expected [99], got {test_list}"
        print("  PASS: test_append_after_clear")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_after_clear — {e}")
        failed += 1

    # ── Test 16: list subclass with overridden append (validation) ──────

    class ValidatedList(list):
        def append(self, val):
            if not isinstance(val, int):
                raise TypeError(f"Only ints allowed, got {type(val).__name__}")
            super().append(val)

    def do_append_validated(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_validated(warmup_list, i)
        check_jit_compiled(do_append_validated, "do_append_validated")

        vl = ValidatedList()
        do_append_validated(vl, 42)
        assert list(vl) == [42]

        got_error = False
        try:
            do_append_validated(vl, "not_an_int")
        except TypeError:
            got_error = True
        assert got_error, "ValidatedList should reject non-int"
        assert list(vl) == [42], "List should be unchanged after rejected append"
        print("  PASS: test_subclass_validated_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_subclass_validated_append — {e}")
        failed += 1

    # ── Test 17: Append heterogeneous types in sequence ─────────────────

    def do_append_hetero(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_hetero(warmup_list, i)
        check_jit_compiled(do_append_hetero, "do_append_hetero")

        result = []
        items = [1, "two", 3.0, None, True, [4], {"five": 5}, (6,), b"seven", {8}]
        for item in items:
            do_append_hetero(result, item)
        assert result == items, f"Got {result}"
        print("  PASS: test_append_heterogeneous")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_heterogeneous — {e}")
        failed += 1

    # ── Test 18: Append large number of items ───────────────────────────

    def do_append_large(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_large(warmup_list, i)
        check_jit_compiled(do_append_large, "do_append_large")

        big_list = []
        n = 50000
        for i in range(n):
            do_append_large(big_list, i)
        assert len(big_list) == n, f"Expected {n}, got {len(big_list)}"
        assert big_list[0] == 0
        assert big_list[n - 1] == n - 1
        # Spot check some middle values
        assert big_list[25000] == 25000
        print("  PASS: test_append_large_count")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_large_count — {e}")
        failed += 1

    # ── Test 19: Equivalence — append vs += [val] ───────────────────────

    def do_append_equiv(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_equiv(warmup_list, i)
        check_jit_compiled(do_append_equiv, "do_append_equiv")

        list_a = []
        list_b = []
        test_values = [1, "x", None, 3.14, True, [], {}]
        for v in test_values:
            do_append_equiv(list_a, v)
            list_b += [v]
        assert list_a == list_b, f"append: {list_a} != +=: {list_b}"
        print("  PASS: test_equivalence_append_vs_iadd")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_append_vs_iadd — {e}")
        failed += 1

    # ── Test 20: Append preserves list order ────────────────────────────

    def do_append_order(lst, val):
        lst.append(val)

    try:
        warmup_list = []
        for i in range(WARMUP):
            do_append_order(warmup_list, i)
        check_jit_compiled(do_append_order, "do_append_order")

        result = []
        for i in range(200):
            do_append_order(result, i)
        # Verify strict ordering
        for i in range(200):
            assert result[i] == i, f"Order violated at index {i}: got {result[i]}"
        # Verify append always goes to the end
        do_append_order(result, "end")
        assert result[-1] == "end", f"Last element should be 'end', got {result[-1]}"
        assert result[-2] == 199
        print("  PASS: test_append_preserves_order")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_append_preserves_order — {e}")
        failed += 1

    # ── Summary ─────────────────────────────────────────────────────────

    print()
    print(f"CALL_NO_KW_LIST_APPEND: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
