#!/usr/bin/env python3
"""Mutual-exclusion tests for JIT builtin calling-convention paths.

The CinderX JIT has two calling-convention paths for builtins:

1. HIR-specialised path: builtins with direct HIR simplification
   (isinstance, hasattr, getattr, next) are translated to specialised
   HIR operations at compile time via TranslateSpecializedCall.

2. METH_FASTCALL wrapper path: builtins flagged METH_FASTCALL but
   WITHOUT HIR simplification (divmod, issubclass, iter, format) go
   through JITRT_FastCall0/1/2/3 wrappers.

INVARIANT: A builtin must go through exactly one path, never both.
If a future HIR simplification is added for a builtin (e.g. getattr
fast path), the METH_FASTCALL wrapper must not also fire on the same
callsite.

FALSIFICATION DESIGN:
  These tests cannot directly observe which JIT path fired (both
  produce correct results when working). Instead, they test:

  1. DEOPT CORRECTNESS: Both paths must deopt correctly when the
     builtin is replaced in builtins.__dict__. If the wrong path
     fires, deopt guards may be incorrect.

  2. GENERATOR-SPECIFIC: next() with generators must use the
     JITRT_BuiltinNext / InvokeIterNext fast path, not fall back to
     tp_iternext indirection. We test correctness of generator next()
     edge cases that would fail if the wrong path fired.

  3. CROSS-PATH INTERFERENCE: After warmup on one builtin, replacing
     it with a different builtin that uses the OTHER calling convention
     must deopt correctly.

Run with CinderX JIT enabled:
  /data/users/alexturner/cinderx_dev/venv/bin/python test_call_path_mutual_exclusion.py
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


# ── Section 1: HIR-specialised builtins deopt correctly ──────────────────

def test_isinstance_deopt_to_custom():
    """isinstance is HIR-specialised. Replacing it must trigger deopt
    and the replacement function must execute, not the HIR path."""
    def use_isinstance(obj, cls):
        return isinstance(obj, cls)

    for _ in range(WARMUP):
        use_isinstance(42, int)

    check_jit_compiled(use_isinstance, "use_isinstance")

    # Verify HIR path works
    assert use_isinstance(42, int) is True
    assert use_isinstance("x", int) is False

    # Replace isinstance — HIR guard must fire deopt
    import builtins
    saved = builtins.isinstance
    try:
        call_count = [0]

        def counting_isinstance(obj, cls):
            call_count[0] += 1
            return saved(obj, cls)

        builtins.isinstance = counting_isinstance
        result = use_isinstance(42, int)
        assert result is True, f"Wrong result after deopt: {result}"
        assert call_count[0] == 1, (
            f"Replacement not called (count={call_count[0]}). "
            "HIR path may have fired despite builtin replacement — "
            "mutual exclusion violated."
        )
    finally:
        builtins.isinstance = saved

    # Verify original works after restore
    assert use_isinstance(42, int) is True
    print("  PASS: isinstance deopt to custom replacement")


def test_hasattr_deopt_to_custom():
    """hasattr is HIR-specialised. Replacing it must trigger deopt."""
    class Obj:
        x = 1

    def use_hasattr(obj, name):
        return hasattr(obj, name)

    for _ in range(WARMUP):
        use_hasattr(Obj(), 'x')

    check_jit_compiled(use_hasattr, "use_hasattr")
    assert use_hasattr(Obj(), 'x') is True
    assert use_hasattr(Obj(), 'z') is False

    import builtins
    saved = builtins.hasattr
    try:
        call_count = [0]

        def counting_hasattr(obj, name):
            call_count[0] += 1
            return saved(obj, name)

        builtins.hasattr = counting_hasattr
        result = use_hasattr(Obj(), 'x')
        assert result is True, f"Wrong result: {result}"
        assert call_count[0] == 1, (
            f"Replacement not called (count={call_count[0]}). "
            "HIR path may have fired despite replacement."
        )
    finally:
        builtins.hasattr = saved

    print("  PASS: hasattr deopt to custom replacement")


def test_getattr_deopt_to_custom():
    """getattr is HIR-specialised. Replacing it must trigger deopt."""
    class Obj:
        x = 42

    def use_getattr(obj, name):
        return getattr(obj, name)

    for _ in range(WARMUP):
        use_getattr(Obj(), 'x')

    check_jit_compiled(use_getattr, "use_getattr")
    assert use_getattr(Obj(), 'x') == 42

    import builtins
    saved = builtins.getattr
    try:
        call_count = [0]

        def counting_getattr(obj, name, *args):
            call_count[0] += 1
            return saved(obj, name, *args)

        builtins.getattr = counting_getattr
        result = use_getattr(Obj(), 'x')
        assert result == 42, f"Wrong result: {result}"
        assert call_count[0] == 1, (
            f"Replacement not called (count={call_count[0]}). "
            "HIR path may have fired despite replacement."
        )
    finally:
        builtins.getattr = saved

    print("  PASS: getattr deopt to custom replacement")


def test_next_deopt_to_custom():
    """next is HIR-specialised (via builtinNext / JITRT_BuiltinNext).
    Replacing it must trigger deopt."""
    def use_next(it):
        return next(it)

    for _ in range(WARMUP):
        use_next(iter([1]))

    check_jit_compiled(use_next, "use_next")

    import builtins
    saved = builtins.next
    try:
        call_count = [0]

        def counting_next(*args):
            call_count[0] += 1
            return saved(*args)

        builtins.next = counting_next
        result = use_next(iter([99]))
        assert result == 99, f"Wrong result: {result}"
        assert call_count[0] == 1, (
            f"Replacement not called (count={call_count[0]}). "
            "builtinNext path may have fired despite replacement."
        )
    finally:
        builtins.next = saved

    print("  PASS: next deopt to custom replacement")


# ── Section 2: Generator next() edge cases ───────────────────────────────
# These specifically test the JITRT_BuiltinNext / InvokeIterNext path.

def test_generator_next_exhaustion():
    """next(gen) on exhausted generator must raise StopIteration.
    next(gen, default) on exhausted generator must return default.
    This tests the sentinel conversion in JITRT_BuiltinNext."""
    def gen_three():
        yield 1
        yield 2
        yield 3

    def consume_next(it):
        return next(it)

    def consume_next_default(it, default):
        return next(it, default)

    # Warmup both functions
    for _ in range(WARMUP):
        consume_next(iter([1]))
        consume_next_default(iter([1]), -1)

    check_jit_compiled(consume_next, "consume_next")
    check_jit_compiled(consume_next_default, "consume_next_default")

    # Test generator exhaustion with next(gen) — must raise StopIteration
    g = gen_three()
    assert consume_next(g) == 1
    assert consume_next(g) == 2
    assert consume_next(g) == 3
    try:
        consume_next(g)
        assert False, "Should have raised StopIteration"
    except StopIteration:
        pass

    # Test generator exhaustion with next(gen, default) — must return default
    g2 = gen_three()
    assert consume_next_default(g2, -1) == 1
    assert consume_next_default(g2, -1) == 2
    assert consume_next_default(g2, -1) == 3
    assert consume_next_default(g2, -1) == -1  # exhausted → default

    # None as default (distinct from no-default)
    g3 = gen_three()
    for _ in range(3):
        consume_next_default(g3, None)
    result = consume_next_default(g3, None)
    assert result is None, f"Expected None default, got {result}"

    print("  PASS: generator next exhaustion (StopIteration / default)")


def test_generator_next_with_exception():
    """Generator that raises non-StopIteration must propagate the error,
    not swallow it as a sentinel. Tests JITRT_BuiltinNext NULL return
    path (error case vs exhaustion case)."""
    def gen_error():
        yield 1
        raise ValueError("generator error")

    def consume_next(it):
        return next(it)

    def consume_next_default(it, default):
        return next(it, default)

    for _ in range(WARMUP):
        consume_next(iter([1]))
        consume_next_default(iter([1]), -1)

    # next(gen) — ValueError must propagate, not become StopIteration
    g = gen_error()
    assert consume_next(g) == 1
    try:
        consume_next(g)
        assert False, "Should have raised ValueError"
    except ValueError as e:
        assert "generator error" in str(e)

    # next(gen, default) — ValueError must propagate, not return default
    g2 = gen_error()
    assert consume_next_default(g2, -1) == 1
    try:
        consume_next_default(g2, -1)
        assert False, "Should have raised ValueError, not returned default"
    except ValueError as e:
        assert "generator error" in str(e)

    print("  PASS: generator next error propagation (not swallowed as sentinel)")


def test_generator_next_send_interleave():
    """Interleave next() and generator.send() to stress the JIT's
    generator resumption path. The InvokeIterNext fast path must not
    interfere with send()."""
    def echo_gen():
        val = yield "start"
        while True:
            val = yield f"echo:{val}"

    def consume_next(it):
        return next(it)

    for _ in range(WARMUP):
        consume_next(iter([1]))

    g = echo_gen()
    # First next() primes the generator
    result = consume_next(g)
    assert result == "start", f"Expected 'start', got {result}"

    # send() should work even after JIT-compiled next() primed the generator
    result = g.send("hello")
    assert result == "echo:hello", f"Expected 'echo:hello', got {result}"

    # next() again (sends None)
    result = consume_next(g)
    assert result == "echo:None", f"Expected 'echo:None', got {result}"

    # Another send()
    result = g.send(42)
    assert result == "echo:42", f"Expected 'echo:42', got {result}"

    print("  PASS: generator next/send interleave")


# ── Section 3: Cross-path interference ───────────────────────────────────
# Replace an HIR-specialised builtin with a METH_FASTCALL-only builtin.

def test_isinstance_replaced_with_divmod():
    """Replace isinstance (HIR-specialised) with divmod (METH_FASTCALL-only).
    The JIT must deopt and use divmod correctly, not crash or use stale
    HIR-compiled code."""
    def use_isinstance(a, b):
        return isinstance(a, b)

    for _ in range(WARMUP):
        use_isinstance(42, int)

    check_jit_compiled(use_isinstance, "use_isinstance")
    assert use_isinstance(42, int) is True

    import builtins
    saved = builtins.isinstance
    try:
        builtins.isinstance = divmod
        # Now isinstance(7, 3) should call divmod(7, 3) = (2, 1)
        result = use_isinstance(7, 3)
        assert result == (2, 1), (
            f"Expected divmod(7,3)=(2,1), got {result}. "
            "Deopt may have failed — stale HIR isinstance still firing."
        )
    finally:
        builtins.isinstance = saved

    # Verify isinstance restored
    assert use_isinstance(42, int) is True
    print("  PASS: isinstance replaced with divmod — cross-path deopt correct")


def test_next_replaced_with_iter():
    """Replace next (HIR-specialised / builtinNext) with iter
    (METH_FASTCALL-only). The JIT must deopt and use iter correctly."""
    def use_next(obj):
        return next(obj)

    for _ in range(WARMUP):
        use_next(iter([1]))

    check_jit_compiled(use_next, "use_next")

    import builtins
    saved = builtins.next
    try:
        builtins.next = iter
        # Now next([1,2,3]) should call iter([1,2,3]) = list_iterator
        result = use_next([1, 2, 3])
        assert list(result) == [1, 2, 3], (
            f"Expected iter([1,2,3]) to produce [1,2,3], got {list(result)}. "
            "builtinNext deopt may have failed."
        )
    finally:
        builtins.next = saved

    # Verify next restored
    assert use_next(iter([99])) == 99
    print("  PASS: next replaced with iter — cross-path deopt correct")


# ── Section 4: METH_FASTCALL-only builtins deopt correctly ───────────────
# These builtins should go through JITRT_FastCall wrappers, not HIR.

def test_divmod_deopt():
    """divmod is METH_FASTCALL-only. Deopt must work correctly."""
    def use_divmod(a, b):
        return divmod(a, b)

    for _ in range(WARMUP):
        use_divmod(100, 7)

    check_jit_compiled(use_divmod, "use_divmod")
    assert use_divmod(100, 7) == (14, 2)

    import builtins
    saved = builtins.divmod
    try:
        call_count = [0]

        def counting_divmod(a, b):
            call_count[0] += 1
            return saved(a, b)

        builtins.divmod = counting_divmod
        result = use_divmod(100, 7)
        assert result == (14, 2), f"Wrong result: {result}"
        assert call_count[0] == 1, (
            f"Replacement not called (count={call_count[0]}). "
            "METH_FASTCALL wrapper deopt may be broken."
        )
    finally:
        builtins.divmod = saved

    print("  PASS: divmod deopt to custom replacement")


def test_issubclass_deopt():
    """issubclass is METH_FASTCALL-only. Deopt must work correctly."""
    class A: pass
    class B(A): pass

    def use_issubclass(cls, base):
        return issubclass(cls, base)

    for _ in range(WARMUP):
        use_issubclass(B, A)

    check_jit_compiled(use_issubclass, "use_issubclass")
    assert use_issubclass(B, A) is True

    import builtins
    saved = builtins.issubclass
    try:
        call_count = [0]

        def counting_issubclass(cls, base):
            call_count[0] += 1
            return saved(cls, base)

        builtins.issubclass = counting_issubclass
        result = use_issubclass(B, A)
        assert result is True, f"Wrong result: {result}"
        assert call_count[0] == 1, (
            f"Replacement not called (count={call_count[0]})."
        )
    finally:
        builtins.issubclass = saved

    print("  PASS: issubclass deopt to custom replacement")


# ── Runner ───────────────────────────────────────────────────────────────

def main():
    print("=" * 60)
    print("Calling-Convention Path Mutual Exclusion Tests")
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
        print("CinderX JIT: enabled (auto-compilation active)")
    except (ImportError, AttributeError):
        print("CinderX JIT: not available (running interpreter-only)")
    print()

    tests = [
        # Section 1: HIR-specialised builtins deopt correctly
        ("isinstance deopt to custom",       test_isinstance_deopt_to_custom),
        ("hasattr deopt to custom",          test_hasattr_deopt_to_custom),
        ("getattr deopt to custom",          test_getattr_deopt_to_custom),
        ("next deopt to custom",             test_next_deopt_to_custom),

        # Section 2: Generator next() edge cases
        ("generator next exhaustion",        test_generator_next_exhaustion),
        ("generator next error propagation", test_generator_next_with_exception),
        ("generator next/send interleave",   test_generator_next_send_interleave),

        # Section 3: Cross-path interference
        ("isinstance → divmod cross-path",   test_isinstance_replaced_with_divmod),
        ("next → iter cross-path",           test_next_replaced_with_iter),

        # Section 4: METH_FASTCALL-only deopt
        ("divmod deopt to custom",           test_divmod_deopt),
        ("issubclass deopt to custom",       test_issubclass_deopt),
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
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
