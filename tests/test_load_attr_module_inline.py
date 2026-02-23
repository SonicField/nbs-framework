"""
test_load_attr_module_inline — Correctness tests for LOAD_ATTR_MODULE inline
dict access specialisation.

Targets: The LOAD_ATTR_MODULE inline fast path replaces the IC-based
LoadModuleAttrCached approach with direct dict entry access:
1. GuardType receiver is PyModule_Type
2. Load md_dict -> ma_keys -> dk_version
3. Compare dk_version against CPython IC cached version
4. Guard on version match
5. Call JITRT_LoadModuleDictEntry(keys, index) for direct entry access
6. CheckField on result (deopt if NULL = deleted attr)

This bypasses the IC entirely for a direct memory path. The guard depends on
dk_version being invalidated whenever the dict contents change. CPython
guarantees this: dk_version is incremented on any mutation (insert, delete,
update) and on dict resize.

These tests verify that JIT-compiled code produces IDENTICAL results to the
interpreter when module attributes are accessed, mutated, deleted, or when
the dict is resized. Each test compares JIT output against a known reference.

Usage:
  python3 test_load_attr_module_inline.py
"""

import sys
import types


WARMUP = 15000  # CinderX auto-compilation typically needs 10000+ calls

# Set to True to require JIT compilation when cinderjit is available.
# When True, tests FAIL if the function is not compiled — avoids false
# confidence from interpreter-only execution.
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


def make_test_module(name, **attrs):
    """Create a fresh module with given attributes."""
    mod = types.ModuleType(name)
    for k, v in attrs.items():
        setattr(mod, k, v)
    return mod


