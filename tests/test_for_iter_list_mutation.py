"""
test_for_iter_list_mutation — Correctness tests for FOR_ITER_LIST under mutation.

Targets: The FOR_ITER_LIST specialisation replaces generic InvokeIterNext with
CallStatic(JITRT_InvokeIterNext) when the iterator type is list_iterator.
The GuardType is emitted ONCE at GET_ITER (not per-iteration). If the list
is mutated during iteration, the underlying C list_iterator state may become
inconsistent with the fast-path assumptions.

These tests verify that JIT-compiled code produces IDENTICAL results to the
interpreter when the list is mutated during iteration. Each test runs first
without JIT (reference), then with JIT, and asserts equality.

CPython's list_iterator behaviour under mutation:
  - append: iteration continues, may or may not see appended elements
  - pop/del: iteration may skip elements or raise no error but produce
    different sequences than expected
  - clear: StopIteration on next iteration (length becomes 0)
  - assignment (lst[i] = x): iterator continues, sees new value

The key invariant: JIT must match interpreter behaviour EXACTLY, even if
that behaviour is surprising. A divergence means the fast path is unsound.

Usage:
  python3 test_for_iter_list_mutation.py           # Run all tests
  CINDERX_ROOT=~/local/cinderx_dev/cinderx ./test_cinderx.sh  # Via suite
"""

import sys


def get_interpreter_result(test_fn, *args):
    """Run test_fn without JIT to get reference result."""
    return test_fn(*args)


def test_append_during_iteration():
    """Append to list during iteration. CPython allows this — iterator may
    see appended elements depending on internal index vs len check."""
    def iterate_with_append(lst):
        results = []
        count = 0
        for x in lst:
            results.append(x)
            if count == 2:
                lst.append(999)
            count += 1
        return results

    # Reference: interpreter result
    ref = iterate_with_append(list(range(5)))

    return "append_during_iteration", ref


def test_pop_during_iteration():
    """Pop from end of list during iteration. Shortens the list, so the
    iterator's internal index may exceed the new length."""
    def iterate_with_pop(lst):
        results = []
        for x in lst:
            results.append(x)
            if len(lst) > 3:
                lst.pop()
        return results

    ref = iterate_with_pop(list(range(10)))
    return "pop_during_iteration", ref


def test_del_during_iteration():
    """Delete element at current position during iteration. The iterator
    index advances but elements shift, causing a skip."""
    def iterate_with_del(lst):
        results = []
        i = 0
        for x in lst:
            results.append(x)
            if i == 1 and len(lst) > 3:
                del lst[0]
            i += 1
        return results

    ref = iterate_with_del(list(range(8)))
    return "del_during_iteration", ref


def test_clear_during_iteration():
    """Clear the entire list during iteration. Should stop iteration
    immediately (or on next step)."""
    def iterate_with_clear(lst):
        results = []
        for x in lst:
            results.append(x)
            if len(results) == 2:
                lst.clear()
        return results

    ref = iterate_with_clear(list(range(10)))
    return "clear_during_iteration", ref


def test_assignment_during_iteration():
    """Assign to elements during iteration. Iterator should see modified values
    for elements it hasn't visited yet."""
    def iterate_with_assign(lst):
        results = []
        for i, x in enumerate(lst):
            results.append(x)
            if i == 0:
                for j in range(1, len(lst)):
                    lst[j] = lst[j] * 10
        return results

    ref = iterate_with_assign(list(range(1, 6)))
    return "assignment_during_iteration", ref


def test_insert_during_iteration():
    """Insert into the list during iteration. Shifts elements right,
    so the iterator may re-visit elements."""
    def iterate_with_insert(lst):
        results = []
        inserted = False
        for x in lst:
            results.append(x)
            if x == 2 and not inserted:
                lst.insert(0, -1)
                inserted = True
        return results

    ref = iterate_with_insert(list(range(5)))
    return "insert_during_iteration", ref


def test_extend_during_iteration():
    """Extend the list during iteration. Similar to append but adds
    multiple elements at once."""
    def iterate_with_extend(lst):
        results = []
        extended = False
        for x in lst:
            results.append(x)
            if x == 2 and not extended:
                lst.extend([100, 200, 300])
                extended = True
        return results

    ref = iterate_with_extend(list(range(4)))
    return "extend_during_iteration", ref


def test_empty_list_iteration():
    """Iterate over an empty list. Forces immediate StopIteration on the
    first call to tp_iternext. If the TOptObject->TObject type change
    (gatekeeper observation 1) caused the optimiser to elide the NULL
    sentinel check, this will hang or segfault."""
    def iterate_empty(lst):
        results = []
        for x in lst:
            results.append(x)
        return results

    ref = iterate_empty([])
    return "empty_list_iteration", ref


def test_single_element_list():
    """Iterate over a single-element list. The second tp_iternext call
    returns the StopIteration sentinel (NULL). Tests the boundary between
    'has elements' and 'done'."""
    def iterate_single(lst):
        results = []
        for x in lst:
            results.append(x)
        return results

    ref = iterate_single([42])
    return "single_element_iteration", ref


