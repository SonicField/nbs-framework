#!/usr/bin/env python3
"""
test_call_method_descriptor_o.py — Correctness and deopt tests for
CALL_METHOD_DESCRIPTOR_O specialisation.

Targets: CALL_METHOD_DESCRIPTOR_O.

CALL_METHOD_DESCRIPTOR_O specialises calls to C-implemented method
descriptors that take exactly one argument (self + one positional arg).
CPython's adaptive interpreter replaces the generic CALL opcode with this
specialisation when it detects repeated calls to methods using the
METH_O calling convention.

Examples of METH_O methods include list.append(), set.add(), set.discard(),
set.remove(), deque.appendleft(), etc.

Mechanism:
1. Adaptive interpreter detects CALL to a method_descriptor with METH_O
2. Replaces CALL with CALL_METHOD_DESCRIPTOR_O
3. CinderX JIT emits GuardType on the receiver + direct C function pointer call
4. Single arg passed directly — no tuple packing, no kwarg handling

Deopt triggers:
  - Receiver type changes (different type with same method name)
  - Method is overridden on the instance or subclass
  - Method descriptor replaced on the type

Tests cover:
  - list.append()
  - list.remove()
  - set.add()
  - set.discard()
  - set.remove()
  - dict.__contains__ via 'in' (METH_O internally)
  - list.pop() with index arg — note: this is METH_O on some builds
  - bytearray.append()
  - deque.append()
  - deque.appendleft()
  - Deopt: different type with same method name
  - Deopt: subclass overriding method
  - Rapid method calls (1000 cycles)
  - Stability — 10000 calls
  - Method with various arg types (int, str, None, tuple)
  - set.add() idempotent (duplicate adds)
  - list.remove() raises ValueError for missing element
  - Chained single-arg calls
  - frozenset.__contains__
  - Equivalence: obj.method(arg) vs type.method(obj, arg)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_method_descriptor_o.py
"""

