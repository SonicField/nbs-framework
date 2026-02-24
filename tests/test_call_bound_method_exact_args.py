#!/usr/bin/env python3
"""
test_call_bound_method_exact_args.py — Correctness and deopt tests for
CALL_BOUND_METHOD_EXACT_ARGS specialisation.

Targets: CALL_BOUND_METHOD_EXACT_ARGS.

CALL_BOUND_METHOD_EXACT_ARGS specialises calls to Python bound methods
where the number of arguments matches the function signature exactly
(no *args, no **kwargs, no defaults used). The adaptive interpreter
replaces the generic CALL opcode with this specialisation when it detects
repeated calls to the same bound method with a matching argument count.

Mechanism:
1. Adaptive interpreter detects CALL to a bound method (PyMethodObject)
2. Checks that the underlying function is a Python function (not C)
3. Checks that the argument count matches co_argcount exactly
4. Replaces CALL with CALL_BOUND_METHOD_EXACT_ARGS
5. CinderX JIT emits GuardType on the receiver + direct function entry

This avoids the overhead of:
  - Creating a temporary bound method object
  - Argument count validation
  - Generic call dispatch

Deopt triggers:
  - Receiver type changes (different class with same method name)
  - Method is overridden on instance or subclass
  - Argument count mismatch (extra/missing args)
  - Method replaced with a non-Python callable

Tests cover:
  - Method with no args (just self)
  - Method with one positional arg
  - Method with multiple positional args
  - Method returning self (fluent interface)
  - Method modifying instance state
  - Method calling another method on self
  - Inherited method (not overridden)
  - Overridden method in subclass
  - Deopt: different class with same method name
  - Deopt: instance method replaced at runtime
  - Deopt: subclass overrides method
  - Method with closure over instance variables
  - Rapid method calls (1000 cycles)
  - Stability — 10000 calls
  - Method on object with __slots__
  - Method returning computed value
  - Method with boolean logic
  - Chained method calls on same object
  - Multiple instances, same method
  - Equivalence: obj.method(args) vs Type.method(obj, args)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_bound_method_exact_args.py
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
                f"REQUIRE_JIT is set but {name} is NOT JIT-compiled. "
                f"Compiled functions: {[str(cf) for cf in compiled[:10]]}"
            )
        return False
    except ImportError:
        return False


def main():
    # --- CinderX bootstrap (skip gracefully if unavailable) ---
    try:
        import cinderx
        cinderx.init()
    except (ImportError, AttributeError):
        pass

    try:
        import cinderjit
        cinderjit.auto()
    except (ImportError, AttributeError):
        pass

    try:
        import cinderjit
        cinderjit.enable_specialized_opcodes()
    except (ImportError, AttributeError):
        pass

    passed = 0
    failed = 0

    # ------------------------------------------------------------------
    # Test 1: Method with no args (just self)
    # ------------------------------------------------------------------
    try:
        class Greeter:
            def __init__(self, name):
                self.name = name
            def greet(self):
                return f"Hello, {self.name}"

        def call_greet(obj):
            return obj.greet()

        g = Greeter("World")
        for _ in range(WARMUP):
            call_greet(g)
        check_jit_compiled(call_greet, "call_greet")

        assert call_greet(Greeter("Alice")) == "Hello, Alice"
        assert call_greet(Greeter("Bob")) == "Hello, Bob"
        assert call_greet(Greeter("")) == "Hello, "
        print("  PASS: test_method_no_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_no_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Method with one positional arg
    # ------------------------------------------------------------------
    try:
        class Adder:
            def __init__(self, base):
                self.base = base
            def add(self, x):
                return self.base + x

        def call_add(obj, x):
            return obj.add(x)

        a = Adder(10)
        for _ in range(WARMUP):
            call_add(a, 5)
        check_jit_compiled(call_add, "call_add")

        assert call_add(Adder(10), 5) == 15
        assert call_add(Adder(0), 100) == 100
        assert call_add(Adder(-5), 5) == 0
        assert call_add(Adder(0), 0) == 0
        print("  PASS: test_method_one_arg")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_one_arg — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Method with multiple positional args
    # ------------------------------------------------------------------
    try:
        class Calculator:
            def __init__(self):
                self.result = 0
            def compute(self, a, b, c):
                self.result = a * b + c
                return self.result

        def call_compute(obj, a, b, c):
            return obj.compute(a, b, c)

        calc = Calculator()
        for _ in range(WARMUP):
            call_compute(calc, 2, 3, 4)
        check_jit_compiled(call_compute, "call_compute")

        assert call_compute(Calculator(), 2, 3, 4) == 10
        assert call_compute(Calculator(), 0, 100, 0) == 0
        assert call_compute(Calculator(), -1, 5, 10) == 5
        assert call_compute(Calculator(), 10, 10, 10) == 110
        print("  PASS: test_method_multiple_args")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_multiple_args — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Method returning self (fluent interface)
    # ------------------------------------------------------------------
    try:
        class Builder:
            def __init__(self):
                self.parts = []
            def add(self, part):
                self.parts.append(part)
                return self

        def call_builder_add(obj, part):
            return obj.add(part)

        b = Builder()
        for _ in range(WARMUP):
            b.parts.clear()
            call_builder_add(b, "x")
        b.parts.clear()
        check_jit_compiled(call_builder_add, "call_builder_add")

        builder = Builder()
        result = call_builder_add(builder, "a")
        assert result is builder
        call_builder_add(result, "b")
        call_builder_add(result, "c")
        assert builder.parts == ["a", "b", "c"]
        print("  PASS: test_fluent_interface")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_fluent_interface — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Method modifying instance state
    # ------------------------------------------------------------------
    try:
        class Counter:
            def __init__(self):
                self.count = 0
            def increment(self):
                self.count += 1

        def call_increment(obj):
            obj.increment()

        c = Counter()
        for _ in range(WARMUP):
            call_increment(c)
        check_jit_compiled(call_increment, "call_increment")

        c2 = Counter()
        assert c2.count == 0
        call_increment(c2)
        assert c2.count == 1
        call_increment(c2)
        call_increment(c2)
        assert c2.count == 3
        print("  PASS: test_modifying_instance_state")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_modifying_instance_state — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Method calling another method on self
    # ------------------------------------------------------------------
    try:
        class Validator:
            def __init__(self, min_val, max_val):
                self.min_val = min_val
                self.max_val = max_val
            def in_range(self, x):
                return self.min_val <= x <= self.max_val
            def validate(self, x):
                if self.in_range(x):
                    return x
                return self.min_val if x < self.min_val else self.max_val

        def call_validate(obj, x):
            return obj.validate(x)

        v = Validator(0, 100)
        for _ in range(WARMUP):
            call_validate(v, 50)
        check_jit_compiled(call_validate, "call_validate")

        assert call_validate(Validator(0, 100), 50) == 50
        assert call_validate(Validator(0, 100), -10) == 0
        assert call_validate(Validator(0, 100), 200) == 100
        assert call_validate(Validator(10, 20), 10) == 10
        assert call_validate(Validator(10, 20), 20) == 20
        print("  PASS: test_method_calls_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_method_calls_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Inherited method (not overridden)
    # ------------------------------------------------------------------
    try:
        class Base:
            def __init__(self, val):
                self.val = val
            def double(self):
                return self.val * 2

        class Derived(Base):
            def __init__(self, val, extra):
                super().__init__(val)
                self.extra = extra

        def call_double(obj):
            return obj.double()

        d = Derived(5, "x")
        for _ in range(WARMUP):
            call_double(d)
        check_jit_compiled(call_double, "call_double")

        assert call_double(Derived(5, "x")) == 10
        assert call_double(Derived(0, "y")) == 0
        assert call_double(Derived(-3, "z")) == -6

        # Also works on base class
        assert call_double(Base(7)) == 14
        print("  PASS: test_inherited_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_inherited_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Overridden method in subclass
    # ------------------------------------------------------------------
    try:
        class Shape:
            def area(self):
                return 0

        class Square(Shape):
            def __init__(self, side):
                self.side = side
            def area(self):
                return self.side * self.side

        class Circle(Shape):
            def __init__(self, radius):
                self.radius = radius
            def area(self):
                return 3.14159 * self.radius * self.radius

        def call_area(obj):
            return obj.area()

        sq = Square(5)
        for _ in range(WARMUP):
            call_area(sq)
        check_jit_compiled(call_area, "call_area")

        assert call_area(Square(5)) == 25
        assert call_area(Square(1)) == 1
        assert call_area(Square(0)) == 0

        # Deopt to Circle (different subclass, different override)
        c_area = call_area(Circle(1))
        assert abs(c_area - 3.14159) < 0.001

        # Back to Square
        assert call_area(Square(10)) == 100

        # Base class
        assert call_area(Shape()) == 0
        print("  PASS: test_overridden_method_subclass")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_overridden_method_subclass — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt — different class with same method name
    # ------------------------------------------------------------------
    try:
        class Dog:
            def speak(self):
                return "woof"

        class Cat:
            def speak(self):
                return "meow"

        class Duck:
            def speak(self):
                return "quack"

        def call_speak(obj):
            return obj.speak()

        dog = Dog()
        for _ in range(WARMUP):
            call_speak(dog)
        check_jit_compiled(call_speak, "call_speak")

        assert call_speak(Dog()) == "woof"
        # Deopt: Cat
        assert call_speak(Cat()) == "meow"
        # Deopt: Duck
        assert call_speak(Duck()) == "quack"
        # Back to Dog
        assert call_speak(Dog()) == "woof"

        # Rapid alternation
        for _ in range(10):
            assert call_speak(Dog()) == "woof"
            assert call_speak(Cat()) == "meow"
            assert call_speak(Duck()) == "quack"
        print("  PASS: test_deopt_different_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_class — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt — instance method replaced at runtime
    # ------------------------------------------------------------------
    try:
        class Replaceable:
            def __init__(self, val):
                self.val = val
            def get(self):
                return self.val

        def call_get(obj):
            return obj.get()

        r = Replaceable(42)
        for _ in range(WARMUP):
            call_get(r)
        check_jit_compiled(call_get, "call_get")

        assert call_get(r) == 42

        # Replace method on instance
        r.get = lambda: 999
        assert call_get(r) == 999

        # New instance still has original method
        r2 = Replaceable(10)
        assert call_get(r2) == 10
        print("  PASS: test_deopt_instance_method_replaced")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_instance_method_replaced — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — subclass overrides method
    # ------------------------------------------------------------------
    try:
        class Parent:
            def value(self):
                return 1

        class Child(Parent):
            def value(self):
                return 2

        class GrandChild(Child):
            def value(self):
                return 3

        def call_value(obj):
            return obj.value()

        p = Parent()
        for _ in range(WARMUP):
            call_value(p)
        check_jit_compiled(call_value, "call_value")

        assert call_value(Parent()) == 1
        assert call_value(Child()) == 2
        assert call_value(GrandChild()) == 3
        assert call_value(Parent()) == 1
        print("  PASS: test_deopt_subclass_override")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_subclass_override — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Method with closure over instance variables
    # ------------------------------------------------------------------
    try:
        class Accumulator:
            def __init__(self):
                self.total = 0
                self.items = []
            def add(self, val):
                self.total += val
                self.items.append(val)
                return self.total

        def call_acc_add(obj, val):
            return obj.add(val)

        acc = Accumulator()
        for _ in range(WARMUP):
            call_acc_add(acc, 1)
        check_jit_compiled(call_acc_add, "call_acc_add")

        a = Accumulator()
        assert call_acc_add(a, 10) == 10
        assert call_acc_add(a, 20) == 30
        assert call_acc_add(a, 30) == 60
        assert a.items == [10, 20, 30]
        assert a.total == 60
        print("  PASS: test_closure_instance_vars")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_closure_instance_vars — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid method calls (1000 cycles)
    # ------------------------------------------------------------------
    try:
        class FastCounter:
            def __init__(self):
                self.n = 0
            def tick(self):
                self.n += 1

        def call_tick(obj):
            obj.tick()

        fc = FastCounter()
        for _ in range(WARMUP):
            call_tick(fc)
        check_jit_compiled(call_tick, "call_tick")

        fc2 = FastCounter()
        for _ in range(1000):
            call_tick(fc2)
        assert fc2.n == 1000
        print("  PASS: test_rapid_1000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_1000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Stability — 10000 calls
    # ------------------------------------------------------------------
    try:
        class Summation:
            def __init__(self):
                self.total = 0
            def add(self, x):
                self.total += x

        def call_sum_add(obj, x):
            obj.add(x)

        s = Summation()
        for _ in range(WARMUP):
            call_sum_add(s, 1)
        check_jit_compiled(call_sum_add, "call_sum_add")

        s2 = Summation()
        for i in range(10000):
            call_sum_add(s2, i)
        assert s2.total == sum(range(10000))  # 49995000
        print("  PASS: test_stability_10000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Method on object with __slots__
    # ------------------------------------------------------------------
    try:
        class SlottedPair:
            __slots__ = ("a", "b")
            def __init__(self, a, b):
                self.a = a
                self.b = b
            def swap(self):
                self.a, self.b = self.b, self.a
            def to_tuple(self):
                return (self.a, self.b)

        def call_swap(obj):
            obj.swap()

        def call_to_tuple(obj):
            return obj.to_tuple()

        sp = SlottedPair(1, 2)
        for _ in range(WARMUP):
            call_swap(sp)
        check_jit_compiled(call_swap, "call_swap")

        p = SlottedPair(10, 20)
        assert call_to_tuple(p) == (10, 20)
        call_swap(p)
        assert call_to_tuple(p) == (20, 10)
        call_swap(p)
        assert call_to_tuple(p) == (10, 20)
        print("  PASS: test_slots_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_slots_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Method returning computed value
    # ------------------------------------------------------------------
    try:
        class Vector2D:
            def __init__(self, x, y):
                self.x = x
                self.y = y
            def magnitude_sq(self):
                return self.x * self.x + self.y * self.y
            def dot(self, other):
                return self.x * other.x + self.y * other.y

        def call_mag_sq(obj):
            return obj.magnitude_sq()

        def call_dot(a, b):
            return a.dot(b)

        v = Vector2D(3, 4)
        for _ in range(WARMUP):
            call_mag_sq(v)
        check_jit_compiled(call_mag_sq, "call_mag_sq")

        assert call_mag_sq(Vector2D(3, 4)) == 25
        assert call_mag_sq(Vector2D(0, 0)) == 0
        assert call_mag_sq(Vector2D(1, 0)) == 1
        assert call_mag_sq(Vector2D(-3, -4)) == 25

        v1 = Vector2D(1, 0)
        v2 = Vector2D(0, 1)
        for _ in range(WARMUP):
            call_dot(v1, v2)
        check_jit_compiled(call_dot, "call_dot")

        assert call_dot(Vector2D(1, 0), Vector2D(0, 1)) == 0  # orthogonal
        assert call_dot(Vector2D(1, 2), Vector2D(3, 4)) == 11  # 1*3 + 2*4
        print("  PASS: test_computed_return_value")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_computed_return_value — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Method with boolean logic
    # ------------------------------------------------------------------
    try:
        class Range:
            def __init__(self, lo, hi):
                self.lo = lo
                self.hi = hi
            def contains(self, x):
                return self.lo <= x <= self.hi
            def overlaps(self, other):
                return self.lo <= other.hi and other.lo <= self.hi

        def call_contains(obj, x):
            return obj.contains(x)

        r = Range(10, 20)
        for _ in range(WARMUP):
            call_contains(r, 15)
        check_jit_compiled(call_contains, "call_contains")

        assert call_contains(Range(10, 20), 15) is True
        assert call_contains(Range(10, 20), 10) is True
        assert call_contains(Range(10, 20), 20) is True
        assert call_contains(Range(10, 20), 9) is False
        assert call_contains(Range(10, 20), 21) is False

        def call_overlaps(a, b):
            return a.overlaps(b)

        r1 = Range(0, 10)
        r2 = Range(5, 15)
        for _ in range(WARMUP):
            call_overlaps(r1, r2)
        check_jit_compiled(call_overlaps, "call_overlaps")

        assert call_overlaps(Range(0, 10), Range(5, 15)) is True
        assert call_overlaps(Range(0, 10), Range(11, 20)) is False
        assert call_overlaps(Range(0, 10), Range(10, 20)) is True
        print("  PASS: test_boolean_logic_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_boolean_logic_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained method calls on same object
    # ------------------------------------------------------------------
    try:
        class Pipeline:
            def __init__(self, val):
                self.val = val
            def add(self, n):
                self.val += n
                return self
            def mul(self, n):
                self.val *= n
                return self
            def neg(self):
                self.val = -self.val
                return self

        def run_pipeline(obj, a, m):
            return obj.add(a).mul(m).neg()

        p = Pipeline(0)
        for _ in range(WARMUP):
            p.val = 0
            run_pipeline(p, 5, 2)
        check_jit_compiled(run_pipeline, "run_pipeline")

        p1 = Pipeline(10)
        result = run_pipeline(p1, 5, 2)
        assert result is p1
        assert p1.val == -30  # (10 + 5) * 2 = 30, neg = -30

        p2 = Pipeline(0)
        run_pipeline(p2, 0, 100)
        assert p2.val == 0  # (0 + 0) * 100 = 0, neg = 0

        p3 = Pipeline(1)
        run_pipeline(p3, 1, 3)
        assert p3.val == -6  # (1 + 1) * 3 = 6, neg = -6
        print("  PASS: test_chained_methods")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_methods — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Multiple instances, same method
    # ------------------------------------------------------------------
    try:
        class Multiplier:
            def __init__(self, factor):
                self.factor = factor
            def apply(self, x):
                return x * self.factor

        def call_apply(obj, x):
            return obj.apply(x)

        m = Multiplier(2)
        for _ in range(WARMUP):
            call_apply(m, 5)
        check_jit_compiled(call_apply, "call_apply")

        instances = [Multiplier(i) for i in range(10)]
        for i, inst in enumerate(instances):
            result = call_apply(inst, 7)
            assert result == i * 7, f"Multiplier({i}).apply(7) = {result}, expected {i * 7}"

        # All instances use the same type — no deopt expected
        assert call_apply(Multiplier(100), 3) == 300
        assert call_apply(Multiplier(-1), 42) == -42
        print("  PASS: test_multiple_instances_same_method")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_multiple_instances_same_method — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 20: Equivalence — obj.method(args) vs Type.method(obj, args)
    # ------------------------------------------------------------------
    try:
        class Pair:
            def __init__(self, a, b):
                self.a = a
                self.b = b
            def sum(self):
                return self.a + self.b
            def scale(self, factor):
                return (self.a * factor, self.b * factor)

        def via_instance_sum(obj):
            return obj.sum()

        def via_type_sum(obj):
            return Pair.sum(obj)

        def via_instance_scale(obj, f):
            return obj.scale(f)

        def via_type_scale(obj, f):
            return Pair.scale(obj, f)

        p = Pair(3, 4)
        for _ in range(WARMUP):
            via_instance_sum(p)
            via_type_sum(p)
            via_instance_scale(p, 2)
            via_type_scale(p, 2)
        check_jit_compiled(via_instance_sum, "via_instance_sum")
        check_jit_compiled(via_type_sum, "via_type_sum")
        check_jit_compiled(via_instance_scale, "via_instance_scale")
        check_jit_compiled(via_type_scale, "via_type_scale")

        test_pairs = [Pair(1, 2), Pair(0, 0), Pair(-5, 5), Pair(100, 200)]
        for pair in test_pairs:
            s1 = via_instance_sum(pair)
            s2 = via_type_sum(pair)
            assert s1 == s2, f"Sum mismatch: {s1} vs {s2}"

            for f in [0, 1, 2, -1, 10]:
                sc1 = via_instance_scale(pair, f)
                sc2 = via_type_scale(pair, f)
                assert sc1 == sc2, f"Scale mismatch for factor {f}: {sc1} vs {sc2}"
        print("  PASS: test_equivalence_instance_vs_type_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_instance_vs_type_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_BOUND_METHOD_EXACT_ARGS: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
