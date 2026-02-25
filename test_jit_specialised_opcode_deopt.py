# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Tests for JIT compilation of functions containing specialised bytecodes.

These tests falsify the hypothesis that BytecodeInstruction::opcode() correctly
despecialises CPython's adaptive specialised opcodes before the JIT processes
them.

If opcode() returns raw specialised opcodes (e.g. FOR_ITER_LIST instead of
FOR_ITER), the JIT will either:
  - Crash with JIT_ABORT (no dispatch case for the specialised opcode), or
  - Produce incorrect control flow (isBranch/getJumpTarget don't recognise
    the specialised variant).

The tests cover two compilation paths:
  1. force_compile — exercises JIT compilation directly
  2. Auto-JIT via compile_after_n_calls — exercises the bytecode iteration
     path that scans function bytecode to build the HIR

Both paths must work with specialized_opcodes=true to confirm the fix.

Root cause reference: BytecodeInstruction::opcode() in bytecode.cpp must
return _PyOpcode_Deopt[_Py_OPCODE(word())] rather than raw _Py_OPCODE(word()).
"""

import dis
import sys
import unittest
from typing import Callable, TypeVar

import cinderx
import cinderx.jit
from cinderx.jit import (
    force_compile,
    force_uncompile,
    is_jit_compiled,
    jit_suppress,
    jit_unsuppress,
)
from cinderx.test_support import passIf


TCallableRet = TypeVar("TCallableRet")


def _trigger_specialisation(
    func: Callable[..., TCallableRet],
    caller: Callable[[], TCallableRet],
    n: int = 10,
) -> None:
    """Call func enough times to trigger CPython's adaptive specialiser."""
    for _ in range(n):
        caller()


def _has_specialised_opcode(func: Callable[..., object], base_opname: str) -> bool:
    """Check whether func's bytecode contains a specialised variant of base_opname.

    E.g. _has_specialised_opcode(f, "FOR_ITER") returns True if the bytecode
    contains FOR_ITER_LIST, FOR_ITER_RANGE, FOR_ITER_TUPLE, etc.
    """
    bytecode = dis.Bytecode(func, adaptive=True)  # pyre-ignore[16]
    for insn in bytecode:
        name = insn.opname
        if name.startswith(base_opname + "_") and name != base_opname:
            return True
    return False


def _specialize_and_compile(
    func: Callable[..., TCallableRet],
    caller: Callable[[], TCallableRet],
) -> None:
    """Trigger specialisation then force-compile with the JIT."""
    force_uncompile(func)
    jit_suppress(func)
    _trigger_specialisation(func, caller)
    jit_unsuppress(func)
    force_compile(func)


def _auto_jit_compile(
    func: Callable[..., TCallableRet],
    caller: Callable[[], TCallableRet],
) -> None:
    """Trigger auto-JIT compilation via compile_after_n_calls.

    This exercises the bytecode iteration path that was broken when opcode()
    did not despecialise.  We call enough times to exceed compile_after_n_calls
    (default 50), then call once more to trigger compilation.
    """
    call_limit = cinderx.jit.get_compile_after_n_calls()
    if call_limit is None:
        # Auto-JIT not enabled; fall back to force_compile
        _specialize_and_compile(func, caller)
        return

    force_uncompile(func)
    # Ensure specialisation + enough calls to trigger auto-JIT
    for _ in range(max(call_limit + 10, 60)):
        caller()


