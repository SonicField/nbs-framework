"""
test_for_iter_polymorphic_deopt — Polymorphic iteration deopt tests.

Targets: All FOR_ITER specialisations (RANGE, LIST, TUPLE) emit a GuardType
at GET_ITER. When a function is JIT-compiled with one iterator type and then
called with a different iterator type, the GuardType must fire, triggering
deoptimisation back to the interpreter. The interpreter must then produce
the correct result.

These tests verify:
1. A function compiled with list iteration deopts correctly when given
   a range/tuple/dict/set iterator.
2. A function compiled with range iteration deopts correctly when given
   a list/tuple iterator.
3. After deopt, the function continues to produce correct results for
   BOTH the original and new iterator types.
4. Repeated alternation between types produces correct results every time.

This is the polymorphic test that Pythia flagged as missing. Bug 5 showed
that deopt can resume at the wrong bytecode offset — these tests would have
caught that class of bug for FOR_ITER specialisations.

Usage:
  python3 test_for_iter_polymorphic_deopt.py
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
    print("=== FOR_ITER Polymorphic Deopt Tests ===")
    print()

    try:
        import cinderx
        cinderx.init()
        import cinderjit
        cinderjit.auto()
        # Enable specialised opcodes — FOR_ITER_LIST/RANGE/TUPLE are gated behind this flag
        try:
            cinderjit.enable_specialized_opcodes()
        except AttributeError:
            pass  # Older builds may not have this
    except (ImportError, AttributeError):
        print("SKIP — cinderx/cinderjit not available")
        sys.exit(0)

    passed = 0
    failed = 0

    # ── Test 1: List-compiled function, then range input ──────────────

    def sum_iter_1(iterable):
        """Sum over any iterable. Will be compiled with list input."""
        total = 0
        for x in iterable:
            total += x
        return total

    # Warm up with list input (triggers FOR_ITER_LIST specialisation)
    # CinderX auto() mode needs 10000+ calls for compilation
    print("Test 1: List-compiled, then range deopt")
    for _ in range(WARMUP):
        sum_iter_1(list(range(50)))

    check_jit_compiled(sum_iter_1, "sum_iter_1")

    # Verify list still works
    list_result = sum_iter_1(list(range(100)))
    expected = sum(range(100))
    assert list_result == expected, f"List path broken: {list_result} != {expected}"

    # Now call with range — should trigger GuardType deopt
    range_result = sum_iter_1(range(100))
    if range_result == expected:
        print("  PASS  range deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  range deopt: got {range_result}, expected {expected}")
        failed += 1

    # Verify list STILL works after deopt
    list_result_2 = sum_iter_1(list(range(100)))
    if list_result_2 == expected:
        print("  PASS  list path still correct after deopt")
        passed += 1
    else:
        print(f"  FAIL  list path after deopt: got {list_result_2}, expected {expected}")
        failed += 1

    # ── Test 2: Range-compiled function, then list input ──────────────

    def sum_iter_2(iterable):
        total = 0
        for x in iterable:
            total += x
        return total

    print()
    print("Test 2: Range-compiled, then list deopt")
    for _ in range(WARMUP):
        sum_iter_2(range(50))

    check_jit_compiled(sum_iter_2, "sum_iter_2")

    range_result = sum_iter_2(range(100))
    assert range_result == expected

    list_result = sum_iter_2(list(range(100)))
    if list_result == expected:
        print("  PASS  list deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  list deopt: got {list_result}, expected {expected}")
        failed += 1

    # ── Test 3: List-compiled, then tuple input ───────────────────────

    def sum_iter_3(iterable):
        total = 0
        for x in iterable:
            total += x
        return total

    print()
    print("Test 3: List-compiled, then tuple deopt")
    for _ in range(WARMUP):
        sum_iter_3(list(range(50)))

    check_jit_compiled(sum_iter_3, "sum_iter_3")

    tuple_result = sum_iter_3(tuple(range(100)))
    if tuple_result == expected:
        print("  PASS  tuple deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  tuple deopt: got {tuple_result}, expected {expected}")
        failed += 1

    # ── Test 4: List-compiled, then generator input ───────────────────

    def sum_iter_4(iterable):
        total = 0
        for x in iterable:
            total += x
        return total

    print()
    print("Test 4: List-compiled, then generator deopt")
    for _ in range(WARMUP):
        sum_iter_4(list(range(50)))

    check_jit_compiled(sum_iter_4, "sum_iter_4")

    gen_result = sum_iter_4(x for x in range(100))
    if gen_result == expected:
        print("  PASS  generator deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  generator deopt: got {gen_result}, expected {expected}")
        failed += 1

    # ── Test 5: List-compiled, then dict.keys() input ─────────────────

    def sum_iter_5(iterable):
        total = 0
        for x in iterable:
            total += x
        return total

    print()
    print("Test 5: List-compiled, then dict.keys() deopt")
    for _ in range(WARMUP):
        sum_iter_5(list(range(50)))

    check_jit_compiled(sum_iter_5, "sum_iter_5")

    d = {i: None for i in range(100)}
    dict_result = sum_iter_5(d)  # iterates over keys
    if dict_result == expected:
        print("  PASS  dict deopt produces correct result")
        passed += 1
    else:
        print(f"  FAIL  dict deopt: got {dict_result}, expected {expected}")
        failed += 1

    # ── Test 6: Rapid alternation ─────────────────────────────────────

    def sum_iter_6(iterable):
        total = 0
        for x in iterable:
            total += x
        return total

    print()
    print("Test 6: Rapid alternation (list/range/tuple, 1000 cycles)")
    for _ in range(WARMUP):
        sum_iter_6(list(range(50)))

    check_jit_compiled(sum_iter_6, "sum_iter_6")

    alt_failures = 0
    for cycle in range(1000):
        inputs = [
            list(range(100)),
            range(100),
            tuple(range(100)),
        ]
        for inp in inputs:
            result = sum_iter_6(inp)
            if result != expected:
                print(f"  FAIL  alternation cycle {cycle}, type {type(inp).__name__}: "
                      f"got {result}, expected {expected}")
                alt_failures += 1
                break
        if alt_failures > 0:
            break

    if alt_failures == 0:
        print("  PASS  all 3000 calls correct across type alternation")
        passed += 1
    else:
        failed += 1

    # ── Test 7: Non-trivial loop body with deopt ──────────────────────

    def process_iter(iterable):
        """More complex loop body — ensures deopt restores full frame state."""
        results = []
        running_sum = 0
        for x in iterable:
            running_sum += x
            if x % 3 == 0:
                results.append(running_sum)
        return results, running_sum

    print()
    print("Test 7: Complex loop body with deopt")
    for _ in range(WARMUP):
        process_iter(list(range(30)))

    check_jit_compiled(process_iter, "process_iter")

    ref_list = process_iter(list(range(50)))
    ref_range = process_iter(range(50))
    ref_tuple = process_iter(tuple(range(50)))

    # All three should produce the same result
    if ref_list == ref_range == ref_tuple:
        print("  PASS  complex body produces identical results across types")
        passed += 1
    else:
        print(f"  FAIL  results diverge:")
        print(f"         list:  {ref_list}")
        print(f"         range: {ref_range}")
        print(f"         tuple: {ref_tuple}")
        failed += 1

    # ── Test 8: Nested loops with mixed types ─────────────────────────

    def nested_mixed(outer, inner):
        """Nested iteration with different types for outer and inner."""
        total = 0
        for i in outer:
            for j in inner:
                total += i * j
        return total

    print()
    print("Test 8: Nested loops with mixed iterator types")
    # Warm up with list/list
    for _ in range(WARMUP):
        nested_mixed(list(range(10)), list(range(10)))

    check_jit_compiled(nested_mixed, "nested_mixed")

    ref = nested_mixed(list(range(20)), list(range(20)))

    # Now try range/list, list/range, range/range, tuple/list
    combos = [
        (range(20), list(range(20)), "range/list"),
        (list(range(20)), range(20), "list/range"),
        (range(20), range(20), "range/range"),
        (tuple(range(20)), list(range(20)), "tuple/list"),
    ]

    combo_pass = True
    for outer, inner, desc in combos:
        result = nested_mixed(outer, inner)
        if result != ref:
            print(f"  FAIL  nested {desc}: got {result}, expected {ref}")
            combo_pass = False

    if combo_pass:
        print("  PASS  all nested combinations match")
        passed += 1
    else:
        failed += 1

    # ── Test 9: Exception handler inside loop with deopt ──────────

    def sum_with_try(iterable):
        """Loop with try/except — tests exception handler chain restoration
        after deopt. Bug 5 was caused by wrong bytecode offset in FrameState;
        if the except handler points to the wrong offset after deopt, this
        will either swallow real exceptions or miss the TypeError catch."""
        total = 0
        for x in iterable:
            try:
                total += x
            except TypeError:
                pass
        return total

    print()
    print("Test 9: Exception handler inside loop with deopt")
    for _ in range(WARMUP):
        sum_with_try(list(range(50)))

    check_jit_compiled(sum_with_try, "sum_with_try")

    # list input (compiled path)
    try_list = sum_with_try(list(range(100)))
    # range input (deopt path)
    try_range = sum_with_try(range(100))
    # tuple input (deopt path)
    try_tuple = sum_with_try(tuple(range(100)))

    if try_list == try_range == try_tuple == expected:
        print("  PASS  exception handler correct across all types after deopt")
        passed += 1
    else:
        print(f"  FAIL  try/except deopt:")
        print(f"         list:  {try_list}")
        print(f"         range: {try_range}")
        print(f"         tuple: {try_tuple}")
        print(f"         expected: {expected}")
        failed += 1

    # Also test that TypeError IS caught when it should be (mixed types)
    def sum_with_try_mixed(iterable):
        total = 0
        caught = 0
        for x in iterable:
            try:
                total += x
            except TypeError:
                caught += 1
        return total, caught

    print()
    print("Test 10: TypeError actually caught after deopt")
    for _ in range(WARMUP):
        sum_with_try_mixed(list(range(50)))

    check_jit_compiled(sum_with_try_mixed, "sum_with_try_mixed")

    mixed_input = list(range(10)) + ["not_a_number"] + list(range(10))
    ref_result = sum_with_try_mixed(mixed_input[:])  # interpreter-compiled path
    # Now with tuple (deopt)
    tuple_mixed = tuple(mixed_input)
    deopt_result = sum_with_try_mixed(tuple_mixed)

    if ref_result == deopt_result:
        print(f"  PASS  TypeError caught correctly (total={ref_result[0]}, caught={ref_result[1]})")
        passed += 1
    else:
        print(f"  FAIL  TypeError handling diverges after deopt:")
        print(f"         list:  {ref_result}")
        print(f"         tuple: {deopt_result}")
        failed += 1

    # ── Summary ───────────────────────────────────────────────────────

    print()
    print(f"Results: {passed} pass, {failed} fail (of {passed + failed} tests)")

    if failed > 0:
        print("VERDICT: FAIL — GuardType deopt produces incorrect results")
        sys.exit(1)
    else:
        print("VERDICT: PASS — polymorphic deopt is correct for all tested types")
        sys.exit(0)


if __name__ == "__main__":
    main()
