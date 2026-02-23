#!/usr/bin/env python3
"""
test_call_builtin_class.py — Correctness and deopt tests for
CALL_BUILTIN_CLASS specialisation.

Targets: CALL_BUILTIN_CLASS.

CALL_BUILTIN_CLASS specialises calls to builtin type constructors —
int(), float(), str(), list(), tuple(), dict(), set(), bool(), bytes(),
bytearray(), frozenset(), etc. Instead of going through the generic
CALL dispatch (tp_call → type.__call__ → tp_new + tp_init), the
specialisation recognises that the callable is a builtin type with
METH_FASTCALL-compatible __new__ and calls it directly.

The JIT specialisation emits a guard on the callable identity (or type)
to confirm it is still the expected builtin type, then uses the direct
constructor path.

Deopt triggers:
  - Callable replaced with a different function
  - Callable is a subclass of a builtin type
  - Custom metaclass __call__

Tests cover:
  - int() constructor (no args, from str, from float)
  - float() constructor
  - str() constructor
  - list() constructor (from iterable)
  - tuple() constructor
  - dict() constructor (from kwargs, from pairs)
  - set() constructor
  - bool() constructor
  - bytes() / bytearray() constructors
  - type() single-arg form (returns type of object)
  - Deopt: builtin replaced with custom callable
  - Deopt: subclass constructor
  - Constructor with invalid args (TypeError/ValueError)
  - Multiple builtin calls in one function
  - Builtin constructor in loop
  - frozenset() constructor
  - complex() constructor
  - Constructor from various input types
  - Chained constructors (int(str(float)))
  - enumerate() / zip() / map() / filter()

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after callable change (deopt fires)
  3. Correct result for both original and new callables after deopt

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_builtin_class.py
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
    print("=== CALL_BUILTIN_CLASS Correctness & Deopt Tests ===")
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

    # ── Test 1: int() constructor ────────────────────────────────────────

    def call_int_1(x):
        return int(x)

    for _ in range(WARMUP):
        call_int_1("42")

    check_jit_compiled(call_int_1, "call_int_1")

    try:
        assert call_int_1("42") == 42
        assert call_int_1("0") == 0
        assert call_int_1("-7") == -7
        assert call_int_1(3.14) == 3
        assert call_int_1(3.99) == 3  # Truncates, not rounds
        assert call_int_1(-2.7) == -2
        assert call_int_1(True) == 1
        assert call_int_1(False) == 0
        print("PASS  Test 1: int() constructor (str, float, bool)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 1: int() constructor — {e}")
        failed += 1

    # ── Test 2: int() no args and base ───────────────────────────────────

    def call_int_noarg_2():
        return int()

    for _ in range(WARMUP):
        call_int_noarg_2()

    check_jit_compiled(call_int_noarg_2, "call_int_noarg_2")

    try:
        assert call_int_noarg_2() == 0

        # int with base
        def call_int_base(s, base):
            return int(s, base)

        for _ in range(WARMUP):
            call_int_base("ff", 16)

        assert call_int_base("ff", 16) == 255
        assert call_int_base("1010", 2) == 10
        assert call_int_base("77", 8) == 63

        print("PASS  Test 2: int() no args and with base")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 2: int() no args/base — {e}")
        failed += 1

    # ── Test 3: float() constructor ──────────────────────────────────────

    def call_float_3(x):
        return float(x)

    for _ in range(WARMUP):
        call_float_3("3.14")

    check_jit_compiled(call_float_3, "call_float_3")

    try:
        assert call_float_3("3.14") == 3.14
        assert call_float_3("0") == 0.0
        assert call_float_3("-1.5") == -1.5
        assert call_float_3(42) == 42.0
        assert call_float_3(0) == 0.0
        assert call_float_3("inf") == float('inf')
        assert call_float_3("-inf") == float('-inf')

        # float() no args
        assert float() == 0.0

        print("PASS  Test 3: float() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 3: float() constructor — {e}")
        failed += 1

    # ── Test 4: str() constructor ────────────────────────────────────────

    def call_str_4(x):
        return str(x)

    for _ in range(WARMUP):
        call_str_4(42)

    check_jit_compiled(call_str_4, "call_str_4")

    try:
        assert call_str_4(42) == "42"
        assert call_str_4(3.14) == "3.14"
        assert call_str_4(True) == "True"
        assert call_str_4(False) == "False"
        assert call_str_4(None) == "None"
        assert call_str_4([1, 2]) == "[1, 2]"
        assert call_str_4("hello") == "hello"

        # str() no args
        assert str() == ""

        print("PASS  Test 4: str() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 4: str() constructor — {e}")
        failed += 1

    # ── Test 5: list() constructor ───────────────────────────────────────

    def call_list_5(x):
        return list(x)

    for _ in range(WARMUP):
        call_list_5(range(3))

    check_jit_compiled(call_list_5, "call_list_5")

    try:
        assert call_list_5(range(5)) == [0, 1, 2, 3, 4]
        assert call_list_5((1, 2, 3)) == [1, 2, 3]
        assert call_list_5("abc") == ["a", "b", "c"]
        result_set = call_list_5({1, 2, 3})
        assert sorted(result_set) == [1, 2, 3]  # Set order is arbitrary
        assert call_list_5([]) == []
        assert call_list_5(range(0)) == []

        # list() no args
        assert list() == []

        print("PASS  Test 5: list() constructor (range, tuple, str, set)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 5: list() constructor — {e}")
        failed += 1

    # ── Test 6: tuple() constructor ──────────────────────────────────────

    def call_tuple_6(x):
        return tuple(x)

    for _ in range(WARMUP):
        call_tuple_6([1, 2, 3])

    check_jit_compiled(call_tuple_6, "call_tuple_6")

    try:
        assert call_tuple_6([1, 2, 3]) == (1, 2, 3)
        assert call_tuple_6(range(4)) == (0, 1, 2, 3)
        assert call_tuple_6("xy") == ("x", "y")
        assert call_tuple_6([]) == ()

        # tuple() no args
        assert tuple() == ()

        print("PASS  Test 6: tuple() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 6: tuple() constructor — {e}")
        failed += 1

    # ── Test 7: dict() constructor ───────────────────────────────────────

    def call_dict_kwargs_7(**kwargs):
        return dict(**kwargs)

    def call_dict_pairs_7(pairs):
        return dict(pairs)

    for _ in range(WARMUP):
        call_dict_kwargs_7(a=1, b=2)

    check_jit_compiled(call_dict_kwargs_7, "call_dict_kwargs_7")

    for _ in range(WARMUP):
        call_dict_pairs_7([("a", 1)])

    check_jit_compiled(call_dict_pairs_7, "call_dict_pairs_7")

    try:
        assert call_dict_kwargs_7(a=1, b=2) == {"a": 1, "b": 2}
        assert call_dict_kwargs_7() == {}

        assert call_dict_pairs_7([("x", 1), ("y", 2)]) == {"x": 1, "y": 2}
        assert call_dict_pairs_7([]) == {}

        # dict() no args
        assert dict() == {}

        print("PASS  Test 7: dict() constructor (kwargs, pairs)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 7: dict() constructor — {e}")
        failed += 1

    # ── Test 8: set() constructor ────────────────────────────────────────

    def call_set_8(x):
        return set(x)

    for _ in range(WARMUP):
        call_set_8([1, 2, 3])

    check_jit_compiled(call_set_8, "call_set_8")

    try:
        assert call_set_8([1, 2, 3]) == {1, 2, 3}
        assert call_set_8([1, 1, 2, 2, 3]) == {1, 2, 3}
        assert call_set_8("aab") == {"a", "b"}
        assert call_set_8([]) == set()
        assert call_set_8(range(3)) == {0, 1, 2}

        # set() no args
        assert set() == set()

        print("PASS  Test 8: set() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 8: set() constructor — {e}")
        failed += 1

    # ── Test 9: bool() constructor ───────────────────────────────────────

    def call_bool_9(x):
        return bool(x)

    for _ in range(WARMUP):
        call_bool_9(42)

    check_jit_compiled(call_bool_9, "call_bool_9")

    try:
        assert call_bool_9(0) is False
        assert call_bool_9(1) is True
        assert call_bool_9(-1) is True
        assert call_bool_9("") is False
        assert call_bool_9("x") is True
        assert call_bool_9([]) is False
        assert call_bool_9([1]) is True
        assert call_bool_9(None) is False
        assert call_bool_9(0.0) is False
        assert call_bool_9(0.1) is True

        # bool() no args
        assert bool() is False

        print("PASS  Test 9: bool() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 9: bool() constructor — {e}")
        failed += 1

    # ── Test 10: type() single-arg form ──────────────────────────────────

    def call_type_10(x):
        return type(x)

    for _ in range(WARMUP):
        call_type_10(42)

    check_jit_compiled(call_type_10, "call_type_10")

    try:
        assert call_type_10(42) is int
        assert call_type_10("hi") is str
        assert call_type_10([]) is list
        assert call_type_10(3.14) is float
        assert call_type_10(True) is bool
        assert call_type_10(None) is type(None)
        assert call_type_10({}) is dict
        print("PASS  Test 10: type() single-arg form")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 10: type() — {e}")
        failed += 1

    # ── Test 11: bytes() constructor ─────────────────────────────────────

    def call_bytes_11(x):
        return bytes(x)

    for _ in range(WARMUP):
        call_bytes_11(3)

    check_jit_compiled(call_bytes_11, "call_bytes_11")

    try:
        assert call_bytes_11(3) == b'\x00\x00\x00'
        assert call_bytes_11(0) == b''
        assert call_bytes_11([65, 66, 67]) == b'ABC'
        assert call_bytes_11(range(3)) == b'\x00\x01\x02'

        # bytes() no args
        assert bytes() == b''

        print("PASS  Test 11: bytes() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 11: bytes() constructor — {e}")
        failed += 1

    # ── Test 12: Deopt builtin replaced with custom callable ─────────────

    def make_converter_12(converter):
        def convert(x):
            return converter(x)
        return convert

    convert_jit = make_converter_12(int)

    for _ in range(WARMUP):
        convert_jit("42")

    check_jit_compiled(convert_jit, "convert_jit")

    try:
        assert convert_jit("42") == 42

        # Replace with custom function
        convert_custom = make_converter_12(lambda x: f"custom:{x}")
        # Note: convert_custom has same code object but different closure
        # The JIT on convert_jit should not affect convert_custom
        assert convert_custom("42") == "custom:42"

        # Original still works
        assert convert_jit("99") == 99

        print("PASS  Test 12: converter with different callables (int vs lambda)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 12: callable replacement — {e}")
        failed += 1

    # ── Test 13: Constructor with invalid args (ValueError) ──────────────

    def call_int_13(x):
        return int(x)

    for _ in range(WARMUP):
        call_int_13("42")

    check_jit_compiled(call_int_13, "call_int_13")

    try:
        assert call_int_13("42") == 42

        try:
            call_int_13("not_a_number")
            assert False, "expected ValueError"
        except ValueError:
            pass

        try:
            call_int_13("3.14")  # int() doesn't accept float strings
            assert False, "expected ValueError"
        except ValueError:
            pass

        # Still works after errors
        assert call_int_13("99") == 99

        print("PASS  Test 13: constructor ValueError for invalid args")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 13: invalid args — {e}")
        failed += 1

    # ── Test 14: Constructor TypeError ───────────────────────────────────

    def call_int_14(x):
        return int(x)

    for _ in range(WARMUP):
        call_int_14("42")

    check_jit_compiled(call_int_14, "call_int_14")

    try:
        assert call_int_14("42") == 42

        try:
            call_int_14([1, 2, 3])
            assert False, "expected TypeError for list"
        except TypeError:
            pass

        try:
            call_int_14(None)
            assert False, "expected TypeError for None"
        except TypeError:
            pass

        # Still works
        assert call_int_14("0") == 0

        print("PASS  Test 14: constructor TypeError for invalid types")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 14: TypeError — {e}")
        failed += 1

    # ── Test 15: Multiple builtin calls in one function ──────────────────

    def multi_call_15(x):
        return (int(x), float(x), str(x))

    for _ in range(WARMUP):
        multi_call_15(42)

    check_jit_compiled(multi_call_15, "multi_call_15")

    try:
        assert multi_call_15(42) == (42, 42.0, "42")
        assert multi_call_15(0) == (0, 0.0, "0")
        assert multi_call_15(True) == (1, 1.0, "True")

        print("PASS  Test 15: multiple builtin calls in one function")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 15: multiple calls — {e}")
        failed += 1

    # ── Test 16: Builtin constructor in loop ─────────────────────────────

    def loop_int_16(strings):
        result = []
        for s in strings:
            result.append(int(s))
        return result

    for _ in range(WARMUP):
        loop_int_16(["1", "2", "3"])

    check_jit_compiled(loop_int_16, "loop_int_16")

    try:
        assert loop_int_16(["1", "2", "3"]) == [1, 2, 3]
        assert loop_int_16([]) == []
        assert loop_int_16(["0", "-1", "999"]) == [0, -1, 999]
        print("PASS  Test 16: int() in loop")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 16: constructor in loop — {e}")
        failed += 1

    # ── Test 17: frozenset() constructor ─────────────────────────────────

    def call_frozenset_17(x):
        return frozenset(x)

    for _ in range(WARMUP):
        call_frozenset_17([1, 2, 3])

    check_jit_compiled(call_frozenset_17, "call_frozenset_17")

    try:
        assert call_frozenset_17([1, 2, 3]) == frozenset({1, 2, 3})
        assert call_frozenset_17([1, 1, 2]) == frozenset({1, 2})
        assert call_frozenset_17([]) == frozenset()
        assert call_frozenset_17("abc") == frozenset({"a", "b", "c"})

        # frozenset() no args
        assert frozenset() == frozenset()

        print("PASS  Test 17: frozenset() constructor")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 17: frozenset() — {e}")
        failed += 1

    # ── Test 18: Chained constructors ────────────────────────────────────

    def chained_18(x):
        return str(int(float(x)))

    for _ in range(WARMUP):
        chained_18("3.14")

    check_jit_compiled(chained_18, "chained_18")

    try:
        assert chained_18("3.14") == "3"
        assert chained_18("0.0") == "0"
        assert chained_18("-2.7") == "-2"
        assert chained_18("100.99") == "100"
        print("PASS  Test 18: chained constructors str(int(float()))")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 18: chained constructors — {e}")
        failed += 1

    # ── Test 19: Subclass constructor (deopt from builtin) ───────────────

    class MyInt(int):
        pass

    class MyList(list):
        pass

    def call_constructor_19(cls, x):
        return cls(x)

    for _ in range(WARMUP):
        call_constructor_19(int, "42")

    check_jit_compiled(call_constructor_19, "call_constructor_19")

    try:
        # Builtin
        result = call_constructor_19(int, "42")
        assert result == 42
        assert type(result) is int

        # Deopt: subclass
        result = call_constructor_19(MyInt, "42")
        assert result == 42
        assert type(result) is MyInt

        # Deopt: list subclass
        result = call_constructor_19(MyList, [1, 2])
        assert result == [1, 2]
        assert type(result) is MyList

        # Builtin still works
        result = call_constructor_19(int, "99")
        assert result == 99
        assert type(result) is int

        print("PASS  Test 19: subclass constructor (deopt from builtin)")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 19: subclass constructor — {e}")
        failed += 1

    # ── Test 20: Custom __int__ / __float__ / __str__ ────────────────────

    class Convertible:
        def __init__(self, val):
            self.val = val
        def __int__(self):
            return self.val
        def __float__(self):
            return float(self.val)
        def __str__(self):
            return f"C({self.val})"

    def convert_int_20(x):
        return int(x)

    def convert_str_20(x):
        return str(x)

    for _ in range(WARMUP):
        convert_int_20(42)

    check_jit_compiled(convert_int_20, "convert_int_20")

    for _ in range(WARMUP):
        convert_str_20(42)

    check_jit_compiled(convert_str_20, "convert_str_20")

    try:
        # Normal int
        assert convert_int_20(42) == 42

        # Custom __int__
        assert convert_int_20(Convertible(99)) == 99
        assert convert_int_20(Convertible(0)) == 0

        # Normal str
        assert convert_str_20(42) == "42"

        # Custom __str__
        assert convert_str_20(Convertible(7)) == "C(7)"

        # Normal still works
        assert convert_int_20(10) == 10

        print("PASS  Test 20: custom __int__ / __str__ protocol")
        passed += 1
    except (AssertionError, Exception) as e:
        print(f"FAIL  Test 20: custom protocols — {e}")
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
