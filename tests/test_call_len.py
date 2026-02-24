#!/usr/bin/env python3
"""
test_call_len.py — Correctness and deopt tests for CALL_LEN specialisation.

Targets: CALL_LEN.

CALL_LEN specialises len(obj) calls. CPython's adaptive interpreter replaces
the generic CALL opcode with CALL_LEN when it detects repeated calls to the
len() builtin function.

The CinderX JIT then emits an optimised length query (GuardIs on the builtin
len function, then direct sq_length / mp_length slot call) instead of a full
CALL through the C calling convention.

Mechanism:
1. Adaptive interpreter detects CALL to len (builtin)
2. Replaces CALL with CALL_LEN
3. CinderX JIT emits GuardIs(len_func) + direct slot call
4. Direct tp_as_sequence->sq_length or tp_as_mapping->mp_length

Deopt triggers:
  - len() builtin is shadowed or replaced
  - Object has custom __len__ (still works but through descriptor call)
  - Object does not support len() (TypeError)

Tests cover:
  - Basic len(list)
  - Basic len(str)
  - Basic len(tuple)
  - Basic len(dict)
  - Basic len(set)
  - Basic len(bytes)
  - len of empty containers
  - len of large containers
  - len with custom __len__
  - len with custom __len__ returning large value
  - Deopt: len shadowed by local function
  - len after mutation (append/pop/insert)
  - len in conditional branch (if len(...) > N)
  - Rapid len checks (1000 cycles)
  - Stability — 10000 len calls
  - len of range object
  - len of bytearray
  - len result used in arithmetic
  - len of nested containers (outer len only)
  - Equivalence: len(x) vs x.__len__()

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct len result when JIT-compiled (warmup -> JIT -> check)
  2. Result matches interpreter semantics exactly
  3. Edge cases (empty, large, custom __len__) handled correctly

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_len.py
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

    passed = 0
    failed = 0

    # ------------------------------------------------------------------
    # Test 1: Basic len(list)
    # ------------------------------------------------------------------
    try:
        def len_list(obj):
            return len(obj)

        data = [1, 2, 3, 4, 5]
        for _ in range(WARMUP):
            len_list(data)
        check_jit_compiled(len_list, "len_list")

        assert len_list([]) == 0
        assert len_list([1]) == 1
        assert len_list([1, 2, 3]) == 3
        assert len_list(list(range(100))) == 100
        print("  PASS: test_len_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_list — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Basic len(str)
    # ------------------------------------------------------------------
    try:
        def len_str(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_str("hello")
        check_jit_compiled(len_str, "len_str")

        assert len_str("") == 0
        assert len_str("a") == 1
        assert len_str("hello") == 5
        assert len_str("hello world") == 11
        assert len_str("\n\t") == 2
        print("  PASS: test_len_str")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_str — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Basic len(tuple)
    # ------------------------------------------------------------------
    try:
        def len_tuple(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_tuple((1, 2, 3))
        check_jit_compiled(len_tuple, "len_tuple")

        assert len_tuple(()) == 0
        assert len_tuple((1,)) == 1
        assert len_tuple((1, 2, 3)) == 3
        assert len_tuple(tuple(range(50))) == 50
        print("  PASS: test_len_tuple")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_tuple — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Basic len(dict)
    # ------------------------------------------------------------------
    try:
        def len_dict(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_dict({"a": 1, "b": 2})
        check_jit_compiled(len_dict, "len_dict")

        assert len_dict({}) == 0
        assert len_dict({"a": 1}) == 1
        assert len_dict({"a": 1, "b": 2, "c": 3}) == 3
        assert len_dict({i: i for i in range(100)}) == 100
        print("  PASS: test_len_dict")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_dict — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Basic len(set)
    # ------------------------------------------------------------------
    try:
        def len_set(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_set({1, 2, 3})
        check_jit_compiled(len_set, "len_set")

        assert len_set(set()) == 0
        assert len_set({1}) == 1
        assert len_set({1, 2, 3}) == 3
        assert len_set(set(range(100))) == 100
        # Duplicates collapse
        assert len_set({1, 1, 1, 2, 2}) == 2
        print("  PASS: test_len_set")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_set — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Basic len(bytes)
    # ------------------------------------------------------------------
    try:
        def len_bytes(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_bytes(b"hello")
        check_jit_compiled(len_bytes, "len_bytes")

        assert len_bytes(b"") == 0
        assert len_bytes(b"\x00") == 1
        assert len_bytes(b"hello") == 5
        assert len_bytes(bytes(range(256))) == 256
        print("  PASS: test_len_bytes")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_bytes — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: len of empty containers
    # ------------------------------------------------------------------
    try:
        def len_any(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_any([])
        check_jit_compiled(len_any, "len_any")

        assert len_any([]) == 0
        assert len_any(()) == 0
        assert len_any("") == 0
        assert len_any({}) == 0
        assert len_any(set()) == 0
        assert len_any(b"") == 0
        assert len_any(bytearray()) == 0
        print("  PASS: test_len_empty_containers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_empty_containers — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: len of large containers
    # ------------------------------------------------------------------
    try:
        def len_large(obj):
            return len(obj)

        big_list = list(range(100000))
        for _ in range(WARMUP):
            len_large(big_list)
        check_jit_compiled(len_large, "len_large")

        assert len_large(big_list) == 100000
        assert len_large("a" * 50000) == 50000
        assert len_large(tuple(range(10000))) == 10000
        print("  PASS: test_len_large_containers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_large_containers — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: len with custom __len__
    # ------------------------------------------------------------------
    try:
        class FixedLen:
            def __len__(self):
                return 42

        def len_custom(obj):
            return len(obj)

        fl = FixedLen()
        for _ in range(WARMUP):
            len_custom(fl)
        check_jit_compiled(len_custom, "len_custom")

        assert len_custom(FixedLen()) == 42

        class DynamicLen:
            def __init__(self, n):
                self.n = n
            def __len__(self):
                return self.n

        assert len_custom(DynamicLen(0)) == 0
        assert len_custom(DynamicLen(100)) == 100
        assert len_custom(DynamicLen(999)) == 999
        print("  PASS: test_len_custom_dunder_len")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_custom_dunder_len — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: len with custom __len__ returning large value
    # ------------------------------------------------------------------
    try:
        class HugeLen:
            def __len__(self):
                return 2**31 - 1  # max 32-bit signed

        class BigLen:
            def __len__(self):
                return 10**9

        def len_huge(obj):
            return len(obj)

        hl = HugeLen()
        for _ in range(WARMUP):
            len_huge(hl)
        check_jit_compiled(len_huge, "len_huge")

        assert len_huge(HugeLen()) == 2**31 - 1
        assert len_huge(BigLen()) == 10**9
        print("  PASS: test_len_custom_large_value")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_custom_large_value — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — len shadowed by local function
    # ------------------------------------------------------------------
    try:
        def check_len_normal(obj):
            return len(obj)

        for _ in range(WARMUP):
            check_len_normal([1, 2, 3])
        check_jit_compiled(check_len_normal, "check_len_normal")

        assert check_len_normal([1, 2, 3]) == 3

        # Shadow len in a different function's scope
        def check_with_shadow(obj):
            def len(o):  # noqa: F811
                return -1
            return len(obj)

        assert check_with_shadow([1, 2, 3]) == -1

        # Original function should still work correctly
        assert check_len_normal([1, 2, 3]) == 3
        assert check_len_normal("abc") == 3
        print("  PASS: test_deopt_len_shadowed")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_len_shadowed — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: len after mutation (append/pop/insert)
    # ------------------------------------------------------------------
    try:
        def len_mutating(obj):
            return len(obj)

        data = [1, 2, 3]
        for _ in range(WARMUP):
            len_mutating(data)
        check_jit_compiled(len_mutating, "len_mutating")

        assert len_mutating(data) == 3

        data.append(4)
        assert len_mutating(data) == 4

        data.append(5)
        assert len_mutating(data) == 5

        data.pop()
        assert len_mutating(data) == 4

        data.pop()
        data.pop()
        assert len_mutating(data) == 2

        data.insert(0, 99)
        assert len_mutating(data) == 3

        data.clear()
        assert len_mutating(data) == 0
        print("  PASS: test_len_after_mutation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_after_mutation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: len in conditional branch
    # ------------------------------------------------------------------
    try:
        def classify_length(obj):
            if len(obj) == 0:
                return "empty"
            elif len(obj) < 5:
                return "short"
            elif len(obj) < 20:
                return "medium"
            else:
                return "long"

        for _ in range(WARMUP):
            classify_length([1, 2, 3])
        check_jit_compiled(classify_length, "classify_length")

        assert classify_length([]) == "empty"
        assert classify_length([1]) == "short"
        assert classify_length([1, 2, 3, 4]) == "short"
        assert classify_length(list(range(5))) == "medium"
        assert classify_length(list(range(19))) == "medium"
        assert classify_length(list(range(20))) == "long"
        assert classify_length(list(range(100))) == "long"
        print("  PASS: test_len_in_conditional")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_in_conditional — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Rapid len checks (1000 cycles)
    # ------------------------------------------------------------------
    try:
        def rapid_len(obj):
            return len(obj)

        data = [1, 2, 3]
        for _ in range(WARMUP):
            rapid_len(data)
        check_jit_compiled(rapid_len, "rapid_len")

        for i in range(1000):
            result = rapid_len(data)
            assert result == 3, f"cycle {i}: expected 3, got {result}"
        print("  PASS: test_rapid_len_checks")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_len_checks — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Stability — 10000 len calls
    # ------------------------------------------------------------------
    try:
        def stable_len(obj):
            return len(obj)

        data = "hello world"
        for _ in range(WARMUP):
            stable_len(data)
        check_jit_compiled(stable_len, "stable_len")

        for i in range(10000):
            result = stable_len(data)
            assert result == 11, f"iteration {i}: got {result}, expected 11"
        print("  PASS: test_stability_10000_len")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_len — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: len of range object
    # ------------------------------------------------------------------
    try:
        def len_range(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_range(range(10))
        check_jit_compiled(len_range, "len_range")

        assert len_range(range(0)) == 0
        assert len_range(range(1)) == 1
        assert len_range(range(100)) == 100
        assert len_range(range(0, 10, 2)) == 5     # 0,2,4,6,8
        assert len_range(range(0, 10, 3)) == 4     # 0,3,6,9
        assert len_range(range(10, 0, -1)) == 10   # 10,9,...,1
        assert len_range(range(10, 0, -3)) == 4    # 10,7,4,1
        print("  PASS: test_len_range")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_range — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: len of bytearray
    # ------------------------------------------------------------------
    try:
        def len_bytearray(obj):
            return len(obj)

        ba = bytearray(b"hello")
        for _ in range(WARMUP):
            len_bytearray(ba)
        check_jit_compiled(len_bytearray, "len_bytearray")

        assert len_bytearray(bytearray()) == 0
        assert len_bytearray(bytearray(b"a")) == 1
        assert len_bytearray(bytearray(b"hello")) == 5

        # Mutate and re-check
        ba2 = bytearray(b"abc")
        assert len_bytearray(ba2) == 3
        ba2.append(0x64)
        assert len_bytearray(ba2) == 4
        ba2.pop()
        ba2.pop()
        assert len_bytearray(ba2) == 2
        print("  PASS: test_len_bytearray")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_bytearray — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: len result used in arithmetic
    # ------------------------------------------------------------------
    try:
        def double_len(obj):
            return len(obj) * 2

        def len_sum(a, b):
            return len(a) + len(b)

        def len_diff(a, b):
            return len(a) - len(b)

        for _ in range(WARMUP):
            double_len([1, 2, 3])
            len_sum([1, 2], [3, 4, 5])
            len_diff([1, 2, 3, 4], [1])
        check_jit_compiled(double_len, "double_len")
        check_jit_compiled(len_sum, "len_sum")
        check_jit_compiled(len_diff, "len_diff")

        assert double_len([1, 2, 3]) == 6
        assert double_len([]) == 0
        assert double_len("hi") == 4

        assert len_sum([1, 2], [3, 4, 5]) == 5
        assert len_sum([], []) == 0

        assert len_diff([1, 2, 3, 4], [1]) == 3
        assert len_diff([], [1, 2]) == -2
        print("  PASS: test_len_in_arithmetic")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_in_arithmetic — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: len of nested containers (outer len only)
    # ------------------------------------------------------------------
    try:
        def len_outer(obj):
            return len(obj)

        for _ in range(WARMUP):
            len_outer([[1, 2], [3, 4, 5]])
        check_jit_compiled(len_outer, "len_outer")

        # len counts top-level elements only
        assert len_outer([[1, 2], [3, 4, 5]]) == 2
        assert len_outer([[], [], [], []]) == 4
        assert len_outer({"a": [1, 2, 3], "b": [4, 5]}) == 2
        assert len_outer(([1, 2], [3, 4], [5])) == 3

        # Deeply nested — still just top-level
        nested = [[[[[1]]]]]
        assert len_outer(nested) == 1
        print("  PASS: test_len_nested_containers")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_nested_containers — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — len(x) vs x.__len__()
    # ------------------------------------------------------------------
    try:
        def len_builtin(obj):
            return len(obj)

        def len_dunder(obj):
            return obj.__len__()

        for _ in range(WARMUP):
            len_builtin([1, 2, 3])
            len_dunder([1, 2, 3])
        check_jit_compiled(len_builtin, "len_builtin")
        check_jit_compiled(len_dunder, "len_dunder")

        test_cases = [
            [],
            [1, 2, 3],
            "hello",
            "",
            (1, 2),
            (),
            {"a": 1, "b": 2},
            {},
            {1, 2, 3},
            b"bytes",
            bytearray(b"ba"),
            range(10),
        ]

        for obj in test_cases:
            r_builtin = len_builtin(obj)
            r_dunder = len_dunder(obj)
            assert r_builtin == r_dunder, (
                f"Mismatch for {type(obj).__name__}: "
                f"len()={r_builtin}, __len__()={r_dunder}"
            )
        print("  PASS: test_equivalence_len_vs_dunder_len")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_len_vs_dunder_len — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_LEN: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
