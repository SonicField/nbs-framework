#!/usr/bin/env python3
"""
LOAD_ATTR polymorphic reproducer — discriminating test matrix.

Tests whether LOAD_ATTR_INSTANCE_VALUE produces correct results for
polymorphic type hierarchies under different JIT/spec conditions.

Based on testkeeper's finding: self.nxt loaded as int instead of Node
for polymorphic linked list with ~5000 warmup iterations.

Usage (on build-host
  # Test A: JIT + spec ON
  PYTHONJIT=1 python3 test_load_attr_poly.py --spec-on --force-compile

  # Test B: JIT + spec OFF
  PYTHONJIT=1 python3 test_load_attr_poly.py --force-compile

  # Test C: No JIT
  PYTHONJIT=0 python3 test_load_attr_poly.py

  # Test D: JIT + spec ON + HIR dump
  PYTHONJIT=1 PYTHONJITHIRDUMPHIR=1 python3 test_load_attr_poly.py --spec-on --force-compile
"""
import os
import sys
import time

# --- Polymorphic node hierarchy ---
class Node:
    def __init__(self, val, nxt=None):
        self.val = val
        self.nxt = nxt

class DoubleNode(Node):
    def __init__(self, val, nxt=None):
        super().__init__(val, nxt)
        self.extra = val * 2

class TripleNode(Node):
    def __init__(self, val, nxt=None):
        super().__init__(val, nxt)
        self.extra = val * 3
        self.tag = "triple"


def traverse_list(head):
    """Traverse linked list, returning sum of values."""
    total = 0
    current = head
    while current is not None:
        # This is the LOAD_ATTR_INSTANCE_VALUE site.
        # For polymorphic receivers (Node, DoubleNode, TripleNode),
        # the inline cache may specialise for one type.
        # On a JIT type guard deopt, the interpreter should handle
        # the other types correctly.
        val = current.val
        if not isinstance(val, int):
            print(f"BUG: current.val returned {type(val).__name__}={val!r}, expected int")
            print(f"  current type: {type(current).__name__}")
            print(f"  current.__dict__: {current.__dict__}")
            return -1  # Sentinel for bug detection
        total += val
        nxt = current.nxt
        if nxt is not None and not isinstance(nxt, Node):
            print(f"BUG: current.nxt returned {type(nxt).__name__}={nxt!r}, expected Node or None")
            print(f"  current type: {type(current).__name__}")
            print(f"  current.__dict__: {current.__dict__}")
            return -1
        current = nxt
    return total


def build_polymorphic_list(n):
    """Build a linked list with mixed Node types."""
    head = None
    for i in range(n, 0, -1):
        if i % 3 == 0:
            head = TripleNode(i, head)
        elif i % 3 == 1:
            head = DoubleNode(i, head)
        else:
            head = Node(i, head)
    return head


def run_test(warmup_count, spec_on, force_compile):
    """Run the test with given configuration."""
    # Build the list
    head = build_polymorphic_list(100)
    expected = sum(range(1, 101))  # 5050

    # Check CinderX availability
    jit_available = False
    try:
        import cinderjit
        jit_available = True
    except ImportError:
        pass

    if jit_available and spec_on:
        cinderjit.enable_specialized_opcodes(True)
        print(f"Specialised opcodes: ENABLED")
    elif jit_available:
        print(f"Specialised opcodes: DISABLED (default)")
    else:
        print(f"CinderX: NOT AVAILABLE (interpreter only)")

    # Warmup
    print(f"Warmup: {warmup_count} iterations...")
    bug_detected = False
    for i in range(warmup_count):
        result = traverse_list(head)
        if result == -1:
            print(f"BUG detected at warmup iteration {i}")
            bug_detected = True
            break
        if result != expected:
            print(f"WRONG RESULT at warmup iteration {i}: got {result}, expected {expected}")
            bug_detected = True
            break

    if bug_detected:
        return False

    # Force compile if requested
    if jit_available and force_compile:
        cinderjit.force_compile(traverse_list)
        compiled = cinderjit.is_jit_compiled(traverse_list)
        print(f"force_compile(traverse_list): is_jit_compiled={compiled}")

    # Post-compilation test
    print(f"Post-compilation test (10 iterations)...")
    for i in range(10):
        result = traverse_list(head)
        if result == -1:
            print(f"BUG detected at post-compilation iteration {i}")
            return False
        if result != expected:
            print(f"WRONG RESULT at post-compilation iteration {i}: got {result}, expected {expected}")
            return False

    print(f"All iterations produced correct result: {expected}")
    return True


if __name__ == "__main__":
    spec_on = "--spec-on" in sys.argv
    force_compile = "--force-compile" in sys.argv
    warmup = 15000  # Same as testkeeper's test

    print("=" * 60)
    print("LOAD_ATTR Polymorphic Reproducer")
    print("=" * 60)
    print(f"Python: {sys.version}")
    print(f"PYTHONJIT: {os.environ.get('PYTHONJIT', 'unset')}")
    print(f"spec_on: {spec_on}")
    print(f"force_compile: {force_compile}")
    print(f"warmup: {warmup}")
    print()

    success = run_test(warmup, spec_on, force_compile)

    print()
    if success:
        print("RESULT: PASS — no correctness bugs detected")
    else:
        print("RESULT: FAIL — correctness bug detected")
    sys.exit(0 if success else 1)
