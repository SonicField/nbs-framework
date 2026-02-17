#!/usr/bin/env python3
"""Minimal reproducer for CinderX JIT aarch64 closure segfault.

Crashes when a closure uses BOTH a free variable (LOAD_DEREF) and
a fast local (LOAD_FAST) in a BINARY_OP.
"""
import cinderx
cinderx.init()
cinderx.install_frame_evaluator()

import cinderjit

def make_adder(x):
    def add(y):
        return x + y  # LOAD_DEREF(x) + LOAD_FAST(y) -> BINARY_OP -> SEGFAULT
    return add

add5 = make_adder(5)

# Force JIT compilation
cinderjit.force_compile(add5)
print(f"add5 JIT compiled: {cinderjit.is_jit_compiled(add5)}")

# This should segfault
result = add5(3)
print(f"add5(3) = {result}")
