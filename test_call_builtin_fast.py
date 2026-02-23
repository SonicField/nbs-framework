#!/usr/bin/env python3
"""Deopt and correctness tests for CALL_BUILTIN_FAST specialisation.

Tests that METH_FASTCALL builtins work correctly through the JIT,
including deoptimisation when the callable changes.

Run with CinderX JIT enabled:
  /data/users/alexturner/cinderx_dev/venv/bin/python test_call_builtin_fast.py

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup → JIT → check result)
  2. Correct deopt when callable is replaced (monomorphic → polymorphic)
  3. Edge cases for each specific builtin

  A test PASSES only if all assertions hold. A test FAILS if any
  assertion fires or an unexpected exception occurs.
"""
import sys

WARMUP = 15000  # CinderX auto-compilation typically needs 10000+ calls

# Set to True to require JIT compilation when cinderjit is available.
# When True, tests FAIL if the function is not JIT-compiled (avoids
# false confidence from interpreter-only execution).
REQUIRE_JIT = True


def check_jit_compiled(func, name):
    """Verify function is JIT-compiled.

    If REQUIRE_JIT is True and cinderjit is importable, raises AssertionError
    when the function is not compiled — the test is not exercising the JIT
    path it claims to test. If cinderjit is not available, always returns
    False (interpreter-only mode, tests still run for correctness baseline).

    NOTE: cinderjit.is_jit_compiled() is BROKEN on AArch64 (always returns
    False). Use get_compiled_functions() as fallback.
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


# ── isinstance tests ─────────────────────────────────────────────────────────

def test_isinstance_basic():
    """isinstance(obj, cls) returns correct results through JIT."""
    class Animal: pass
    class Dog(Animal): pass
    class Cat(Animal): pass

    def check_isinstance(obj, cls):
        return isinstance(obj, cls)

    # Warmup
    dog = Dog()
    for _ in range(WARMUP):
        check_isinstance(dog, Animal)

    check_jit_compiled(check_isinstance, "check_isinstance")

    # Correctness
    assert check_isinstance(dog, Animal) == True
    assert check_isinstance(dog, Dog) == True
    assert check_isinstance(dog, Cat) == False
    assert check_isinstance(42, int) == True
    assert check_isinstance("hello", int) == False

    # Tuple of types
    assert check_isinstance(dog, (Dog, Cat)) == True
    assert check_isinstance(42, (str, int)) == True
    assert check_isinstance(42, (str, list)) == False

    print("  PASS: isinstance basic")


def test_isinstance_deopt():
    """isinstance deopts correctly when callable is replaced."""
    def use_isinstance(obj, cls):
        return isinstance(obj, cls)

    # Warmup with isinstance
    for _ in range(WARMUP):
        use_isinstance(42, int)

    check_jit_compiled(use_isinstance, "use_isinstance")
    assert use_isinstance(42, int) == True

    # Replace isinstance in locals — should deopt
    # (Note: replacing builtins requires globals manipulation)
    import builtins
    saved = builtins.isinstance
    try:
        def fake_isinstance(obj, cls):
            return "fake"

        builtins.isinstance = fake_isinstance

        # After replacing isinstance, the call should deopt and use fake
        result = use_isinstance(42, int)
        assert result == "fake", f"Expected 'fake', got {result}"
    finally:
        builtins.isinstance = saved

    # Verify restored isinstance works
    assert use_isinstance(42, int) == True

    print("  PASS: isinstance deopt")


# ── hasattr tests ────────────────────────────────────────────────────────────

def test_hasattr_basic():
    """hasattr(obj, name) returns correct results through JIT."""
    class Obj:
        def __init__(self):
            self.x = 1
            self.y = 2

    def check_hasattr(obj, name):
        return hasattr(obj, name)

    obj = Obj()
    for _ in range(WARMUP):
        check_hasattr(obj, 'x')

    check_jit_compiled(check_hasattr, "check_hasattr")

    assert check_hasattr(obj, 'x') == True
    assert check_hasattr(obj, 'y') == True
    assert check_hasattr(obj, 'z') == False
    assert check_hasattr(obj, '__class__') == True

    print("  PASS: hasattr basic")


# ── getattr tests ────────────────────────────────────────────────────────────

def test_getattr_basic():
    """getattr(obj, name) and getattr(obj, name, default) through JIT."""
    class Obj:
        def __init__(self):
            self.x = 42
            self.y = "hello"

    def get_attr_2(obj, name):
        return getattr(obj, name)

    def get_attr_3(obj, name, default):
        return getattr(obj, name, default)

    obj = Obj()
    for _ in range(WARMUP):
        get_attr_2(obj, 'x')
        get_attr_3(obj, 'missing', -1)

    check_jit_compiled(get_attr_2, "get_attr_2")
    check_jit_compiled(get_attr_3, "get_attr_3")

    # 2-arg getattr
    assert get_attr_2(obj, 'x') == 42
    assert get_attr_2(obj, 'y') == "hello"
    try:
        get_attr_2(obj, 'missing')
        assert False, "Should have raised AttributeError"
    except AttributeError:
        pass

    # 3-arg getattr (with default)
    assert get_attr_3(obj, 'x', -1) == 42
    assert get_attr_3(obj, 'missing', -1) == -1
    assert get_attr_3(obj, 'missing', None) is None

    print("  PASS: getattr basic")


# ── next tests ───────────────────────────────────────────────────────────────

def test_next_basic():
    """next(iterator) and next(iterator, default) through JIT."""
    def consume_next(it):
        return next(it)

    def consume_next_default(it, default):
        return next(it, default)

    # Warmup
    for _ in range(WARMUP):
        consume_next(iter([1]))
        consume_next_default(iter([1]), -1)

    check_jit_compiled(consume_next, "consume_next")
    check_jit_compiled(consume_next_default, "consume_next_default")

    # Basic next
    it = iter([10, 20, 30])
    assert consume_next(it) == 10
    assert consume_next(it) == 20
    assert consume_next(it) == 30
    try:
        consume_next(it)
        assert False, "Should have raised StopIteration"
    except StopIteration:
        pass

    # next with default
    it = iter([10, 20])
    assert consume_next_default(it, -1) == 10
    assert consume_next_default(it, -1) == 20
    assert consume_next_default(it, -1) == -1  # exhausted, returns default

    print("  PASS: next basic")


# ── divmod tests ─────────────────────────────────────────────────────────────

def test_divmod_basic():
    """divmod(a, b) returns correct results through JIT."""
    def compute_divmod(a, b):
        return divmod(a, b)

    for _ in range(WARMUP):
        compute_divmod(100, 7)

    check_jit_compiled(compute_divmod, "compute_divmod")

    assert compute_divmod(100, 7) == (14, 2)
    assert compute_divmod(0, 5) == (0, 0)
    assert compute_divmod(-7, 3) == (-3, 2)  # Python floor division
    assert compute_divmod(7, -3) == (-3, -2)

    # Division by zero
    try:
        compute_divmod(1, 0)
        assert False, "Should have raised ZeroDivisionError"
    except ZeroDivisionError:
        pass

    # Float divmod
    q, r = compute_divmod(7.5, 2.0)
    assert q == 3.0
    assert abs(r - 1.5) < 1e-10

    print("  PASS: divmod basic")


# ── issubclass tests ─────────────────────────────────────────────────────────

def test_issubclass_basic():
    """issubclass(cls, base) returns correct results through JIT."""
    class A: pass
    class B(A): pass
    class C: pass

    def check_issubclass(cls, base):
        return issubclass(cls, base)

    for _ in range(WARMUP):
        check_issubclass(B, A)

    check_jit_compiled(check_issubclass, "check_issubclass")

    assert check_issubclass(B, A) == True
    assert check_issubclass(A, A) == True
    assert check_issubclass(C, A) == False
    assert check_issubclass(bool, int) == True

    # Tuple of bases
    assert check_issubclass(B, (A, C)) == True
    assert check_issubclass(C, (A, B)) == False

    print("  PASS: issubclass basic")


# ── iter tests ───────────────────────────────────────────────────────────────

def test_iter_basic():
    """iter(iterable) returns correct iterators through JIT."""
    def make_iter(obj):
        return iter(obj)

    for _ in range(WARMUP):
        make_iter([1, 2, 3])

    check_jit_compiled(make_iter, "make_iter")

    # List iterator
    it = make_iter([1, 2, 3])
    assert list(it) == [1, 2, 3]

    # Tuple iterator
    it = make_iter((4, 5, 6))
    assert list(it) == [4, 5, 6]

    # String iterator
    it = make_iter("abc")
    assert list(it) == ['a', 'b', 'c']

    # Empty
    it = make_iter([])
    assert list(it) == []

    print("  PASS: iter basic")


# ── format tests ─────────────────────────────────────────────────────────────

def test_format_basic():
    """format(value, spec) returns correct strings through JIT."""
    def fmt(value, spec):
        return format(value, spec)

    for _ in range(WARMUP):
        fmt(3.14, '.2f')

    check_jit_compiled(fmt, "fmt")

    assert fmt(3.14159, '.2f') == '3.14'
    assert fmt(42, 'd') == '42'
    assert fmt(42, '08b') == '00101010'
    assert fmt(255, '02x') == 'ff'
    assert fmt('hello', '>10') == '     hello'

    print("  PASS: format basic")


# ── Runner ───────────────────────────────────────────────────────────────────

def main():
    print("=" * 60)
    print("CALL_BUILTIN_FAST Deopt & Correctness Tests")
    print("=" * 60)
    print(f"Python: {sys.version}")

    try:
        import cinderx
        if hasattr(cinderx, 'init'):
            cinderx.init()
        import cinderjit
        cinderjit.auto()
        try:
            cinderjit.enable_specialized_opcodes()
        except AttributeError:
            pass
        print(f"CinderX JIT: enabled (auto-compilation active)")
    except (ImportError, AttributeError):
        print(f"CinderX JIT: not available (running interpreter-only)")
    print()

    tests = [
        ("isinstance basic",    test_isinstance_basic),
        ("isinstance deopt",    test_isinstance_deopt),
        ("hasattr basic",       test_hasattr_basic),
        ("getattr basic",       test_getattr_basic),
        ("next basic",          test_next_basic),
        ("divmod basic",        test_divmod_basic),
        ("issubclass basic",    test_issubclass_basic),
        ("iter basic",          test_iter_basic),
        ("format basic",        test_format_basic),
    ]

    passed = 0
    failed = 0
    for name, test_func in tests:
        try:
            test_func()
            passed += 1
        except Exception as e:
            print(f"  FAIL: {name} — {e}")
            failed += 1

    print()
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")

    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