# All test functions that produce (name, reference_result) pairs
INTERPRETER_TESTS = [
    test_append_during_iteration,
    test_pop_during_iteration,
    test_del_during_iteration,
    test_clear_during_iteration,
    test_assignment_during_iteration,
    test_insert_during_iteration,
    test_extend_during_iteration,
    test_empty_list_iteration,
    test_single_element_list,
]


def main():
    """Run each test, comparing interpreter vs JIT results.

    Phase 1: Collect interpreter reference results (no JIT).
    Phase 2: Enable JIT, warm up, and verify JIT matches interpreter.
    """
    print("=== FOR_ITER_LIST Mutation Tests ===")
    print()

    # Phase 1: Interpreter reference results
    print("Phase 1: Collecting interpreter reference results...")
    references = {}
    for test_fn in INTERPRETER_TESTS:
        name, ref = test_fn()
        references[name] = ref
        print(f"  {name}: {ref}")

    # Phase 2: JIT — import cinderx and enable JIT
    print()
    print("Phase 2: JIT compilation and verification...")

    try:
        import cinderx
        cinderx.init()
        import cinderjit
        cinderjit.auto()
        # Enable specialised opcodes — FOR_ITER_LIST is gated behind this flag
        try:
            cinderjit.enable_specialized_opcodes()
        except AttributeError:
            pass  # Older builds may not have this
    except ImportError:
        print("SKIP — cinderx/cinderjit not available")
        sys.exit(0)

    # Define the mutation functions again so JIT compiles fresh versions
    def jit_append(lst):
        results = []
        count = 0
        for x in lst:
            results.append(x)
            if count == 2:
                lst.append(999)
            count += 1
        return results

    def jit_pop(lst):
        results = []
        for x in lst:
            results.append(x)
            if len(lst) > 3:
                lst.pop()
        return results

    def jit_del(lst):
        results = []
        i = 0
        for x in lst:
            results.append(x)
            if i == 1 and len(lst) > 3:
                del lst[0]
            i += 1
        return results

    def jit_clear(lst):
        results = []
        for x in lst:
            results.append(x)
            if len(results) == 2:
                lst.clear()
        return results

    def jit_assign(lst):
        results = []
        for i, x in enumerate(lst):
            results.append(x)
            if i == 0:
                for j in range(1, len(lst)):
                    lst[j] = lst[j] * 10
        return results

    def jit_insert(lst):
        results = []
        inserted = False
        for x in lst:
            results.append(x)
            if x == 2 and not inserted:
                lst.insert(0, -1)
                inserted = True
        return results

    def jit_extend(lst):
        results = []
        extended = False
        for x in lst:
            results.append(x)
            if x == 2 and not extended:
                lst.extend([100, 200, 300])
                extended = True
        return results

    def jit_empty(lst):
        results = []
        for x in lst:
            results.append(x)
        return results

    def jit_single(lst):
        results = []
        for x in lst:
            results.append(x)
        return results

    jit_fns = [
        ("append_during_iteration", jit_append, lambda: list(range(5))),
        ("pop_during_iteration", jit_pop, lambda: list(range(10))),
        ("del_during_iteration", jit_del, lambda: list(range(8))),
        ("clear_during_iteration", jit_clear, lambda: list(range(10))),
        ("assignment_during_iteration", jit_assign, lambda: list(range(1, 6))),
        ("insert_during_iteration", jit_insert, lambda: list(range(5))),
        ("extend_during_iteration", jit_extend, lambda: list(range(4))),
        ("empty_list_iteration", jit_empty, lambda: []),
        ("single_element_iteration", jit_single, lambda: [42]),
    ]

    # Warm up all functions (need 10000+ calls for JIT compilation with cinderjit.auto())
    print("  Warming up (15000 calls per function)...")
    for name, fn, make_input in jit_fns:
        for _ in range(15000):
            fn(make_input())

    # Verify JIT compilation where possible
    for name, fn, _ in jit_fns:
        try:
            compiled = cinderjit.is_jit_compiled(fn)
            print(f"  {name}: jit_compiled={compiled}")
        except AttributeError:
            pass

    # Test: JIT results must match interpreter references
    print()
    passed = 0
    failed = 0
    for name, fn, make_input in jit_fns:
        jit_result = fn(make_input())
        ref = references[name]

        if jit_result == ref:
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"         interpreter: {ref}")
            print(f"         jit:         {jit_result}")
            failed += 1

    # Stability: run each test 100 more times to check for intermittent failures
    print()
    print("  Stability check (100 repetitions per test)...")
    for name, fn, make_input in jit_fns:
        ref = references[name]
        for rep in range(100):
            result = fn(make_input())
            if result != ref:
                print(f"  FAIL  {name} at repetition {rep}")
                print(f"         interpreter: {ref}")
                print(f"         jit:         {result}")
                failed += 1
                break

    print()
    print(f"Results: {passed} pass, {failed} fail (of {len(jit_fns)} tests)")

    if failed > 0:
        print("VERDICT: FAIL — JIT diverges from interpreter under list mutation")
        sys.exit(1)
    else:
        print("VERDICT: PASS — JIT matches interpreter for all mutation patterns")
        sys.exit(0)


if __name__ == "__main__":
    main()
