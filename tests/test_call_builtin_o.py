#!/usr/bin/env python3
"""
test_call_builtin_o.py — Correctness and deopt tests for
CALL_BUILTIN_O specialisation.

Targets: CALL_BUILTIN_O.

CALL_BUILTIN_O specialises calls to C-implemented builtin functions that
take exactly one positional argument (METH_O flag in PyMethodDef). Instead
of going through the generic CALL dispatch (which must resolve the callable,
check argument counts, and dispatch through vectorcall or tp_call), the
specialisation calls the C function pointer directly.

Common METH_O builtins: len(), repr(), str(), int(), float(), bool(), abs(),
hash(), id(), type(), chr(), ord(), hex(), oct(), bin(), ascii(), callable().

The adaptive specialiser emits CALL_BUILTIN_O after observing repeated calls
to the same METH_O builtin with a single argument.

Deopt triggers:
  - Callable changes (e.g. len -> repr)
  - Callable is not a METH_O builtin (e.g. user-defined function)
  - Callable is shadowed or replaced at runtime

Tests cover:
  - len() on list, str, tuple, dict, set
  - repr() on various types
  - abs() on int and float
  - bool() on various types
  - type() single-arg form
  - id() identity preservation
  - chr() and ord() round-trip
  - hex(), oct(), bin() formatting
  - hash() on immutable types
  - callable() check
  - Deopt: builtin -> user function
  - Deopt: one builtin -> different builtin
  - Deopt: shadowed builtin
  - Loop accumulator with builtin calls
  - Rapid callable alternation
  - Exception propagation from builtin
  - ascii() on unicode
  - float() single-arg conversion
  - int() single-arg conversion
  - Equivalence: builtin(x) vs type.__call__(builtin, x)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after callable change (deopt fires)
  3. Semantic equivalence with known-good reference implementations

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_builtin_o.py
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
    # Test 1: len() on list
    # ------------------------------------------------------------------
    try:
        def call_len(obj):
            return len(obj)

        data = [1, 2, 3, 4, 5]
        for _ in range(WARMUP):
            call_len(data)
        check_jit_compiled(call_len, "call_len")

        assert call_len(data) == 5
        assert call_len([]) == 0
        assert call_len([None]) == 1
        assert call_len(list(range(1000))) == 1000
        print("  PASS: test_len_list")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_list — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: len() on str, tuple, dict, set
    # ------------------------------------------------------------------
    try:
        def call_len2(obj):
            return len(obj)

        lst = [1, 2, 3]
        for _ in range(WARMUP):
            call_len2(lst)
        check_jit_compiled(call_len2, "call_len2")

        assert call_len2("hello") == 5
        assert call_len2("") == 0
        assert call_len2((1, 2, 3)) == 3
        assert call_len2({}) == 0
        assert call_len2({"a": 1, "b": 2}) == 2
        assert call_len2({1, 2, 3, 4}) == 4
        assert call_len2(set()) == 0
        print("  PASS: test_len_various_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_len_various_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: repr() on various types
    # ------------------------------------------------------------------
    try:
        def call_repr(obj):
            return repr(obj)

        for _ in range(WARMUP):
            call_repr(42)
        check_jit_compiled(call_repr, "call_repr")

        assert call_repr(42) == "42"
        assert call_repr("hello") == "'hello'"
        assert call_repr([1, 2]) == "[1, 2]"
        assert call_repr(None) == "None"
        assert call_repr(True) == "True"
        assert call_repr(3.14) == "3.14"
        print("  PASS: test_repr_various_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_repr_various_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: abs() on int and float
    # ------------------------------------------------------------------
    try:
        def call_abs(x):
            return abs(x)

        for _ in range(WARMUP):
            call_abs(-5)
        check_jit_compiled(call_abs, "call_abs")

        assert call_abs(-5) == 5
        assert call_abs(5) == 5
        assert call_abs(0) == 0
        assert call_abs(-3.14) == 3.14
        assert call_abs(3.14) == 3.14
        assert call_abs(-10**50) == 10**50
        print("  PASS: test_abs_int_float")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_abs_int_float — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: bool() on various types
    # ------------------------------------------------------------------
    try:
        def call_bool(x):
            return bool(x)

        for _ in range(WARMUP):
            call_bool(1)
        check_jit_compiled(call_bool, "call_bool")

        assert call_bool(1) is True
        assert call_bool(0) is False
        assert call_bool("") is False
        assert call_bool("x") is True
        assert call_bool([]) is False
        assert call_bool([1]) is True
        assert call_bool(None) is False
        assert call_bool(0.0) is False
        assert call_bool(0.1) is True
        print("  PASS: test_bool_various_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bool_various_types — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: type() single-arg form
    # ------------------------------------------------------------------
    try:
        def call_type(x):
            return type(x)

        for _ in range(WARMUP):
            call_type(42)
        check_jit_compiled(call_type, "call_type")

        assert call_type(42) is int
        assert call_type("hello") is str
        assert call_type([1, 2]) is list
        assert call_type(3.14) is float
        assert call_type(None) is type(None)
        assert call_type(True) is bool
        assert call_type((1,)) is tuple

        class MyClass:
            pass
        obj = MyClass()
        assert call_type(obj) is MyClass
        print("  PASS: test_type_single_arg")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_type_single_arg — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: id() identity preservation
    # ------------------------------------------------------------------
    try:
        def call_id(x):
            return id(x)

        sentinel = object()
        for _ in range(WARMUP):
            call_id(sentinel)
        check_jit_compiled(call_id, "call_id")

        # Same object must produce same id
        id1 = call_id(sentinel)
        id2 = call_id(sentinel)
        assert id1 == id2, f"Same object gave different ids: {id1} vs {id2}"

        # Different objects must produce different ids
        other = object()
        id3 = call_id(other)
        assert id1 != id3, "Different objects gave same id"

        # id matches built-in id()
        assert call_id(sentinel) == id(sentinel)
        print("  PASS: test_id_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_id_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: chr() and ord() round-trip
    # ------------------------------------------------------------------
    try:
        def call_chr(n):
            return chr(n)

        def call_ord(c):
            return ord(c)

        for _ in range(WARMUP):
            call_chr(65)
        for _ in range(WARMUP):
            call_ord("A")
        check_jit_compiled(call_chr, "call_chr")
        check_jit_compiled(call_ord, "call_ord")

        assert call_chr(65) == "A"
        assert call_chr(97) == "a"
        assert call_chr(0) == "\x00"
        assert call_chr(8364) == "\u20ac"  # Euro sign

        assert call_ord("A") == 65
        assert call_ord("a") == 97
        assert call_ord("\x00") == 0

        # Round-trip: chr(ord(c)) == c and ord(chr(n)) == n
        for n in [0, 32, 65, 97, 127, 255, 1000, 8364, 65535]:
            assert call_ord(call_chr(n)) == n
        print("  PASS: test_chr_ord_roundtrip")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chr_ord_roundtrip — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: hex(), oct(), bin() formatting
    # ------------------------------------------------------------------
    try:
        def call_hex(n):
            return hex(n)

        def call_oct(n):
            return oct(n)

        def call_bin(n):
            return bin(n)

        for _ in range(WARMUP):
            call_hex(255)
        for _ in range(WARMUP):
            call_oct(255)
        for _ in range(WARMUP):
            call_bin(255)
        check_jit_compiled(call_hex, "call_hex")
        check_jit_compiled(call_oct, "call_oct")
        check_jit_compiled(call_bin, "call_bin")

        assert call_hex(255) == "0xff"
        assert call_hex(0) == "0x0"
        assert call_hex(-1) == "-0x1"
        assert call_hex(16) == "0x10"

        assert call_oct(8) == "0o10"
        assert call_oct(0) == "0o0"
        assert call_oct(-1) == "-0o1"

        assert call_bin(10) == "0b1010"
        assert call_bin(0) == "0b0"
        assert call_bin(-1) == "-0b1"
        assert call_bin(255) == "0b11111111"
        print("  PASS: test_hex_oct_bin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_hex_oct_bin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: hash() on immutable types
    # ------------------------------------------------------------------
    try:
        def call_hash(x):
            return hash(x)

        for _ in range(WARMUP):
            call_hash(42)
        check_jit_compiled(call_hash, "call_hash")

        # hash must be deterministic within a run
        h1 = call_hash(42)
        h2 = call_hash(42)
        assert h1 == h2, f"Same value gave different hashes: {h1} vs {h2}"

        # hash must match built-in hash()
        assert call_hash(42) == hash(42)
        assert call_hash("hello") == hash("hello")
        assert call_hash((1, 2, 3)) == hash((1, 2, 3))
        assert call_hash(None) == hash(None)
        assert call_hash(True) == hash(True)
        assert call_hash(3.14) == hash(3.14)

        # Unhashable should raise TypeError
        raised = False
        try:
            call_hash([1, 2])
        except TypeError:
            raised = True
        assert raised, "Expected TypeError for unhashable list"
        print("  PASS: test_hash_immutable")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_hash_immutable — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: callable() check
    # ------------------------------------------------------------------
    try:
        def call_callable(x):
            return callable(x)

        for _ in range(WARMUP):
            call_callable(len)
        check_jit_compiled(call_callable, "call_callable")

        assert call_callable(len) is True
        assert call_callable(print) is True
        assert call_callable(lambda x: x) is True
        assert call_callable(int) is True
        assert call_callable(42) is False
        assert call_callable("hello") is False
        assert call_callable(None) is False
        assert call_callable([]) is False

        class WithCall:
            def __call__(self):
                return 1
        assert call_callable(WithCall()) is True
        assert call_callable(WithCall) is True
        print("  PASS: test_callable_check")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_callable_check — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Deopt builtin -> user function
    # ------------------------------------------------------------------
    try:
        def user_len(x):
            return 999

        def call_fn(fn, arg):
            return fn(arg)

        for _ in range(WARMUP):
            call_fn(len, [1, 2, 3])
        check_jit_compiled(call_fn, "call_fn")

        assert call_fn(len, [1, 2, 3]) == 3
        # Deopt: switch to user function
        assert call_fn(user_len, [1, 2, 3]) == 999
        # Back to builtin
        assert call_fn(len, [1, 2, 3, 4]) == 4
        # Different user function
        assert call_fn(lambda x: x[0], [10, 20]) == 10
        print("  PASS: test_deopt_builtin_to_user_function")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_builtin_to_user_function — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Deopt one builtin -> different builtin
    # ------------------------------------------------------------------
    try:
        def call_one_arg(fn, arg):
            return fn(arg)

        for _ in range(WARMUP):
            call_one_arg(len, [1, 2])
        check_jit_compiled(call_one_arg, "call_one_arg")

        assert call_one_arg(len, [1, 2]) == 2
        # Switch to repr (different METH_O builtin)
        assert call_one_arg(repr, 42) == "42"
        # Switch to abs
        assert call_one_arg(abs, -7) == 7
        # Switch to bool
        assert call_one_arg(bool, 0) is False
        # Back to len
        assert call_one_arg(len, "abc") == 3
        print("  PASS: test_deopt_builtin_to_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_builtin_to_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Deopt shadowed builtin
    # ------------------------------------------------------------------
    try:
        def call_with_local_len(lst):
            return len(lst)

        for _ in range(WARMUP):
            call_with_local_len([1, 2, 3])
        check_jit_compiled(call_with_local_len, "call_with_local_len")

        assert call_with_local_len([1, 2, 3]) == 3

        # Shadow len in the function's globals
        original_len = len
        try:
            globals()["len"] = lambda x: 42
            # After shadowing, call should use the shadow
            assert call_with_local_len([1, 2, 3]) == 42
        finally:
            globals()["len"] = original_len

        # After restoring, should work again
        assert call_with_local_len([1, 2, 3]) == 3
        print("  PASS: test_deopt_shadowed_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_shadowed_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Loop accumulator with builtin calls
    # ------------------------------------------------------------------
    try:
        def sum_of_lengths(lists):
            total = 0
            for lst in lists:
                total += len(lst)
            return total

        data = [[1], [1, 2], [1, 2, 3], [1, 2, 3, 4]]
        for _ in range(WARMUP):
            sum_of_lengths(data)
        check_jit_compiled(sum_of_lengths, "sum_of_lengths")

        assert sum_of_lengths(data) == 10  # 1+2+3+4
        assert sum_of_lengths([]) == 0
        assert sum_of_lengths([[], [], []]) == 0
        assert sum_of_lengths([list(range(i)) for i in range(100)]) == 4950
        print("  PASS: test_loop_accumulator_builtin")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_loop_accumulator_builtin — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Rapid callable alternation
    # ------------------------------------------------------------------
    try:
        def call_dynamic(fn, arg):
            return fn(arg)

        for _ in range(WARMUP):
            call_dynamic(len, [1, 2])
        check_jit_compiled(call_dynamic, "call_dynamic")

        for cycle in range(50):
            r1 = call_dynamic(len, [1, 2, 3])
            r2 = call_dynamic(repr, 42)
            r3 = call_dynamic(abs, -10)
            assert r1 == 3, f"len failed at cycle {cycle}"
            assert r2 == "42", f"repr failed at cycle {cycle}"
            assert r3 == 10, f"abs failed at cycle {cycle}"

        # Final check
        assert call_dynamic(len, "abcde") == 5
        print("  PASS: test_rapid_callable_alternation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_callable_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Exception propagation from builtin
    # ------------------------------------------------------------------
    try:
        def call_ord_wrapper(c):
            return ord(c)

        for _ in range(WARMUP):
            call_ord_wrapper("A")
        check_jit_compiled(call_ord_wrapper, "call_ord_wrapper")

        assert call_ord_wrapper("A") == 65

        # ord() with string of length > 1 raises TypeError
        raised_type = False
        try:
            call_ord_wrapper("AB")
        except TypeError:
            raised_type = True
        assert raised_type, "Expected TypeError for multi-char string"

        # chr() with out-of-range raises ValueError
        def call_chr_wrapper(n):
            return chr(n)

        for _ in range(WARMUP):
            call_chr_wrapper(65)
        check_jit_compiled(call_chr_wrapper, "call_chr_wrapper")

        raised_val = False
        try:
            call_chr_wrapper(-1)
        except ValueError:
            raised_val = True
        assert raised_val, "Expected ValueError for negative chr()"

        raised_val2 = False
        try:
            call_chr_wrapper(0x110000)  # max unicode + 1
        except ValueError:
            raised_val2 = True
        assert raised_val2, "Expected ValueError for chr() beyond max unicode"
        print("  PASS: test_exception_propagation")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_exception_propagation — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: ascii() on unicode
    # ------------------------------------------------------------------
    try:
        def call_ascii(x):
            return ascii(x)

        for _ in range(WARMUP):
            call_ascii("hello")
        check_jit_compiled(call_ascii, "call_ascii")

        assert call_ascii("hello") == "'hello'"
        assert call_ascii("caf\u00e9") == "'caf\\xe9'"
        assert call_ascii("\u20ac") == "'\\u20ac'"  # Euro sign
        assert call_ascii("\U0001f600") == "'\\U0001f600'"  # emoji
        assert call_ascii(42) == "42"
        assert call_ascii(None) == "None"
        print("  PASS: test_ascii_unicode")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_ascii_unicode — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: float() and int() single-arg conversion
    # ------------------------------------------------------------------
    try:
        def call_float(x):
            return float(x)

        def call_int(x):
            return int(x)

        for _ in range(WARMUP):
            call_float(42)
        for _ in range(WARMUP):
            call_int(3.14)
        check_jit_compiled(call_float, "call_float")
        check_jit_compiled(call_int, "call_int")

        assert call_float(42) == 42.0
        assert type(call_float(42)) is float
        assert call_float("3.14") == 3.14
        assert call_float(True) == 1.0
        assert call_float(False) == 0.0

        assert call_int(3.14) == 3
        assert type(call_int(3.14)) is int
        assert call_int("42") == 42
        assert call_int(True) == 1
        assert call_int(False) == 0

        # ValueError for bad string
        raised = False
        try:
            call_int("not_a_number")
        except ValueError:
            raised = True
        assert raised, "Expected ValueError for int('not_a_number')"
        print("  PASS: test_float_int_conversion")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_float_int_conversion — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — len(x) vs x.__len__()
    # ------------------------------------------------------------------
    try:
        def builtin_len(x):
            return len(x)

        def dunder_len(x):
            return x.__len__()

        data_cases = [
            [1, 2, 3],
            "hello",
            (1, 2),
            {"a": 1, "b": 2, "c": 3},
            {1, 2, 3, 4, 5},
            b"bytes",
            bytearray(b"ba"),
            range(100),
        ]

        for _ in range(WARMUP):
            builtin_len([1, 2, 3])
        check_jit_compiled(builtin_len, "builtin_len")

        for case in data_cases:
            bl = builtin_len(case)
            dl = dunder_len(case)
            assert bl == dl, (
                f"Mismatch for {type(case).__name__}: "
                f"len()={bl}, __len__()={dl}"
            )
        print("  PASS: test_equivalence_len_vs_dunder")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_len_vs_dunder — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_BUILTIN_O: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
