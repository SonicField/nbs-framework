"""
test_binary_subscr_correctness.py — Correctness tests for BINARY_SUBSCR specialisations.

Verifies that the JIT-compiled specialised paths (LoadArrayItem for list/tuple,
DictSubscr for dict) produce correct results for edge cases.

Tests cover:
  - Positive and negative indexing
  - Boundary indices (0, -1, len-1, -len)
  - Out-of-bounds IndexError
  - Dict KeyError on missing key
  - Large containers
  - Single-element containers
  - Empty containers (should raise)

Usage:
  python3 test_binary_subscr_correctness.py
"""

import sys

WARMUP = 15000  # CinderX auto-compilation typically needs 10000+ calls

# Set to True to require JIT compilation when cinderjit is available.
# When True, tests FAIL if the function is not compiled — avoids false
# confidence from interpreter-only execution.
REQUIRE_JIT = True


def check_jit_compiled(func, name):
    """Verify function is JIT-compiled.

    If REQUIRE_JIT is True and cinderjit is importable, raises AssertionError
    when the function is not compiled — the test is not exercising the JIT
    path it claims to test. If cinderjit is not available, always returns
    False (interpreter-only mode, tests still run for correctness baseline).
    """
    try:
        import cinderjit
        # Primary check (broken on AArch64, works on x86_64)
        if cinderjit.is_jit_compiled(func):
            return True
        # Fallback: check get_compiled_functions()
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
    print("=== BINARY_SUBSCR Correctness Tests ===")
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

    # ── Helper: warm up a function to trigger JIT compilation ──
    def warmup(fn, *args, n=WARMUP):
        for _ in range(n):
            try:
                fn(*args)
            except (IndexError, KeyError, TypeError):
                pass

    # ── Test 1: List positive index ──
    def list_pos_idx(lst, idx):
        return lst[idx]

    data = [10, 20, 30, 40, 50]
    warmup(list_pos_idx, data, 0)
    check_jit_compiled(list_pos_idx, "list_pos_idx")

    try:
        assert list_pos_idx(data, 0) == 10, f"got {list_pos_idx(data, 0)}"
        assert list_pos_idx(data, 2) == 30, f"got {list_pos_idx(data, 2)}"
        assert list_pos_idx(data, 4) == 50, f"got {list_pos_idx(data, 4)}"
        print("PASS  Test 1: list positive index")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: list positive index — {e}")
        failed += 1

    # ── Test 2: List negative index ──
    def list_neg_idx(lst, idx):
        return lst[idx]

    warmup(list_neg_idx, data, -1)
    check_jit_compiled(list_neg_idx, "list_neg_idx")

    try:
        assert list_neg_idx(data, -1) == 50, f"got {list_neg_idx(data, -1)}"
        assert list_neg_idx(data, -3) == 30, f"got {list_neg_idx(data, -3)}"
        assert list_neg_idx(data, -5) == 10, f"got {list_neg_idx(data, -5)}"
        print("PASS  Test 2: list negative index")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: list negative index — {e}")
        failed += 1

    # ── Test 3: List out-of-bounds ──
    def list_oob(lst, idx):
        return lst[idx]

    warmup(list_oob, data, 0)
    check_jit_compiled(list_oob, "list_oob")

    try:
        raised = False
        try:
            list_oob(data, 5)
        except IndexError:
            raised = True
        assert raised, "expected IndexError for index 5"

        raised = False
        try:
            list_oob(data, -6)
        except IndexError:
            raised = True
        assert raised, "expected IndexError for index -6"
        print("PASS  Test 3: list out-of-bounds raises IndexError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: list out-of-bounds — {e}")
        failed += 1

    # ── Test 4: Tuple positive and negative index ──
    def tuple_idx(tup, idx):
        return tup[idx]

    tdata = (100, 200, 300, 400, 500)
    warmup(tuple_idx, tdata, 0)
    check_jit_compiled(tuple_idx, "tuple_idx")

    try:
        assert tuple_idx(tdata, 0) == 100
        assert tuple_idx(tdata, 4) == 500
        assert tuple_idx(tdata, -1) == 500
        assert tuple_idx(tdata, -5) == 100
        print("PASS  Test 4: tuple positive and negative index")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: tuple index — {e}")
        failed += 1

    # ── Test 5: Tuple out-of-bounds ──
    def tuple_oob(tup, idx):
        return tup[idx]

    warmup(tuple_oob, tdata, 0)
    check_jit_compiled(tuple_oob, "tuple_oob")

    try:
        raised = False
        try:
            tuple_oob(tdata, 5)
        except IndexError:
            raised = True
        assert raised, "expected IndexError"

        raised = False
        try:
            tuple_oob(tdata, -6)
        except IndexError:
            raised = True
        assert raised, "expected IndexError for -6"
        print("PASS  Test 5: tuple out-of-bounds raises IndexError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: tuple out-of-bounds — {e}")
        failed += 1

    # ── Test 6: Dict existing key ──
    def dict_get(d, key):
        return d[key]

    ddata = {"a": 1, "b": 2, "c": 3, 42: "forty-two"}
    warmup(dict_get, ddata, "a")
    check_jit_compiled(dict_get, "dict_get")

    try:
        assert dict_get(ddata, "a") == 1
        assert dict_get(ddata, "c") == 3
        assert dict_get(ddata, 42) == "forty-two"
        print("PASS  Test 6: dict existing key")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: dict existing key — {e}")
        failed += 1

    # ── Test 7: Dict missing key raises KeyError ──
    def dict_miss(d, key):
        return d[key]

    warmup(dict_miss, ddata, "a")
    check_jit_compiled(dict_miss, "dict_miss")

    try:
        raised = False
        try:
            dict_miss(ddata, "z")
        except KeyError:
            raised = True
        assert raised, "expected KeyError"
        print("PASS  Test 7: dict missing key raises KeyError")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: dict missing key — {e}")
        failed += 1

    # ── Test 8: Single-element containers ──
    def single_get(container, idx):
        return container[idx]

    warmup(single_get, [99], 0)
    check_jit_compiled(single_get, "single_get")

    try:
        assert single_get([99], 0) == 99
        assert single_get([99], -1) == 99
        assert single_get((99,), 0) == 99
        assert single_get((99,), -1) == 99
        assert single_get({0: 99}, 0) == 99
        print("PASS  Test 8: single-element containers")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: single-element — {e}")
        failed += 1

    # ── Test 9: Empty container raises ──
    def empty_get(container, idx):
        return container[idx]

    warmup(empty_get, [1], 0)
    check_jit_compiled(empty_get, "empty_get")

    try:
        raised = False
        try:
            empty_get([], 0)
        except IndexError:
            raised = True
        assert raised, "expected IndexError on empty list"

        raised = False
        try:
            empty_get((), 0)
        except IndexError:
            raised = True
        assert raised, "expected IndexError on empty tuple"

        raised = False
        try:
            empty_get({}, "x")
        except KeyError:
            raised = True
        assert raised, "expected KeyError on empty dict"
        print("PASS  Test 9: empty containers raise correctly")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: empty containers — {e}")
        failed += 1

    # ── Test 10: Large list boundary ──
    def large_list_get(lst, idx):
        return lst[idx]

    big = list(range(10000))
    warmup(large_list_get, big, 0)
    check_jit_compiled(large_list_get, "large_list_get")

    try:
        assert large_list_get(big, 0) == 0
        assert large_list_get(big, 9999) == 9999
        assert large_list_get(big, -1) == 9999
        assert large_list_get(big, -10000) == 0
        assert large_list_get(big, 5000) == 5000
        print("PASS  Test 10: large list boundary indices")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: large list — {e}")
        failed += 1

    # ── Test 11: Subscript in loop accumulation ──
    def sum_subscr(lst, indices):
        total = 0
        for i in indices:
            total += lst[i]
        return total

    warmup(sum_subscr, [1, 2, 3], [0, 1, 2])
    check_jit_compiled(sum_subscr, "sum_subscr")

    try:
        assert sum_subscr([10, 20, 30], [0, 1, 2]) == 60
        assert sum_subscr([10, 20, 30], [2, 2, 2]) == 90
        assert sum_subscr([10, 20, 30], [-1, -2, -3]) == 60
        assert sum_subscr(list(range(100)), list(range(100))) == 4950
        print("PASS  Test 11: subscript in loop accumulation")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: loop accumulation — {e}")
        failed += 1

    # ── Test 12: Dict with int keys (matches BINARY_SUBSCR_DICT pattern) ──
    def dict_int_keys(d, keys):
        total = 0
        for k in keys:
            total += d[k]
        return total

    d = {i: i * 3 for i in range(100)}
    warmup(dict_int_keys, d, list(range(100)))
    check_jit_compiled(dict_int_keys, "dict_int_keys")

    try:
        assert dict_int_keys(d, [0, 1, 2]) == 9   # 0 + 3 + 6
        assert dict_int_keys(d, list(range(100))) == sum(i * 3 for i in range(100))
        print("PASS  Test 12: dict with int keys loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: dict int keys — {e}")
        failed += 1

    # ── Summary ──
    print()
    print(f"Results: {passed} passed, {failed} failed out of {passed + failed}")
    if failed > 0:
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
