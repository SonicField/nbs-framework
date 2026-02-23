#!/usr/bin/env python3
"""
test_load_attr_method_with_values.py — Correctness and deopt tests for
LOAD_ATTR_METHOD_WITH_VALUES specialisation.

Targets: LOAD_ATTR_METHOD_WITH_VALUES.

LOAD_ATTR_METHOD_WITH_VALUES specialises attribute load operations
(obj.method) when the attribute resolves to an unbound method on the
type and the instance has an instance dictionary (tp_dictoffset != 0).
Instead of going through the full descriptor protocol, the specialisation
checks the type version, verifies no instance dict shadowing, and directly
loads the method from the type dict.

This is the most common method lookup specialisation — it applies to
regular class instances calling regular methods.

The adaptive specialiser emits LOAD_ATTR_METHOD_WITH_VALUES after
observing repeated method access on instances whose type has the method
in its MRO and instances have a __dict__.

Deopt triggers:
  - Object type changes (different class)
  - Method is shadowed by instance attribute
  - Method is deleted or replaced on the class
  - Type version changes (class modified)

Tests cover:
  - Basic method load and call
  - Method with arguments
  - Method returning self (chaining)
  - Method with different return types
  - Method with side effects (mutation)
  - Method raising exception
  - Inherited method
  - Overridden method in subclass
  - Deopt: switch to different class
  - Deopt: instance attribute shadows method
  - Deopt: switch to class with __getattr__
  - Method in loop
  - Multiple methods on same class
  - Method returning None
  - Method with closure (accessing enclosing scope)
  - Bound method identity
  - Deopt: method deleted from class
  - Rapid type alternation
  - Method call vs manual unbound call equivalence
  - Method on dynamically created class

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> check)
  2. Correct result after type change (deopt fires)
  3. Error handling preserved (AttributeError, custom exceptions)

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_load_attr_method_with_values.py
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
                f"{name} not JIT-compiled after {WARMUP} warmup calls. "
                "Test cannot verify JIT path — increase WARMUP or check "
                "cinderjit.auto() is enabled."
            )
        print(f"  WARNING: {name} not found in compiled functions — may not test JIT path")
        return False
    except (ImportError, AttributeError):
        return False


def main():
    print("=== LOAD_ATTR_METHOD_WITH_VALUES Correctness & Deopt Tests ===")
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

    # ------------------------------------------------------------------ #
    # Test 1: Basic method load and call
    # ------------------------------------------------------------------ #
    try:
        class Greeter:
            def __init__(self, name):
                self.name = name

            def greet(self):
                return f"hello {self.name}"

        def call_greet(obj):
            return obj.greet()

        g = Greeter("world")
        for _ in range(WARMUP):
            call_greet(g)

        check_jit_compiled(call_greet, "call_greet")
        result = call_greet(g)
        assert result == "hello world", f"Expected 'hello world', got {result}"
        g2 = Greeter("alex")
        assert call_greet(g2) == "hello alex"
        print("  PASS: test_basic_method_load_and_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_basic_method_load_and_call — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 2: Method with arguments
    # ------------------------------------------------------------------ #
    try:
        class Adder:
            def __init__(self, base):
                self.base = base

            def add(self, x, y):
                return self.base + x + y

        def call_add(obj, x, y):
            return obj.add(x, y)

        a = Adder(100)
        for _ in range(WARMUP):
            call_add(a, 1, 2)

        check_jit_compiled(call_add, "call_add")
        assert call_add(a, 10, 20) == 130, f"100+10+20=130"
        assert call_add(Adder(0), 3, 4) == 7
        print("  PASS: test_method_with_arguments")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_arguments — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 3: Method returning self (chaining)
    # ------------------------------------------------------------------ #
    try:
        class Builder:
            def __init__(self):
                self.parts = []

            def add(self, part):
                self.parts.append(part)
                return self

        def chain_add(obj, part):
            return obj.add(part)

        b = Builder()
        for _ in range(WARMUP):
            b.parts.clear()
            chain_add(b, "x")

        check_jit_compiled(chain_add, "chain_add")
        b.parts.clear()
        result = chain_add(b, "a")
        assert result is b, "Method returning self must preserve identity"
        assert b.parts == ["a"]
        # Chain
        chain_add(chain_add(b, "b"), "c")
        assert b.parts == ["a", "b", "c"]
        print("  PASS: test_method_returning_self")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_returning_self — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 4: Method with different return types
    # ------------------------------------------------------------------ #
    try:
        class TypeReturner:
            def __init__(self, val):
                self.val = val

            def get(self):
                return self.val

        def call_get(obj):
            return obj.get()

        obj_int = TypeReturner(42)
        for _ in range(WARMUP):
            call_get(obj_int)

        check_jit_compiled(call_get, "call_get")
        assert call_get(obj_int) == 42
        assert call_get(TypeReturner("hello")) == "hello"
        assert call_get(TypeReturner([1, 2])) == [1, 2]
        assert call_get(TypeReturner(None)) is None
        print("  PASS: test_method_different_return_types")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_different_return_types — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 5: Method with side effects (mutation)
    # ------------------------------------------------------------------ #
    try:
        class Accumulator:
            def __init__(self):
                self.total = 0
                self.call_count = 0

            def add(self, x):
                self.total += x
                self.call_count += 1
                return self.total

        def call_acc_add(obj, x):
            return obj.add(x)

        acc = Accumulator()
        for _ in range(WARMUP):
            acc.total = 0
            acc.call_count = 0
            call_acc_add(acc, 1)

        check_jit_compiled(call_acc_add, "call_acc_add")
        acc.total = 0
        acc.call_count = 0
        call_acc_add(acc, 10)
        call_acc_add(acc, 20)
        call_acc_add(acc, 30)
        assert acc.total == 60, f"Expected 60, got {acc.total}"
        assert acc.call_count == 3, f"Expected 3 calls, got {acc.call_count}"
        print("  PASS: test_method_with_side_effects")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_side_effects — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 6: Method raising exception
    # ------------------------------------------------------------------ #
    try:
        class Validator:
            def validate(self, x):
                if x < 0:
                    raise ValueError(f"negative: {x}")
                return x

        def call_validate(obj, x):
            return obj.validate(x)

        v = Validator()
        for _ in range(WARMUP):
            call_validate(v, 1)

        check_jit_compiled(call_validate, "call_validate")
        assert call_validate(v, 42) == 42

        caught = False
        try:
            call_validate(v, -1)
        except ValueError as ex:
            caught = True
            assert "negative" in str(ex)
        assert caught, "Method should raise ValueError for negative input"
        print("  PASS: test_method_raising_exception")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_raising_exception — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 7: Inherited method
    # ------------------------------------------------------------------ #
    try:
        class Base:
            def __init__(self, x):
                self.x = x

            def value(self):
                return self.x

        class Child(Base):
            pass

        def call_value(obj):
            return obj.value()

        child = Child(33)
        for _ in range(WARMUP):
            call_value(child)

        check_jit_compiled(call_value, "call_value")
        assert call_value(child) == 33
        base = Base(44)
        assert call_value(base) == 44
        print("  PASS: test_inherited_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inherited_method — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 8: Overridden method in subclass
    # ------------------------------------------------------------------ #
    try:
        class Parent:
            def __init__(self, x):
                self.x = x

            def compute(self):
                return self.x

        class Override(Parent):
            def compute(self):
                return self.x * 10

        def call_compute(obj):
            return obj.compute()

        p = Parent(5)
        for _ in range(WARMUP):
            call_compute(p)

        check_jit_compiled(call_compute, "call_compute")
        assert call_compute(p) == 5

        o = Override(5)
        assert call_compute(o) == 50, f"Override: 5*10=50, got {call_compute(o)}"
        print("  PASS: test_overridden_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overridden_method — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 9: Deopt — switch to different class
    # ------------------------------------------------------------------ #
    try:
        class ClassA:
            def __init__(self, x):
                self.x = x

            def get(self):
                return self.x + 1

        class ClassB:
            def __init__(self, x):
                self.x = x

            def get(self):
                return self.x * 3

        def call_get_9(obj):
            return obj.get()

        a = ClassA(10)
        for _ in range(WARMUP):
            call_get_9(a)

        check_jit_compiled(call_get_9, "call_get_9")
        assert call_get_9(a) == 11

        b = ClassB(10)
        assert call_get_9(b) == 30, f"ClassB: 10*3=30, got {call_get_9(b)}"
        print("  PASS: test_deopt_different_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 10: Deopt — instance attribute shadows method
    # ------------------------------------------------------------------ #
    try:
        class Shadowed:
            def __init__(self, x):
                self.x = x

            def get(self):
                return self.x

        def call_get_10(obj):
            return obj.get()

        s = Shadowed(42)
        for _ in range(WARMUP):
            call_get_10(s)

        check_jit_compiled(call_get_10, "call_get_10")
        assert call_get_10(s) == 42

        # Shadow the method with an instance attribute (callable)
        s.get = lambda: 999
        result = call_get_10(s)
        assert result == 999, f"Shadowed method should return 999, got {result}"
        print("  PASS: test_deopt_instance_shadows_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_instance_shadows_method — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 11: Deopt — switch to class with __getattr__
    # ------------------------------------------------------------------ #
    try:
        class Normal:
            def __init__(self, x):
                self.x = x

            def get(self):
                return self.x

        class WithGetattr:
            def __init__(self, x):
                self.x = x

            def __getattr__(self, name):
                if name == 'get':
                    return lambda: self.x * 5
                raise AttributeError(name)

        def call_get_11(obj):
            return obj.get()

        n = Normal(10)
        for _ in range(WARMUP):
            call_get_11(n)

        check_jit_compiled(call_get_11, "call_get_11")
        assert call_get_11(n) == 10

        wg = WithGetattr(10)
        assert call_get_11(wg) == 50, f"__getattr__ method: 10*5=50"
        print("  PASS: test_deopt_to_getattr_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_to_getattr_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 12: Method in loop
    # ------------------------------------------------------------------ #
    try:
        class LoopObj:
            def __init__(self, x):
                self.x = x

            def value(self):
                return self.x

        def sum_method_loop(obj, n):
            total = 0
            for _ in range(n):
                total += obj.value()
            return total

        lo = LoopObj(3)
        for _ in range(WARMUP):
            sum_method_loop(lo, 1)

        check_jit_compiled(sum_method_loop, "sum_method_loop")
        result = sum_method_loop(lo, 100)
        assert result == 300, f"3*100=300, got {result}"
        print("  PASS: test_method_in_loop")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_in_loop — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 13: Multiple methods on same class
    # ------------------------------------------------------------------ #
    try:
        class MultiMethod:
            def __init__(self, x, y):
                self.x = x
                self.y = y

            def get_x(self):
                return self.x

            def get_y(self):
                return self.y

            def get_sum(self):
                return self.x + self.y

        def call_all_methods(obj):
            return (obj.get_x(), obj.get_y(), obj.get_sum())

        mm = MultiMethod(3, 7)
        for _ in range(WARMUP):
            call_all_methods(mm)

        check_jit_compiled(call_all_methods, "call_all_methods")
        result = call_all_methods(mm)
        assert result == (3, 7, 10), f"Expected (3, 7, 10), got {result}"
        print("  PASS: test_multiple_methods")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_methods — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 14: Method returning None
    # ------------------------------------------------------------------ #
    try:
        class Void:
            def __init__(self):
                self.called = False

            def do_nothing(self):
                self.called = True
                # Implicit return None

        def call_void(obj):
            return obj.do_nothing()

        v = Void()
        for _ in range(WARMUP):
            v.called = False
            call_void(v)

        check_jit_compiled(call_void, "call_void")
        v.called = False
        result = call_void(v)
        assert result is None, f"Expected None, got {result}"
        assert v.called is True, "Side effect must occur"
        print("  PASS: test_method_returning_none")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_returning_none — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 15: Bound method identity
    # ------------------------------------------------------------------ #
    try:
        class IdCheck:
            def __init__(self, x):
                self.x = x

            def method(self):
                return self.x

        def get_bound_method(obj):
            return obj.method

        ic = IdCheck(42)
        for _ in range(WARMUP):
            get_bound_method(ic)

        check_jit_compiled(get_bound_method, "get_bound_method")

        m = get_bound_method(ic)
        # Bound method should be callable and return correct value
        assert m() == 42
        # Bound method's __self__ should be the instance
        assert m.__self__ is ic
        # Bound method's __func__ should be the unbound function
        assert m.__func__ is IdCheck.method
        print("  PASS: test_bound_method_identity")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_bound_method_identity — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 16: Deopt — method deleted from class
    # ------------------------------------------------------------------ #
    try:
        class Deletable:
            def __init__(self, x):
                self.x = x

            def get(self):
                return self.x

        def call_get_16(obj):
            return obj.get()

        d = Deletable(10)
        for _ in range(WARMUP):
            call_get_16(d)

        check_jit_compiled(call_get_16, "call_get_16")
        assert call_get_16(d) == 10

        # Delete the method from the class
        del Deletable.get

        caught = False
        try:
            call_get_16(d)
        except AttributeError:
            caught = True
        assert caught, "After deleting method, AttributeError expected"
        print("  PASS: test_deopt_method_deleted")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_method_deleted — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 17: Rapid type alternation
    # ------------------------------------------------------------------ #
    try:
        class TypeM:
            def __init__(self, x):
                self.x = x

            def val(self):
                return self.x

        class TypeN:
            def __init__(self, x):
                self.x = x

            def val(self):
                return self.x + 100

        def call_val(obj):
            return obj.val()

        tm = TypeM(1)
        for _ in range(WARMUP):
            call_val(tm)

        check_jit_compiled(call_val, "call_val")

        tn = TypeN(1)
        ok = True
        for i in range(50):
            rm = call_val(tm)
            rn = call_val(tn)
            if rm != 1:
                print(f"  FAIL: TypeM iteration {i}: expected 1, got {rm}")
                ok = False
                break
            if rn != 101:
                print(f"  FAIL: TypeN iteration {i}: expected 101, got {rn}")
                ok = False
                break

        if ok:
            print("  PASS: test_rapid_type_alternation")
            passed += 1
        else:
            failed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_type_alternation — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 18: Method call vs manual unbound call equivalence
    # ------------------------------------------------------------------ #
    try:
        class EquivObj:
            def __init__(self, x):
                self.x = x

            def get(self):
                return self.x

        def via_method(obj):
            return obj.get()

        obj = EquivObj(42)
        for _ in range(WARMUP):
            via_method(obj)

        check_jit_compiled(via_method, "via_method")

        unbound = EquivObj.get
        for val in [0, 1, -1, 100, 999]:
            o = EquivObj(val)
            method_result = via_method(o)
            unbound_result = unbound(o)
            assert method_result == unbound_result, (
                f"Mismatch for val={val}: method={method_result}, unbound={unbound_result}"
            )

        print("  PASS: test_method_vs_unbound_equivalence")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_vs_unbound_equivalence — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 19: Method on dynamically created class
    # ------------------------------------------------------------------ #
    try:
        def make_class(multiplier):
            def get(self):
                return self.x * multiplier
            return type('DynClass', (), {
                '__init__': lambda self, x: setattr(self, 'x', x),
                'get': get,
            })

        DynA = make_class(2)
        DynB = make_class(5)

        def call_dyn_get(obj):
            return obj.get()

        a = DynA(10)
        for _ in range(WARMUP):
            call_dyn_get(a)

        check_jit_compiled(call_dyn_get, "call_dyn_get")
        assert call_dyn_get(a) == 20, f"10*2=20, got {call_dyn_get(a)}"

        b = DynB(10)
        assert call_dyn_get(b) == 50, f"10*5=50, got {call_dyn_get(b)}"
        print("  PASS: test_method_on_dynamic_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_on_dynamic_class — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Test 20: Method with closure (accessing enclosing scope)
    # ------------------------------------------------------------------ #
    try:
        class ClosureObj:
            def __init__(self, x):
                self.x = x

        def make_method_with_closure(offset):
            class WithClosure:
                def __init__(self, x):
                    self.x = x

                def get(self):
                    return self.x + offset  # Captures 'offset'

            return WithClosure

        Cls10 = make_method_with_closure(10)
        Cls20 = make_method_with_closure(20)

        def call_closure_get(obj):
            return obj.get()

        c10 = Cls10(5)
        for _ in range(WARMUP):
            call_closure_get(c10)

        check_jit_compiled(call_closure_get, "call_closure_get")
        assert call_closure_get(c10) == 15, f"5+10=15"

        c20 = Cls20(5)
        assert call_closure_get(c20) == 25, f"5+20=25"
        print("  PASS: test_method_with_closure")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_with_closure — {e}")
        failed += 1

    # ------------------------------------------------------------------ #
    # Summary
    # ------------------------------------------------------------------ #
    print()
    print(f"LOAD_ATTR_METHOD_WITH_VALUES: {passed}/{passed + failed} passed, "
          f"{failed}/{passed + failed} failed")
    if failed == 0:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
