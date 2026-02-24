#!/usr/bin/env python3
"""
test_super_fix.py — Verification tests for the CinderX super().__init__() JIT bug fix.

Run this after fixing the autoJITVectorcall transition bug for super() chains.
All tests should PASS with cinderx.init() + compile_after_n_calls(100).

Usage:
    python3 test_super_fix.py
"""
import sys

def check_cinderx():
    try:
        import cinderx
        cinderx.init()
        import cinderjit
        cinderjit.compile_after_n_calls(100)
        return cinderjit
    except ImportError:
        print("SKIP: CinderX not available")
        sys.exit(0)

cinderjit = check_cinderx()

PASS = 0
FAIL = 0

def test(name, fn):
    global PASS, FAIL
    try:
        fn()
        print(f"  PASS: {name}")
        PASS += 1
    except Exception as e:
        print(f"  FAIL: {name} — {e}")
        FAIL += 1


# ── Test 1: 4-level hierarchy (minimal reproducer) ──────────────────────────

def test_4level():
    class Base:
        def __init__(self, name): self.name = name
    class Layer(Base):
        def __init__(self, name, n):
            super().__init__(name); self.n = n
    class Block(Layer):
        def __init__(self, name, n, num=3):
            super().__init__(name, n); self.num = num
    class Model(Block):
        def __init__(self, name, n=32, nb=2):
            super().__init__(name, n, num=3); self.nb = nb

    for i in range(200):
        m = Model("test")
    assert m.name == "test"
    assert m.n == 32
    assert m.num == 3
    assert m.nb == 2


# ── Test 2: 5-level hierarchy (deep_class pattern) ──────────────────────────

def test_5level():
    class Base:
        def __init__(self, name): self.name = name
    class Layer(Base):
        def __init__(self, name, n):
            super().__init__(name); self.n = n
    class Block(Layer):
        def __init__(self, name, n, num=3):
            super().__init__(name, n); self.num = num
    class Net(Block):
        def __init__(self, name, n, nb=2):
            super().__init__(name, n, num=3); self.nb = nb
    class Model(Net):
        def __init__(self, name, n=32, nb=2):
            super().__init__(name, n, nb); self.cw = 0.01 * n

    for i in range(200):
        m = Model("test")
    assert m.name == "test"
    assert m.cw == 0.32


# ── Test 3: Recursive construction (Block creates Layer instances) ───────────

def test_recursive_construction():
    class Base:
        def __init__(self, name): self.name = name
    class Layer(Base):
        def __init__(self, name, n):
            super().__init__(name); self.n = n; self.w = 0.01 * n
    class Block(Layer):
        def __init__(self, name, n, num=3):
            super().__init__(name, n)
            self.num = num
            self.layers = [Layer(f"{name}_s{i}", n) for i in range(num)]
    class Model(Block):
        def __init__(self, name, n=32, nb=2):
            super().__init__(name, n, num=3)
            self.blocks = [Block(f"{name}_b{i}", n) for i in range(nb)]

    for i in range(200):
        m = Model("test")
    assert len(m.layers) == 3
    assert len(m.blocks) == 2
    assert m.blocks[0].layers[0].name == "test_b0_s0"


# ── Test 4: JIT compilation verification ─────────────────────────────────────

def test_jit_compiled():
    class A:
        def __init__(self, x): self.x = x
    class B(A):
        def __init__(self, x, y):
            super().__init__(x); self.y = y

    for _ in range(500):
        B(1, 2)

    # Verify JIT compilation status (informational — don't fail if API missing)
    try:
        compiled = cinderjit.is_jit_compiled(B.__init__)
        print(f"    B.__init__ JIT compiled: {compiled}")
    except AttributeError:
        print("    is_jit_compiled API not available")


# ── Test 5: compile_after_n_calls threshold tracking ─────────────────────────

def test_threshold_tracking():
    """The bug previously triggered at exactly compile_after_n_calls threshold."""
    class Base:
        def __init__(self, name): self.name = name
    class Layer(Base):
        def __init__(self, name, n):
            super().__init__(name); self.n = n
    class Block(Layer):
        def __init__(self, name, n, num=3):
            super().__init__(name, n); self.num = num
    class Model(Block):
        def __init__(self, name, n=32, nb=2):
            super().__init__(name, n, num=3); self.nb = nb

    # Run well past the threshold (100) to verify no corruption
    for i in range(500):
        m = Model("test")
        assert m.name == "test", f"Corruption at iter {i}"


# ── Test 6: Mixed hierarchy depths ──────────────────────────────────────────

def test_mixed_depths():
    """Verify 2, 3, 4, 5 level hierarchies all work in the same process."""
    class A:
        def __init__(self): self.a = 1

    class B(A):
        def __init__(self): super().__init__(); self.b = 2

    class C(B):
        def __init__(self): super().__init__(); self.c = 3

    class D(C):
        def __init__(self): super().__init__(); self.d = 4

    class E(D):
        def __init__(self): super().__init__(); self.e = 5

    for _ in range(200):
        assert B().b == 2
        assert C().c == 3
        assert D().d == 4
        assert E().e == 5


# ── Run all tests ────────────────────────────────────────────────────────────

print("=== CinderX super() Bug Fix Verification ===")
print()

test("4-level hierarchy", test_4level)
test("5-level hierarchy", test_5level)
test("Recursive construction", test_recursive_construction)
test("JIT compilation", test_jit_compiled)
test("Threshold tracking (500 iters)", test_threshold_tracking)
test("Mixed hierarchy depths", test_mixed_depths)

print()
print(f"Results: {PASS} PASS, {FAIL} FAIL")
if FAIL > 0:
    print("VERDICT: FAILURES DETECTED")
    sys.exit(1)
else:
    print("VERDICT: ALL TESTS PASSED")
    sys.exit(0)