def main():
    print("=== LOAD_ATTR_MODULE Inline Dict Access Tests ===")
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

    # ── Test 1: Basic correctness — access module attr under JIT ──────

    import math

    def get_pi():
        return math.pi

    def get_e():
        return math.e

    print("Test 1: Basic correctness — math.pi and math.e under JIT")

    # Get interpreter reference
    ref_pi = math.pi
    ref_e = math.e

    # Warm up for JIT compilation
    for _ in range(15000):
        get_pi()
        get_e()

    check_jit_compiled(get_pi, "get_pi")
    check_jit_compiled(get_e, "get_e")

    jit_pi = get_pi()
    jit_e = get_e()

    if jit_pi == ref_pi and jit_e == ref_e:
        print("  PASS  basic module attr access matches interpreter")
        passed += 1
    else:
        print(f"  FAIL  pi: {jit_pi} vs {ref_pi}, e: {jit_e} vs {ref_e}")
        failed += 1

    # ── Test 2: Module attribute mutation — change value after JIT ─────

    test_mod = make_test_module("test_mod", value=42, label="original")

    def get_value(mod):
        return mod.value

    print()
    print("Test 2: Module attribute mutation after JIT compilation")

    # Warm up with original value
    for _ in range(15000):
        get_value(test_mod)

    check_jit_compiled(get_value, "get_value")

    # Verify original value
    result1 = get_value(test_mod)
    assert result1 == 42, f"Pre-mutation: {result1}"

    # Mutate the module attribute (dk_version should change)
    test_mod.value = 99

    # JIT-compiled function should see the new value (guard fires, deopts)
    result2 = get_value(test_mod)

    if result2 == 99:
        print("  PASS  JIT sees mutated module attribute (deopt on dk_version change)")
        passed += 1
    else:
        print(f"  FAIL  expected 99, got {result2} (stale dict entry?)")
        failed += 1

    # ── Test 3: Module attribute deletion — del attr after JIT ────────

    del_mod = make_test_module("del_mod", target=100, keep=200)

    def get_target(mod):
        return mod.target

    def get_keep(mod):
        return mod.keep

    print()
    print("Test 3: Module attribute deletion after JIT compilation")

    for _ in range(15000):
        get_target(del_mod)
        get_keep(del_mod)

    # Delete the target attribute
    del del_mod.target

    # Access should raise AttributeError (CheckField should deopt, then
    # interpreter raises the error)
    try:
        result = get_target(del_mod)
        print(f"  FAIL  expected AttributeError, got {result}")
        failed += 1
    except AttributeError:
        print("  PASS  AttributeError raised after attribute deletion")
        passed += 1

    # Other attributes should still work
    keep_result = get_keep(del_mod)
    if keep_result == 200:
        print("  PASS  other attributes unaffected by deletion")
        passed += 1
    else:
        print(f"  FAIL  keep attr: expected 200, got {keep_result}")
        failed += 1

    # ── Test 4: Dict resize — add many attributes to force resize ─────

    resize_mod = make_test_module("resize_mod", original=42)

    def get_original(mod):
        return mod.original

    print()
    print("Test 4: Dict resize — force ma_keys reallocation")

    for _ in range(15000):
        get_original(resize_mod)

    # Verify pre-resize
    pre_result = get_original(resize_mod)
    assert pre_result == 42

    # Add many attributes to force dict resize (CPython dicts resize at
    # 2/3 capacity; initial capacity is 8, so adding ~10 attributes should
    # trigger at least one resize)
    for i in range(50):
        setattr(resize_mod, f"padding_{i}", i)

    # Original attribute should still be accessible (guard fires on
    # dk_version change, deopts to interpreter which re-does the lookup)
    post_result = get_original(resize_mod)

    if post_result == 42:
        print("  PASS  original attribute accessible after dict resize")
        passed += 1
    else:
        print(f"  FAIL  expected 42, got {post_result}")
        failed += 1

    # ── Test 5: Multiple mutations — rapid attribute changes ──────────

    rapid_mod = make_test_module("rapid_mod", counter=0)

    def get_counter(mod):
        return mod.counter

    print()
    print("Test 5: Rapid attribute mutations (1000 cycles)")

    for _ in range(15000):
        get_counter(rapid_mod)

    rapid_failures = 0
    for i in range(1000):
        rapid_mod.counter = i
        result = get_counter(rapid_mod)
        if result != i:
            print(f"  FAIL  cycle {i}: expected {i}, got {result}")
            rapid_failures += 1
            break

    if rapid_failures == 0:
        print("  PASS  all 1000 mutations correctly observed")
        passed += 1
    else:
        failed += 1

    # ── Test 6: New attribute added after JIT compilation ─────────────

    new_attr_mod = make_test_module("new_attr_mod", existing=10)

    def get_new_attr(mod):
        return mod.new_attr

    print()
    print("Test 6: Access newly added attribute after JIT compilation")

    # First, verify it raises AttributeError before the attr exists
    try:
        get_new_attr(new_attr_mod)
        print("  FAIL  should raise AttributeError before attr exists")
        failed += 1
    except AttributeError:
        pass  # Expected

    # Now add the attribute
    new_attr_mod.new_attr = 777

    # Warm up with the new attribute present
    for _ in range(15000):
        get_new_attr(new_attr_mod)

    result = get_new_attr(new_attr_mod)
    if result == 777:
        print("  PASS  newly added attribute accessible under JIT")
        passed += 1
    else:
        print(f"  FAIL  expected 777, got {result}")
        failed += 1

    # ── Test 7: Module replacement — different module same function ────

    mod_a = make_test_module("mod_a", shared=100)
    mod_b = make_test_module("mod_b", shared=200)

    def get_shared(mod):
        return mod.shared

    print()
    print("Test 7: Different modules through same function")

    # Warm up with mod_a
    for _ in range(15000):
        get_shared(mod_a)

    result_a = get_shared(mod_a)
    result_b = get_shared(mod_b)

    if result_a == 100 and result_b == 200:
        print("  PASS  different modules produce correct values")
        passed += 1
    else:
        print(f"  FAIL  mod_a={result_a} (expected 100), mod_b={result_b} (expected 200)")
        failed += 1

    # ── Test 8: Attribute type change ─────────────────────────────────

    type_mod = make_test_module("type_mod", val=42)

    def get_val(mod):
        return mod.val

    print()
    print("Test 8: Attribute value type change (int -> str -> list)")

    for _ in range(15000):
        get_val(type_mod)

    # Change type: int -> str
    type_mod.val = "hello"
    str_result = get_val(type_mod)

    # Change type: str -> list
    type_mod.val = [1, 2, 3]
    list_result = get_val(type_mod)

    if str_result == "hello" and list_result == [1, 2, 3]:
        print("  PASS  type changes correctly observed")
        passed += 1
    else:
        print(f"  FAIL  str={str_result}, list={list_result}")
        failed += 1

    # ── Test 9: Stability — repeated access under JIT (no mutation) ───

    import os

    def get_sep():
        return os.sep

    print()
    print("Test 9: Stability — 10000 accesses to os.sep under JIT")

    for _ in range(15000):
        get_sep()

    ref_sep = os.sep
    stability_failures = 0
    for i in range(10000):
        result = get_sep()
        if result != ref_sep:
            print(f"  FAIL  iteration {i}: got {result}, expected {ref_sep}")
            stability_failures += 1
            break

    if stability_failures == 0:
        print("  PASS  10000 stable accesses")
        passed += 1
    else:
        failed += 1

    # ── Summary ───────────────────────────────────────────────────────

    print()
    print(f"Results: {passed} pass, {failed} fail (of {passed + failed} tests)")

    if failed > 0:
        print("VERDICT: FAIL — LOAD_ATTR_MODULE inline dict access produces incorrect results")
        sys.exit(1)
    else:
        print("VERDICT: PASS — LOAD_ATTR_MODULE inline dict access is correct")
        sys.exit(0)


if __name__ == "__main__":
    main()
