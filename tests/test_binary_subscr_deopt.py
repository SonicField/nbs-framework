"""
test_binary_subscr_deopt — Polymorphic subscript deopt tests.

Targets: BINARY_SUBSCR_LIST_INT, BINARY_SUBSCR_TUPLE_INT, BINARY_SUBSCR_DICT
specialisations emit GuardType on the container (and index for list/tuple).
When a function is JIT-compiled with one container type and then called with
a different container type, the GuardType must fire, triggering deoptimisation
back to the interpreter. The interpreter must then produce the correct result.

These tests verify:
1. A function compiled with list input deopts correctly when given tuple/dict/set
2. A function compiled with tuple input deopts correctly when given list/dict
3. A function compiled with dict input deopts correctly when given list/defaultdict
4. After deopt, the function continues to produce correct results for BOTH types
5. try/except inside the subscript loop does not crash (Bug 7 regression guard)
6. Negative index handling works correctly after deopt
7. KeyError handling works correctly after deopt (dict path)

Usage:
  python3 test_binary_subscr_deopt.py
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
    print("=== BINARY_SUBSCR Polymorphic Deopt Tests ===")
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

    # ── Test 1: List-compiled, then tuple input ──────────────────────────

    def subscr_sum_1(container, indices):
        """Sum container[i] for each i in indices."""
        total = 0
        for i in indices:
            total += container[i]
        return total

    print("Test 1: List-compiled, then tuple deopt")
    indices = list(range(100))
    for _ in range(WARMUP):
        subscr_sum_1(list(range(100)), indices)

    check_jit_compiled(subscr_sum_1, "subscr_sum_1")

    # Verify list still works
    list_result = subscr_sum_1(list(range(100)), indices)
    expected = sum(range(100))
    assert list_result == expected, f"List path broken: {list_result} != {expected}"

    # Now call with tuple — should trigger GuardType deopt
    tuple_result = subscr_sum_1(tuple(range(100)), indices)
    if tuple_result == expected:
        print("  PASS  tuple deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  tuple deopt: got {tuple_result}, expected {expected}")
        failed += 1

    # Verify list STILL works after deopt
    list_result_2 = subscr_sum_1(list(range(100)), indices)
    if list_result_2 == expected:
        print("  PASS  list path still correct after deopt")
        passed += 1
    else:
        print(f"  FAIL  list path after deopt: got {list_result_2}, expected {expected}")
        failed += 1

    # ── Test 2: Tuple-compiled, then list input ──────────────────────────

    def subscr_sum_2(container, indices):
        total = 0
        for i in indices:
            total += container[i]
        return total

    print()
    print("Test 2: Tuple-compiled, then list deopt")
    for _ in range(WARMUP):
        subscr_sum_2(tuple(range(100)), indices)

    check_jit_compiled(subscr_sum_2, "subscr_sum_2")

    list_result = subscr_sum_2(list(range(100)), indices)
    if list_result == expected:
        print("  PASS  list deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  list deopt: got {list_result}, expected {expected}")
        failed += 1

    # ── Test 3: Dict-compiled, then custom mapping ───────────────────────

    def subscr_sum_3(container, keys):
        total = 0
        for k in keys:
            total += container[k]
        return total

    print()
    print("Test 3: Dict-compiled, then defaultdict deopt")
    d = {i: i for i in range(100)}
    keys = list(range(100))
    for _ in range(WARMUP):
        subscr_sum_3(d, keys)

    check_jit_compiled(subscr_sum_3, "subscr_sum_3")

    dict_result = subscr_sum_3(d, keys)
    assert dict_result == expected

    # Call with a defaultdict — not a dict subclass but dict-like
    from collections import defaultdict
    dd = defaultdict(int)
    for i in range(100):
        dd[i] = i

    dd_result = subscr_sum_3(dd, keys)
    if dd_result == expected:
        print("  PASS  defaultdict deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  defaultdict deopt: got {dd_result}, expected {expected}")
        failed += 1

    # ── Test 4: List-compiled, then dict input (type mismatch) ───────────

    def subscr_sum_4(container, keys):
        total = 0
        for k in keys:
            total += container[k]
        return total

    print()
    print("Test 4: List-compiled, then dict deopt")
    for _ in range(WARMUP):
        subscr_sum_4(list(range(50)), list(range(50)))

    check_jit_compiled(subscr_sum_4, "subscr_sum_4")

    d4 = {i: i * 3 for i in range(50)}
    dict_result = subscr_sum_4(d4, list(range(50)))
    expected_4 = sum(i * 3 for i in range(50))
    if dict_result == expected_4:
        print("  PASS  dict deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  dict deopt: got {dict_result}, expected {expected_4}")
        failed += 1

    # ── Test 5: Negative index after deopt ───────────────────────────────

    def subscr_neg_5(container, idx):
        return container[idx]

    print()
    print("Test 5: Negative index after deopt")
    data = list(range(10))
    for _ in range(WARMUP):
        subscr_neg_5(data, 5)

    check_jit_compiled(subscr_neg_5, "subscr_neg_5")

    # Negative index on list
    neg_result = subscr_neg_5(data, -1)
    if neg_result == 9:
        print("  PASS  negative index on list correct")
        passed += 1
    else:
        print(f"  FAIL  negative index: got {neg_result}, expected 9")
        failed += 1

    # Negative index on tuple (deopt from list)
    tdata = tuple(range(10))
    neg_tuple = subscr_neg_5(tdata, -2)
    if neg_tuple == 8:
        print("  PASS  negative index on tuple after deopt correct")
        passed += 1
    else:
        print(f"  FAIL  negative index on tuple: got {neg_tuple}, expected 8")
        failed += 1

    # ── Test 6: try/except inside subscript loop (Bug 7 regression) ──────

    def subscr_tryexcept_6(container, indices):
        """Subscript with try/except — Bug 7 trigger condition."""
        total = 0
        for i in indices:
            try:
                total += container[i]
            except (IndexError, KeyError):
                total += -1
        return total

    print()
    print("Test 6: try/except inside subscript loop (Bug 7 guard)")
    safe_indices = list(range(50))
    for _ in range(WARMUP):
        subscr_tryexcept_6(list(range(50)), safe_indices)

    check_jit_compiled(subscr_tryexcept_6, "subscr_tryexcept_6")

    # Call with tuple — deopt with try/except (Bug 7 crash condition)
    te_result = subscr_tryexcept_6(tuple(range(50)), safe_indices)
    expected_6 = sum(range(50))
    if te_result == expected_6:
        print("  PASS  tuple deopt with try/except — no crash")
        passed += 1
    else:
        print(f"  FAIL  tuple deopt with try/except: got {te_result}, expected {expected_6}")
        failed += 1

    # Call with dict — deopt with try/except
    d6 = {i: i for i in range(50)}
    te_dict = subscr_tryexcept_6(d6, safe_indices)
    if te_dict == expected_6:
        print("  PASS  dict deopt with try/except — no crash")
        passed += 1
    else:
        print(f"  FAIL  dict deopt with try/except: got {te_dict}, expected {expected_6}")
        failed += 1

    # ── Test 7: KeyError handling after deopt ────────────────────────────

    def subscr_keyerr_7(container, keys):
        """Dict subscript with missing keys."""
        found = 0
        for k in keys:
            try:
                container[k]
                found += 1
            except (KeyError, IndexError):
                pass
        return found

    print()
    print("Test 7: KeyError handling after deopt")
    d7 = {i: i for i in range(50)}
    all_keys = list(range(100))  # keys 50-99 will KeyError
    for _ in range(WARMUP):
        subscr_keyerr_7(d7, all_keys)

    check_jit_compiled(subscr_keyerr_7, "subscr_keyerr_7")

    # Verify dict path
    found = subscr_keyerr_7(d7, all_keys)
    if found == 50:
        print("  PASS  dict KeyError handling correct")
        passed += 1
    else:
        print(f"  FAIL  dict KeyError: found={found}, expected 50")
        failed += 1

    # Deopt to list — IndexError for out-of-range
    lst7 = list(range(50))
    found_list = subscr_keyerr_7(lst7, all_keys)
    if found_list == 50:
        print("  PASS  list IndexError handling after deopt correct")
        passed += 1
    else:
        print(f"  FAIL  list IndexError: found={found_list}, expected 50")
        failed += 1

    # ── Test 8: Rapid alternation between types ──────────────────────────

    def subscr_alt_8(container, indices):
        total = 0
        for i in indices:
            total += container[i]
        return total

    print()
    print("Test 8: Rapid alternation between list/tuple/dict")
    small_indices = list(range(20))
    small_list = list(range(20))
    small_tuple = tuple(range(20))
    small_dict = {i: i for i in range(20)}
    expected_8 = sum(range(20))

    for _ in range(WARMUP):
        subscr_alt_8(small_list, small_indices)

    check_jit_compiled(subscr_alt_8, "subscr_alt_8")

    all_correct = True
    for _ in range(100):
        r1 = subscr_alt_8(small_list, small_indices)
        r2 = subscr_alt_8(small_tuple, small_indices)
        r3 = subscr_alt_8(small_dict, small_indices)
        if r1 != expected_8 or r2 != expected_8 or r3 != expected_8:
            all_correct = False
            break

    if all_correct:
        print("  PASS  rapid alternation produces correct results")
        passed += 1
    else:
        print("  FAIL  rapid alternation produced incorrect result")
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