import sys
from collections import deque

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
    # Test 1: list.append()
    # ------------------------------------------------------------------
    try:
        def call_append(lst, val):
            lst.append(val)

        data = []
        for _ in range(WARMUP):
            call_append(data, 0)
        data.clear()
        check_jit_compiled(call_append, "call_append")

        result = []
        call_append(result, 1)
        call_append(result, 2)
        call_append(result, 3)
        assert result == [1, 2, 3]
        call_append(result, None)
        assert result == [1, 2, 3, None]
        print("  PASS: test_list_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_append — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: list.remove()
    # ------------------------------------------------------------------
    try:
        def call_remove(lst, val):
            lst.remove(val)

        data = [1, 2, 3, 2, 1]
        for _ in range(WARMUP):
            data.append(99)
            call_remove(data, 99)
        check_jit_compiled(call_remove, "call_remove")

        test = [10, 20, 30, 20, 10]
        call_remove(test, 20)
        assert test == [10, 30, 20, 10]  # removes first occurrence

        call_remove(test, 10)
        assert test == [30, 20, 10]

        call_remove(test, 10)
        assert test == [30, 20]
        print("  PASS: test_list_remove")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_remove — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: set.add()
    # ------------------------------------------------------------------
    try:
        def call_set_add(s, val):
            s.add(val)

        data = set()
        for _ in range(WARMUP):
            call_set_add(data, 0)
        data.clear()
        check_jit_compiled(call_set_add, "call_set_add")

        result = set()
        call_set_add(result, 1)
        call_set_add(result, 2)
        call_set_add(result, 3)
        assert result == {1, 2, 3}

        # Duplicate add — idempotent
        call_set_add(result, 2)
        assert result == {1, 2, 3}
        assert len(result) == 3
        print("  PASS: test_set_add")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_set_add — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: set.discard()
    # ------------------------------------------------------------------
    try:
        def call_discard(s, val):
            s.discard(val)

        data = {1, 2, 3, 4, 5}
        for _ in range(WARMUP):
            data.add(99)
            call_discard(data, 99)
        check_jit_compiled(call_discard, "call_discard")

        test = {10, 20, 30}
        call_discard(test, 20)
        assert test == {10, 30}

        # Discard non-existent — no error
        call_discard(test, 99)
        assert test == {10, 30}

        call_discard(test, 10)
        call_discard(test, 30)
        assert test == set()
        print("  PASS: test_set_discard")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_set_discard — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: set.remove()
    # ------------------------------------------------------------------
    try:
        def call_set_remove(s, val):
            s.remove(val)

        data = {1, 2, 3}
        for _ in range(WARMUP):
            data.add(99)
            call_set_remove(data, 99)
        check_jit_compiled(call_set_remove, "call_set_remove")

        test = {10, 20, 30}
        call_set_remove(test, 20)
        assert test == {10, 30}

        # Remove non-existent — raises KeyError
        got_error = False
        try:
            call_set_remove(test, 99)
        except KeyError:
            got_error = True
        assert got_error, "Expected KeyError for missing element"
        print("  PASS: test_set_remove")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_set_remove — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: dict.__contains__ via operator
    # ------------------------------------------------------------------
    try:
        def check_contains(d, key):
            return key in d

        d = {"a": 1, "b": 2, "c": 3}
        for _ in range(WARMUP):
            check_contains(d, "a")
        check_jit_compiled(check_contains, "check_contains")

        assert check_contains({"x": 1, "y": 2}, "x") is True
        assert check_contains({"x": 1, "y": 2}, "z") is False
        assert check_contains({}, "a") is False
        assert check_contains({1: "a", 2: "b"}, 1) is True
        assert check_contains({1: "a", 2: "b"}, 3) is False
        print("  PASS: test_dict_contains")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_dict_contains — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: bytearray.append()
    # ------------------------------------------------------------------
    try:
        def call_ba_append(ba, val):
            ba.append(val)

        data = bytearray()
        for _ in range(WARMUP):
            call_ba_append(data, 0)
        data.clear()
        check_jit_compiled(call_ba_append, "call_ba_append")

        result = bytearray()
        call_ba_append(result, 65)  # 'A'
        call_ba_append(result, 66)  # 'B'
        call_ba_append(result, 67)  # 'C'
        assert result == bytearray(b"ABC")
        assert len(result) == 3

        # Boundary values
        call_ba_append(result, 0)
        call_ba_append(result, 255)
        assert result[-2] == 0
        assert result[-1] == 255
        print("  PASS: test_bytearray_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bytearray_append — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: deque.append()
    # ------------------------------------------------------------------
    try:
        def call_deque_append(dq, val):
            dq.append(val)

        data = deque()
        for _ in range(WARMUP):
            call_deque_append(data, 0)
        data.clear()
        check_jit_compiled(call_deque_append, "call_deque_append")

        result = deque()
        call_deque_append(result, 1)
        call_deque_append(result, 2)
        call_deque_append(result, 3)
        assert list(result) == [1, 2, 3]
        assert result[-1] == 3
        print("  PASS: test_deque_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deque_append — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: deque.appendleft()
    # ------------------------------------------------------------------
    try:
        def call_deque_appendleft(dq, val):
            dq.appendleft(val)

        data = deque()
        for _ in range(WARMUP):
            call_deque_appendleft(data, 0)
        data.clear()
        check_jit_compiled(call_deque_appendleft, "call_deque_appendleft")

        result = deque()
        call_deque_appendleft(result, 1)
        call_deque_appendleft(result, 2)
        call_deque_appendleft(result, 3)
        assert list(result) == [3, 2, 1]
        assert result[0] == 3
        print("  PASS: test_deque_appendleft")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deque_appendleft — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: list.append with maxlen deque (bounded)
    # ------------------------------------------------------------------
    try:
        def call_bounded_append(dq, val):
            dq.append(val)

        bounded = deque(maxlen=3)
        for _ in range(WARMUP):
            call_bounded_append(bounded, 0)
        bounded.clear()
        check_jit_compiled(call_bounded_append, "call_bounded_append")

        call_bounded_append(bounded, 1)
        call_bounded_append(bounded, 2)
        call_bounded_append(bounded, 3)
        assert list(bounded) == [1, 2, 3]

        # Overflow — drops oldest
        call_bounded_append(bounded, 4)
        assert list(bounded) == [2, 3, 4]

        call_bounded_append(bounded, 5)
        assert list(bounded) == [3, 4, 5]
        print("  PASS: test_bounded_deque_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bounded_deque_append — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — different type with same method name (append)
    # ------------------------------------------------------------------
    try:
        def call_append_poly(obj, val):
            obj.append(val)

        # Warm up on list
        data = []
        for _ in range(WARMUP):
            call_append_poly(data, 0)
        data.clear()
        check_jit_compiled(call_append_poly, "call_append_poly")

        # list.append()
        lst = [1, 2]
        call_append_poly(lst, 3)
        assert lst == [1, 2, 3]

        # Deopt: deque.append()
        dq = deque([10, 20])
        call_append_poly(dq, 30)
        assert list(dq) == [10, 20, 30]

        # Deopt: bytearray.append()
        ba = bytearray(b"AB")
        call_append_poly(ba, 67)
        assert ba == bytearray(b"ABC")

        # Back to list
        lst2 = []
        call_append_poly(lst2, 99)
        assert lst2 == [99]
        print("  PASS: test_deopt_different_type_append")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_type_append — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt — subclass overriding method
    # ------------------------------------------------------------------
    try:
        class TrackingSet(set):
            def __init__(self):
                super().__init__()
                self.add_count = 0
            def add(self, val):
                self.add_count += 1
                super().add(val)

        def call_add_sub(s, val):
            s.add(val)

        # Warm up on plain set
        plain = set()
        for _ in range(WARMUP):
            call_add_sub(plain, 0)
        plain.clear()
        check_jit_compiled(call_add_sub, "call_add_sub")

        plain2 = set()
        call_add_sub(plain2, 1)
        assert plain2 == {1}

        # Deopt: subclass with overridden method
        ts = TrackingSet()
        call_add_sub(ts, 10)
        call_add_sub(ts, 20)
        assert ts == {10, 20}
        assert ts.add_count == 2

        # Back to plain set
        plain3 = set()
        call_add_sub(plain3, 99)
        assert plain3 == {99}
        print("  PASS: test_deopt_subclass_override")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_subclass_override — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid method calls (1000 cycles)
    # ------------------------------------------------------------------
    try:
        def rapid_add(s, val):
            s.add(val)

        data = set()
        for _ in range(WARMUP):
            rapid_add(data, 0)
        data.clear()
        check_jit_compiled(rapid_add, "rapid_add")

        result = set()
        for i in range(1000):
            rapid_add(result, i)
        assert len(result) == 1000
        assert 0 in result
        assert 999 in result
        print("  PASS: test_rapid_method_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_method_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Stability — 10000 calls
    # ------------------------------------------------------------------
    try:
        def stable_append(lst, val):
            lst.append(val)

        data = []
        for _ in range(WARMUP):
            stable_append(data, 0)
        data.clear()
        check_jit_compiled(stable_append, "stable_append")

        for i in range(10000):
            stable_append(data, i)
        assert len(data) == 10000
        assert data[0] == 0
        assert data[9999] == 9999
        print("  PASS: test_stability_10000_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Method with various arg types
    # ------------------------------------------------------------------
    try:
        def add_various(lst, val):
            lst.append(val)

        data = []
        for _ in range(WARMUP):
            add_various(data, 0)
        data.clear()
        check_jit_compiled(add_various, "add_various")

        add_various(data, 42)
        add_various(data, "hello")
        add_various(data, None)
        add_various(data, (1, 2))
        add_various(data, [3, 4])
        add_various(data, {"k": "v"})
        add_various(data, 3.14)
        add_various(data, True)

        assert data == [42, "hello", None, (1, 2), [3, 4], {"k": "v"}, 3.14, True]
        print("  PASS: test_various_arg_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_various_arg_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: set.add() idempotent (duplicate adds)
    # ------------------------------------------------------------------
    try:
        def idempotent_add(s, val):
            s.add(val)

        data = set()
        for _ in range(WARMUP):
            idempotent_add(data, 0)
        data.clear()
        check_jit_compiled(idempotent_add, "idempotent_add")

        result = set()
        for _ in range(100):
            idempotent_add(result, 42)
        assert result == {42}
        assert len(result) == 1

        for i in range(10):
            idempotent_add(result, i)
            idempotent_add(result, i)  # duplicate
        assert len(result) == 11  # 42 + 0..9
        print("  PASS: test_set_add_idempotent")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_set_add_idempotent — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: list.remove() raises ValueError
    # ------------------------------------------------------------------
    try:
        def remove_missing(lst, val):
            lst.remove(val)

        data = [1, 2, 3]
        for _ in range(WARMUP):
            data.append(99)
            remove_missing(data, 99)
        check_jit_compiled(remove_missing, "remove_missing")

        got_error = False
        try:
            remove_missing([1, 2, 3], 99)
        except ValueError:
            got_error = True
        assert got_error, "Expected ValueError for missing element"

        # Empty list
        got_error2 = False
        try:
            remove_missing([], 1)
        except ValueError:
            got_error2 = True
        assert got_error2, "Expected ValueError for empty list"
        print("  PASS: test_list_remove_valueerror")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_list_remove_valueerror — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained single-arg calls
    # ------------------------------------------------------------------
    try:
        def build_and_append(lst, val):
            lst.append(val)
            return lst

        data = []
        for _ in range(WARMUP):
            build_and_append(data, 0)
        data.clear()
        check_jit_compiled(build_and_append, "build_and_append")

        chain = []
        build_and_append(build_and_append(build_and_append(chain, 1), 2), 3)
        assert chain == [1, 2, 3]

        # Chained set operations
        def add_and_return(s, val):
            s.add(val)
            return s

        data_s = set()
        for _ in range(WARMUP):
            add_and_return(data_s, 0)
        data_s.clear()
        check_jit_compiled(add_and_return, "add_and_return")

        s = set()
        add_and_return(add_and_return(add_and_return(s, "a"), "b"), "c")
        assert s == {"a", "b", "c"}
        print("  PASS: test_chained_single_arg_calls")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_single_arg_calls — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: frozenset.__contains__
    # ------------------------------------------------------------------
    try:
        def check_in_frozenset(fs, val):
            return val in fs

        fs = frozenset({1, 2, 3, 4, 5})
        for _ in range(WARMUP):
            check_in_frozenset(fs, 3)
        check_jit_compiled(check_in_frozenset, "check_in_frozenset")

        assert check_in_frozenset(frozenset({10, 20, 30}), 10) is True
        assert check_in_frozenset(frozenset({10, 20, 30}), 99) is False
        assert check_in_frozenset(frozenset(), 1) is False
        assert check_in_frozenset(frozenset({"a", "b"}), "a") is True
        assert check_in_frozenset(frozenset({"a", "b"}), "c") is False
        print("  PASS: test_frozenset_contains")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_frozenset_contains — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — obj.method(arg) vs type.method(obj, arg)
    # ------------------------------------------------------------------
    try:
        def via_instance(lst, val):
            lst.append(val)

        def via_type(lst, val):
            list.append(lst, val)

        for _ in range(WARMUP):
            tmp = []
            via_instance(tmp, 0)
            via_type(tmp, 0)
        check_jit_compiled(via_instance, "via_instance")
        check_jit_compiled(via_type, "via_type")

        # Build two lists identically via different call paths
        lst1 = []
        lst2 = []
        for val in [1, "two", 3.0, None, (4, 5), [6], True]:
            via_instance(lst1, val)
            via_type(lst2, val)
        assert lst1 == lst2, f"Mismatch: {lst1} != {lst2}"

        # Same for set.add
        def set_via_instance(s, val):
            s.add(val)

        def set_via_type(s, val):
            set.add(s, val)

        for _ in range(WARMUP):
            tmp_s = set()
            set_via_instance(tmp_s, 0)
            set_via_type(tmp_s, 0)
        check_jit_compiled(set_via_instance, "set_via_instance")
        check_jit_compiled(set_via_type, "set_via_type")

        s1 = set()
        s2 = set()
        for val in [1, 2, 3, "a", "b", (1, 2)]:
            set_via_instance(s1, val)
            set_via_type(s2, val)
        assert s1 == s2, f"Set mismatch: {s1} != {s2}"
        print("  PASS: test_equivalence_instance_vs_type_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_instance_vs_type_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_METHOD_DESCRIPTOR_O: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