# ---------------------------------------------------------------------------
# Test class: force_compile path
# ---------------------------------------------------------------------------


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
@passIf(sys.version_info < (3, 12), "Requires the specialising interpreter")
class ForceCompileSpecialisedOpcodeTests(unittest.TestCase):
    """Verify force_compile works when bytecode contains specialised opcodes.

    Each test:
      1. Defines a function that will trigger a specific opcode specialisation
      2. Runs the function to trigger CPython's adaptive specialiser
      3. Force-compiles with the JIT
      4. Asserts the function is JIT-compiled
      5. Asserts correctness of the result
    """

    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    # -- FOR_ITER family --

    def test_for_iter_list(self) -> None:
        def f() -> int:
            total = 0
            for x in [1, 2, 3, 4, 5]:
                total += x
            return total

        _specialize_and_compile(f, f)

        self.assertTrue(is_jit_compiled(f))
        self.assertEqual(f(), 15)

    def test_for_iter_tuple(self) -> None:
        def f() -> int:
            total = 0
            for x in (10, 20, 30):
                total += x
            return total

        _specialize_and_compile(f, f)

        self.assertTrue(is_jit_compiled(f))
        self.assertEqual(f(), 60)

    def test_for_iter_range(self) -> None:
        def f() -> int:
            total = 0
            for x in range(5):
                total += x
            return total

        _specialize_and_compile(f, f)

        self.assertTrue(is_jit_compiled(f))
        self.assertEqual(f(), 10)

    # -- STORE_SUBSCR family --

    def test_store_subscr_list_int(self) -> None:
        def f() -> list[int]:
            lst = [0, 0, 0]
            lst[0] = 10
            lst[1] = 20
            lst[2] = 30
            return lst

        _specialize_and_compile(f, f)

        self.assertTrue(is_jit_compiled(f))
        self.assertEqual(f(), [10, 20, 30])

    # -- BINARY_SUBSCR family (not in existing test_jit_specialization.py) --

    def test_binary_subscr_list_int(self) -> None:
        def f() -> int:
            lst = [10, 20, 30]
            return lst[0] + lst[1] + lst[2]

        _specialize_and_compile(f, f)

        self.assertTrue(is_jit_compiled(f))
        self.assertEqual(f(), 60)

    # -- Combined: multiple specialised opcodes in one function --

    def test_combined_for_iter_and_subscr(self) -> None:
        """A function using both FOR_ITER_LIST and BINARY_SUBSCR_LIST_INT.

        This is the realistic pattern that originally triggered JIT_ABORT:
        iterating a list and indexing into another.
        """

        def f() -> int:
            data = [10, 20, 30, 40, 50]
            total = 0
            for i in range(len(data)):
                total += data[i]
            return total

        _specialize_and_compile(f, f)

        self.assertTrue(is_jit_compiled(f))
        self.assertEqual(f(), 150)


# ---------------------------------------------------------------------------
# Test class: auto-JIT path (compile_after_n_calls)
# ---------------------------------------------------------------------------


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
@passIf(sys.version_info < (3, 12), "Requires the specialising interpreter")
class AutoJITSpecialisedOpcodeTests(unittest.TestCase):
    """Verify auto-JIT compilation works when bytecode contains specialised opcodes.

    This exercises the compile_after_n_calls path — the path that was broken
    when BytecodeInstruction::opcode() did not despecialise.  The force_compile
    path may work even without the fix (different entry point), so this class
    specifically tests the auto-JIT compilation pathway.
    """

    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_auto_jit_for_iter_list(self) -> None:
        def f() -> int:
            total = 0
            for x in [1, 2, 3, 4, 5]:
                total += x
            return total

        _auto_jit_compile(f, f)

        self.assertTrue(
            is_jit_compiled(f),
            "FOR_ITER_LIST function failed auto-JIT compilation"
        )
        self.assertEqual(f(), 15)

    def test_auto_jit_for_iter_range(self) -> None:
        def f() -> int:
            total = 0
            for x in range(10):
                total += x
            return total

        _auto_jit_compile(f, f)

        self.assertTrue(
            is_jit_compiled(f),
            "FOR_ITER_RANGE function failed auto-JIT compilation"
        )
        self.assertEqual(f(), 45)

    def test_auto_jit_binary_subscr_list_int(self) -> None:
        def f() -> int:
            lst = [10, 20, 30]
            return lst[0] + lst[1] + lst[2]

        _auto_jit_compile(f, f)

        self.assertTrue(
            is_jit_compiled(f),
            "BINARY_SUBSCR_LIST_INT function failed auto-JIT compilation"
        )
        self.assertEqual(f(), 60)

    def test_auto_jit_store_subscr_list_int(self) -> None:
        def f() -> list[int]:
            lst = [0, 0, 0]
            lst[0] = 1
            lst[1] = 2
            lst[2] = 3
            return lst

        _auto_jit_compile(f, f)

        self.assertTrue(
            is_jit_compiled(f),
            "STORE_SUBSCR_LIST_INT function failed auto-JIT compilation"
        )
        self.assertEqual(f(), [1, 2, 3])

    def test_auto_jit_combined_specialised_opcodes(self) -> None:
        """Auto-JIT with multiple specialised opcode families in one function.

        This is the most realistic test: a function that triggers FOR_ITER_LIST,
        BINARY_SUBSCR_LIST_INT, BINARY_OP_ADD_INT, and COMPARE_OP_INT
        specialisations simultaneously.
        """

        def f() -> int:
            data = [10, 20, 30, 40, 50]
            result = 0
            for i in range(len(data)):
                val = data[i]
                if val > 25:
                    result += val
            return result

        _auto_jit_compile(f, f)

        self.assertTrue(
            is_jit_compiled(f),
            "Combined specialised opcode function failed auto-JIT compilation"
        )
        self.assertEqual(f(), 120)  # 30 + 40 + 50


if __name__ == "__main__":
    unittest.main()
