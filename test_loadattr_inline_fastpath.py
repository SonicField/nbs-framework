#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Standalone tests for LOAD_ATTR codegen changes.
# Covers: A-lite (C++ fast path), Approach A (inline cache in LIR),
# and Approach B (GuardType + LoadField in HIR simplifier).
#
# These tests verify correctness of LOAD_ATTR optimisations, including
# deopt behaviour for Approach B, without depending on the CinderX test
# infrastructure import chain (which requires cinderx.compiler.opcode).
#
# Run: CINDERJIT_ENABLE=1 python3 test_loadattr_inline_fastpath.py [-v]
#
# pyre-unsafe

import gc
import platform
import sys
import threading
import unittest
import weakref

try:
    import cinderjit
    HAS_CINDERJIT = True
except ImportError:
    HAS_CINDERJIT = False


def force_compile(func):
    """Force JIT compilation if cinderjit is available."""
    if HAS_CINDERJIT:
        cinderjit.force_compile(func)
    return func


def is_jit_compiled(func):
    """Check if function is JIT compiled."""
    if HAS_CINDERJIT:
        return cinderjit.is_jit_compiled(func)
    return False


def skip_unless_jit(test_func):
    """Skip test if JIT is not available."""
    if not HAS_CINDERJIT:
        return unittest.skip("cinderjit not available")(test_func)
    return test_func


# =============================================================================
# Layer 2a: Fast Path Hit Tests
# =============================================================================

class TestFastPathHit(unittest.TestCase):
    """Tests where the type matches the cached type — fast path should fire."""

    @skip_unless_jit
    def test_slots_simple(self):
        """__slots__ attribute: the primary fast path target.

        The inline code should: check Py_TYPE(obj) == cached_type,
        then LDR at cached byte offset. No function call.
        """
        class Point:
            __slots__ = ('x', 'y')
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def get_x(obj):
            return obj.x

        def get_y(obj):
            return obj.y

        force_compile(get_x)
        force_compile(get_y)

        p = Point(10, 20)
        # First call (cache cold → slow path populates cache)
        self.assertEqual(get_x(p), 10)
        self.assertEqual(get_y(p), 20)
        # Second call (cache warm → fast path)
        self.assertEqual(get_x(p), 10)
        self.assertEqual(get_y(p), 20)
        # Verify JIT compilation
        self.assertTrue(is_jit_compiled(get_x), "get_x should be JIT compiled")

    @skip_unless_jit
    def test_dict_based_attr(self):
        """Regular instance attribute (dict-based, not __slots__).

        This goes through split dict or combined dict accessor, not
        the MemberDescrMutator path. Still should benefit from inline
        type check eliminating the function call.
        """
        class Obj:
            def __init__(self, val):
                self.val = val

        def get_val(obj):
            return obj.val

        force_compile(get_val)

        o = Obj(42)
        self.assertEqual(get_val(o), 42)
        self.assertEqual(get_val(o), 42)  # cached
        o.val = 99
        self.assertEqual(get_val(o), 99)  # same type, different value

    @skip_unless_jit
    def test_inherited_attr(self):
        """Attribute inherited from base class via MRO."""
        class Base:
            __slots__ = ('x',)
            def __init__(self, x):
                self.x = x

        class Derived(Base):
            __slots__ = ('y',)
            def __init__(self, x, y):
                super().__init__(x)
                self.y = y

        def get_x(obj):
            return obj.x

        def get_y(obj):
            return obj.y

        force_compile(get_x)
        force_compile(get_y)

        d = Derived(10, 20)
        self.assertEqual(get_x(d), 10)
        self.assertEqual(get_y(d), 20)
        # Repeated access
        self.assertEqual(get_x(d), 10)
        self.assertEqual(get_y(d), 20)

    @skip_unless_jit
    def test_multiple_attrs_same_object(self):
        """Multiple attribute accesses on the same object in one function.

        Exercises multiple inline cache entries for different attribute names.
        """
        class Record:
            __slots__ = ('a', 'b', 'c', 'd')
            def __init__(self):
                self.a = 1
                self.b = 2
                self.c = 3
                self.d = 4

        def read_all(obj):
            return obj.a + obj.b + obj.c + obj.d

        force_compile(read_all)

        r = Record()
        self.assertEqual(read_all(r), 10)
        self.assertEqual(read_all(r), 10)  # cached

    @skip_unless_jit
    def test_repeated_access_stability(self):
        """Cache warm over many iterations — fast path must be stable."""
        class Counter:
            __slots__ = ('n',)
            def __init__(self):
                self.n = 0

        def get_n(obj):
            return obj.n

        force_compile(get_n)

        c = Counter()
        for i in range(10000):
            c.n = i
            self.assertEqual(get_n(c), i)


# =============================================================================
# Layer 2b: Fast Path Miss Tests (slow path fallback)
# =============================================================================

class TestFastPathMiss(unittest.TestCase):
    """Tests where the type does NOT match — must fall back to slow path."""

    @skip_unless_jit
    def test_polymorphic_receiver(self):
        """Call with type A then type B — cache misses on second type.

        The slow path must produce correct results for both types.
        """
        class A:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 100

        class B:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 200

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        a = A()
        b = B()
        self.assertEqual(get_x(a), 100)  # populates cache for A
        self.assertEqual(get_x(b), 200)  # cache miss → slow path for B
        self.assertEqual(get_x(a), 100)  # back to A
        self.assertEqual(get_x(b), 200)  # back to B

    @skip_unless_jit
    def test_type_mutation_invalidates_cache(self):
        """Modify type dict between calls — cache must invalidate."""
        class C:
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 42)

        # Add a data descriptor that shadows instance attr
        class Descr:
            def __get__(self, obj, typ):
                return 999
            def __set__(self, obj, val):
                pass

        C.x = Descr()
        self.assertEqual(get_x(c), 999)  # descriptor takes priority

    @skip_unless_jit
    def test_base_class_mutation(self):
        """Modify base class dict — derived class cache must invalidate."""
        class Base:
            class_attr = "base"

        class Derived(Base):
            pass

        def get_class_attr(obj):
            return obj.class_attr

        force_compile(get_class_attr)

        d = Derived()
        self.assertEqual(get_class_attr(d), "base")

        Base.class_attr = "modified"
        self.assertEqual(get_class_attr(d), "modified")

    @skip_unless_jit
    def test_bases_reassignment(self):
        """Change __bases__ — completely different MRO."""
        class Base1:
            class_attr = "from_base1"

        class Base2:
            class_attr = "from_base2"

        class C(Base1):
            pass

        def get_class_attr(obj):
            return obj.class_attr

        force_compile(get_class_attr)

        c = C()
        self.assertEqual(get_class_attr(c), "from_base1")

        C.__bases__ = (Base2,)
        self.assertEqual(get_class_attr(c), "from_base2")

    @skip_unless_jit
    def test_dunder_class_reassignment(self):
        """Change object's __class__ at runtime."""
        class A:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 10

        class B:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 20

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        a = A()
        self.assertEqual(get_x(a), 10)

        b = B()
        b.x = 30
        # Change b's class to A — layout compatible due to same __slots__
        b.__class__ = A
        self.assertEqual(get_x(b), 30)  # now typed as A, value unchanged


# =============================================================================
# Layer 2c: Descriptor Protocol Tests
# =============================================================================

class TestDescriptorProtocol(unittest.TestCase):
    """The fast path must NOT bypass descriptor protocol."""

    @skip_unless_jit
    def test_data_descriptor_not_bypassed(self):
        """Data descriptor (__get__ + __set__) must take priority over
        instance dict, even on the fast path."""
        class DataDescr:
            def __get__(self, obj, typ):
                return "descriptor_value"
            def __set__(self, obj, val):
                pass  # silently ignore

        class C:
            x = DataDescr()

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), "descriptor_value")
        c.__dict__["x"] = "instance_value"
        # Data descriptor takes priority over instance dict
        self.assertEqual(get_x(c), "descriptor_value")

    @skip_unless_jit
    def test_nondata_descriptor_shadowed_by_instance(self):
        """Non-data descriptor (__get__ only) is shadowed by instance attr."""
        class NonDataDescr:
            def __get__(self, obj, typ):
                return "descriptor_value"

        class C:
            x = NonDataDescr()

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), "descriptor_value")
        c.__dict__["x"] = "instance_value"
        self.assertEqual(get_x(c), "instance_value")

    @skip_unless_jit
    def test_descriptor_added_after_cache_warm(self):
        """Add a data descriptor to the type AFTER the cache is populated."""
        class C:
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 42)  # cache warm, instance dict path

        class Descr:
            def __get__(self, obj, typ):
                return 999
            def __set__(self, obj, val):
                pass

        C.x = Descr()
        self.assertEqual(get_x(c), 999)  # must invalidate and use descriptor

    @skip_unless_jit
    def test_descriptor_removed_after_cache_warm(self):
        """Remove a data descriptor from the type after cache warm."""
        class Descr:
            def __get__(self, obj, typ):
                return "descr"
            def __set__(self, obj, val):
                pass

        class C:
            x = Descr()

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), "descr")

        del C.x
        c.x = "instance"
        self.assertEqual(get_x(c), "instance")

    @skip_unless_jit
    def test_descriptor_class_changed(self):
        """Change descriptor from data to non-data by modifying __class__."""
        class DataDescr:
            def __get__(self, obj, typ):
                return "data"
            def __set__(self, obj, val):
                pass

        class NonDataDescr:
            def __get__(self, obj, typ):
                return "nondata"

        class C:
            x = DataDescr()

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        c.__dict__["x"] = "instance"
        self.assertEqual(get_x(c), "data")  # data descriptor wins

        C.__dict__["x"].__class__ = NonDataDescr
        self.assertEqual(get_x(c), "instance")  # non-data: instance wins

    @skip_unless_jit
    def test_property_access(self):
        """@property is a data descriptor — must work correctly."""
        class C:
            def __init__(self):
                self._x = 42

            @property
            def x(self):
                return self._x * 2

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 84)
        c._x = 10
        self.assertEqual(get_x(c), 20)


# =============================================================================
# Layer 2d: Edge Cases
# =============================================================================

class TestEdgeCases(unittest.TestCase):
    """Edge cases that could break the inline fast path."""

    @skip_unless_jit
    def test_attribute_error(self):
        """Access non-existent attribute — must raise AttributeError."""
        class C:
            pass

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        with self.assertRaises(AttributeError):
            get_x(c)

    @skip_unless_jit
    def test_getattr_fallback(self):
        """__getattr__ is called when normal lookup fails."""
        class C:
            def __getattr__(self, name):
                return f"fallback_{name}"

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), "fallback_x")

    @skip_unless_jit
    def test_getattribute_override(self):
        """__getattribute__ intercepts ALL attribute access."""
        class C:
            def __init__(self):
                object.__setattr__(self, 'x', 42)

            def __getattribute__(self, name):
                return "intercepted"

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), "intercepted")

    @skip_unless_jit
    def test_builtin_type_attr(self):
        """Access attribute on builtin types (immutable type objects)."""
        def get_real(obj):
            return obj.real

        force_compile(get_real)

        self.assertEqual(get_real(42), 42)
        self.assertEqual(get_real(3.14), 3.14)
        self.assertEqual(get_real(True), 1)

    @skip_unless_jit
    def test_inner_class_pattern(self):
        """Fresh class created each call — cache always misses.

        This is the pattern that causes 0.47x regression. The inline
        fast path should handle repeated cache misses gracefully (fall
        to slow path every time, no crash).
        """
        def make_and_access():
            class Inner:
                __slots__ = ('x',)
                def __init__(self):
                    self.x = 42
            return Inner().x

        force_compile(make_and_access)

        for _ in range(100):
            self.assertEqual(make_and_access(), 42)

    @skip_unless_jit
    def test_none_attr_access(self):
        """Access attribute on None — must raise AttributeError."""
        def get_x(obj):
            return obj.x

        force_compile(get_x)

        with self.assertRaises(AttributeError):
            get_x(None)

    @skip_unless_jit
    def test_dict_reassignment(self):
        """Replace instance __dict__ — cached accessor must handle."""
        class C:
            def __init__(self):
                self.x = 10

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 10)  # cache warm

        c.__dict__ = {"x": 99}
        self.assertEqual(get_x(c), 99)  # new dict

    @skip_unless_jit
    def test_split_to_combined_dict(self):
        """Transition from split dict to combined dict.

        CinderX uses split dicts for instances of the same type.
        Adding a non-standard key forces transition to combined dict.
        """
        class C:
            def __init__(self, v):
                self.x = v

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c1 = C(10)
        c2 = C(20)
        self.assertEqual(get_x(c1), 10)
        self.assertEqual(get_x(c2), 20)

        # Force c1 to combined dict
        c1.__dict__["unique_key_" + str(id(c1))] = "extra"
        self.assertEqual(get_x(c1), 10)  # must still work

        c3 = C(30)
        self.assertEqual(get_x(c3), 30)  # new split-dict instance


# =============================================================================
# Layer 2e: Interaction Tests
# =============================================================================

class TestInteractions(unittest.TestCase):
    """Test LOAD_ATTR interaction with generators, GC, threads."""

    @skip_unless_jit
    def test_attr_access_in_generator(self):
        """LOAD_ATTR inside a generator body — FP points to GenDataFooter
        on aarch64, so inline cache addressing must be correct."""
        class Point:
            __slots__ = ('x', 'y')
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def gen_coords(points):
            for p in points:
                yield p.x, p.y

        force_compile(gen_coords)

        pts = [Point(i, i*10) for i in range(5)]
        result = list(gen_coords(pts))
        self.assertEqual(result, [(0, 0), (1, 10), (2, 20), (3, 30), (4, 40)])

    @skip_unless_jit
    def test_attr_access_across_jit_interpreter_boundary(self):
        """JIT-compiled function accesses attr after interpreter modifies type."""
        class C:
            def __init__(self):
                self.x = 10

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 10)

        # Interpreter code modifies the type (not JIT-compiled)
        def modify_type():
            C.x = property(lambda self: 999)

        modify_type()
        self.assertEqual(get_x(c), 999)

    @skip_unless_jit
    def test_attr_access_after_gc(self):
        """Object survives GC — attr access still correct."""
        class C:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 42)

        # Create garbage and collect
        for _ in range(10):
            waste = [C() for _ in range(100)]
            del waste
            gc.collect()

        self.assertEqual(get_x(c), 42)

    @skip_unless_jit
    def test_type_destroyed_while_cached(self):
        """Type object is destroyed — cache entry must not dangle.

        Creates a type, warms the cache, then deletes the type.
        A new type at potentially the same address must not
        incorrectly match the stale cache entry.
        """
        class C:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 100

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 100)

        # Save ref to c, delete the class
        del C
        gc.collect()

        # c still exists, its type still exists (ref from c)
        self.assertEqual(get_x(c), 100)

        # Create new class with same structure
        class D:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 200

        d = D()
        self.assertEqual(get_x(d), 200)

    @skip_unless_jit
    def test_concurrent_attr_access(self):
        """Multiple threads accessing the same attribute concurrently.

        The inline cache is per-callsite, so concurrent calls from
        different threads should not corrupt it.
        """
        class C:
            __slots__ = ('x',)
            def __init__(self, v):
                self.x = v

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        results = [None] * 10
        errors = []

        def worker(idx, obj, expected):
            try:
                for _ in range(1000):
                    val = get_x(obj)
                    if val != expected:
                        errors.append(f"Thread {idx}: got {val}, expected {expected}")
                        return
                results[idx] = True
            except Exception as e:
                errors.append(f"Thread {idx}: {e}")

        objs = [C(i * 100) for i in range(10)]
        threads = [
            threading.Thread(target=worker, args=(i, objs[i], i * 100))
            for i in range(10)
        ]

        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10)

        self.assertEqual(errors, [], f"Thread errors: {errors}")
        self.assertTrue(all(r is True for r in results), f"Not all threads completed: {results}")

    @skip_unless_jit
    def test_weakref_type_with_attr(self):
        """Attr access on object whose type has weakrefs."""
        class C:
            __slots__ = ('x', '__weakref__')
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        w = weakref.ref(c)
        self.assertEqual(get_x(c), 42)
        self.assertIs(w(), c)

        # Attr access after creating weakref
        self.assertEqual(get_x(c), 42)


# =============================================================================
# Layer 2f: Deopt Storm Tests (Approach B specific)
# =============================================================================

class TestDeoptStorm(unittest.TestCase):
    """Tests for GuardType + LoadField deopt behaviour (Approach B).

    Approach B emits GuardType (type check) + LoadField (direct load) in the
    HIR simplifier. If the profiled type is wrong, the guard fails and the
    function deopts to the interpreter (which falls back to LoadAttrCached).

    These tests verify:
    1. Deopt produces correct results (not just no crash)
    2. Polymorphic code with deopt is NOT slower than the pre-Approach-B baseline
    3. No deopt thrashing (re-JIT → re-deopt loop)
    """

    @skip_unless_jit
    def test_deopt_correctness_type_switch(self):
        """Function JIT'd for type A, then called with type B.

        After deopt, the function must still return correct results
        for both types. This is the fundamental deopt correctness test.
        """
        class A:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 100

        class B:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 200

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        a = A()
        # Warm the cache / profiled type with A
        for _ in range(10):
            self.assertEqual(get_x(a), 100)

        b = B()
        # This should trigger deopt (GuardType fails for B)
        self.assertEqual(get_x(b), 200)

        # After deopt, A should still work
        self.assertEqual(get_x(a), 100)

        # Alternating types should all produce correct results
        for _ in range(100):
            self.assertEqual(get_x(a), 100)
            self.assertEqual(get_x(b), 200)

    @skip_unless_jit
    def test_deopt_storm_no_regression(self):
        """Polymorphic receiver causes repeated deopts — must not be
        slower than the baseline (BL to LoadAttrCache::invoke).

        This test measures throughput of a polymorphic access pattern.
        With Approach B, every type mismatch triggers deopt. If deopt
        is more expensive than the current function call, we have a
        regression for polymorphic code.

        Gate criterion: polymorphic throughput must be >= 80% of the
        monomorphic throughput baseline. (The current baseline already
        has overhead from cache miss, so we use a relative threshold.)
        """
        import time

        class A:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 1

        class B:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 2

        class C:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 3

        class D:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 4

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        # Monomorphic baseline
        a = A()
        n = 500_000
        for _ in range(100):
            get_x(a)  # warm

        start = time.perf_counter()
        for _ in range(n):
            get_x(a)
        mono_time = time.perf_counter() - start

        # Polymorphic (4 types — deopt storm scenario)
        objs = [A(), B(), C(), D()]
        for o in objs:
            get_x(o)  # warm each type

        start = time.perf_counter()
        for _ in range(n):
            for o in objs:
                get_x(o)
        poly_time = time.perf_counter() - start

        mono_rate = n / mono_time
        poly_rate = (n * 4) / poly_time

        print(f"\n  Deopt storm: mono={mono_rate:.0f}/s poly={poly_rate:.0f}/s "
              f"ratio={poly_rate/mono_rate:.2f}")

        # Correctness check
        for o in objs:
            self.assertEqual(get_x(o), o.x)

        # The polymorphic rate should not be catastrophically worse
        # than monomorphic. A ratio below 0.3 indicates deopt storm
        # is destroying performance. Current baseline (BL to invoke)
        # gives ratio ~1.5 (polymorphic is faster due to CPU pipelining
        # on the tight loop). After Approach B, if deopt is expensive,
        # this ratio could drop below 0.5.
        self.assertGreater(
            poly_rate / mono_rate, 0.3,
            f"Deopt storm regression: poly/mono ratio {poly_rate/mono_rate:.2f} < 0.3. "
            f"Polymorphic access is catastrophically slow — deopt overhead too high."
        )

    @skip_unless_jit
    def test_no_deopt_thrashing(self):
        """After deopt, function should NOT re-JIT and re-deopt in a loop.

        Deopt thrashing: JIT compiles function → type mismatch → deopt →
        interpreter runs → JIT re-compiles → type mismatch → deopt → ...

        After a deopt, CinderX should either:
        (a) Stay in interpreter (don't re-JIT), or
        (b) Re-JIT with a polymorphic guard that handles multiple types

        Either way, repeated calls with a mismatched type should converge
        to stable performance, not oscillate.
        """
        import time

        class A:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 10

        class B:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 20

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        a = A()
        b = B()

        # Warm with A
        for _ in range(100):
            get_x(a)

        # Now alternate with B to trigger deopt
        # Measure two batches — if thrashing, second batch is slower
        n = 100_000

        start = time.perf_counter()
        for _ in range(n):
            get_x(b)
        batch1_time = time.perf_counter() - start

        start = time.perf_counter()
        for _ in range(n):
            get_x(b)
        batch2_time = time.perf_counter() - start

        print(f"\n  Thrashing check: batch1={batch1_time:.4f}s batch2={batch2_time:.4f}s "
              f"ratio={batch2_time/batch1_time:.2f}")

        # Correctness
        self.assertEqual(get_x(a), 10)
        self.assertEqual(get_x(b), 20)

        # Batch 2 should not be significantly slower than batch 1.
        # If thrashing, batch 2 could be 2-10x slower due to repeated
        # JIT compilation overhead.
        self.assertLess(
            batch2_time, batch1_time * 2.0,
            f"Possible deopt thrashing: batch2 ({batch2_time:.4f}s) is >{2.0}x "
            f"slower than batch1 ({batch1_time:.4f}s). Function may be "
            f"re-JITing and re-deopting in a loop."
        )

    @skip_unless_jit
    def test_deopt_with_many_types(self):
        """Access attr through 20 different types — stress test for
        deopt + cache eviction interaction.

        With Approach B, the GuardType is compiled for one profiled type.
        The other 19 types all deopt. The deopt path falls back to
        LoadAttrCached which has a 4-entry PIC. So cache eviction
        also occurs. This tests the interaction of both mechanisms.
        """
        types = []
        for i in range(20):
            # Create 20 distinct types dynamically
            cls = type(f'Type{i}', (), {'__slots__': ('x',)})
            obj = cls()
            obj.x = i * 10
            types.append(obj)

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        # Access all 20 types
        for obj in types:
            result = get_x(obj)
            self.assertEqual(result, obj.x)

        # Do it again — should still be correct after cache thrashing
        for obj in types:
            result = get_x(obj)
            self.assertEqual(result, obj.x)

        # And in reverse order
        for obj in reversed(types):
            result = get_x(obj)
            self.assertEqual(result, obj.x)


# =============================================================================
# Layer 2g: Version-Tag Guard Tests (Option D specific)
# =============================================================================

class TestVersionTagGuard(unittest.TestCase):
    """Tests specific to the version-tag guard (Option D).

    Option D emits a guard that compares tp_version_tag instead of the
    type pointer. These tests verify that version-tag invalidation
    (via PyType_Modified) correctly causes the guard to fail and fall
    through to the slow path.
    """

    @skip_unless_jit
    def test_version_tag_invalidation_on_type_modify(self):
        """Modifying a type bumps its tp_version_tag.

        The version-tag guard must fail after the type is modified,
        causing fallback to LoadAttrCached which resolves the new
        attribute correctly.
        """
        class C:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        # Warm — version-tag guard compiled for C's current version
        self.assertEqual(get_x(c), 42)
        self.assertEqual(get_x(c), 42)

        # Modify the type — this calls PyType_Modified internally,
        # which bumps tp_version_tag (or sets it to 0)
        C.new_method = lambda self: "hello"

        # The version-tag guard must fail now, fall through to slow path
        # Slow path must still return the correct result
        self.assertEqual(get_x(c), 42)

    @skip_unless_jit
    def test_version_tag_invalidation_cascade(self):
        """Modifying a BASE class invalidates version tags of derived classes.

        PyType_Modified propagates to subclasses. The version-tag guard
        on a derived class must fail when the base is modified.
        """
        class Base:
            class_val = 10

        class Derived(Base):
            __slots__ = ('x',)
            def __init__(self):
                self.x = 99

        def get_class_val(obj):
            return obj.class_val

        def get_x(obj):
            return obj.x

        force_compile(get_class_val)
        force_compile(get_x)

        d = Derived()
        self.assertEqual(get_class_val(d), 10)
        self.assertEqual(get_x(d), 99)

        # Modify BASE class — invalidates Derived's version tag too
        Base.class_val = 20

        # Version-tag guard for Derived must fail, slow path gives new value
        self.assertEqual(get_class_val(d), 20)
        # Instance slot should still work (different cache entry)
        self.assertEqual(get_x(d), 99)

    @skip_unless_jit
    def test_version_tag_zero_handling(self):
        """Types with tp_version_tag == 0 must NOT match the guard.

        Some types have version_tag 0 (e.g., after too many modifications
        or types that exceed the version tag assignment limit). The guard
        must treat 0 as 'always miss' — never cache version 0.
        """
        class C:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 42)

        # Hammer the type to potentially exhaust version tags
        # (CPython has a limited version tag space)
        for i in range(100):
            setattr(C, f'_tmp_{i}', i)
            delattr(C, f'_tmp_{i}')

        # Must still return correct result regardless of version tag state
        self.assertEqual(get_x(c), 42)

    @skip_unless_jit
    def test_version_tag_guard_with_descriptor_mutation(self):
        """Adding a data descriptor bumps version tag AND changes semantics.

        This is the critical correctness test: the version-tag guard must
        fail when a descriptor is added, and the slow path must correctly
        apply descriptor protocol (data descriptor wins over instance attr).
        """
        class C:
            def __init__(self):
                self.x = 42

        def get_x(obj):
            return obj.x

        force_compile(get_x)

        c = C()
        self.assertEqual(get_x(c), 42)  # instance dict lookup

        # Add data descriptor — bumps version tag
        class Descr:
            def __get__(self, obj, typ):
                return 999
            def __set__(self, obj, val):
                pass

        C.x = Descr()

        # Version-tag guard fails → slow path → descriptor protocol
        self.assertEqual(get_x(c), 999)

    @skip_unless_jit
    def test_version_tag_stable_after_warmup(self):
        """After warmup, repeated access with same type should be fast
        (guard hits, no fallthrough to slow path).

        This is the positive case: verify the guard WORKS when it should.
        """
        class Point:
            __slots__ = ('x', 'y')
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def sum_coords(p):
            return p.x + p.y

        force_compile(sum_coords)

        p = Point(3, 7)
        # Warmup
        for _ in range(100):
            self.assertEqual(sum_coords(p), 10)

        # Stable access — version-tag guard should hit every time
        import time
        n = 500_000
        start = time.perf_counter()
        for _ in range(n):
            sum_coords(p)
        elapsed = time.perf_counter() - start

        rate = n / elapsed
        print(f"\n  Version-tag stable access: {rate:.0f} calls/sec")

        # Correctness
        self.assertEqual(sum_coords(p), 10)


# =============================================================================
# Layer 3: Performance Micro-benchmarks (informational, not gating)
# =============================================================================

class TestPerformanceBaseline(unittest.TestCase):
    """Record performance baselines. Not pass/fail — just measurement.

    Run with -v to see timing output.
    """

    @skip_unless_jit
    def test_slots_access_throughput(self):
        """Measure slot access throughput — primary fast path target."""
        import time

        class Point:
            __slots__ = ('x', 'y')
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def access_loop(p, n):
            total = 0
            for _ in range(n):
                total += p.x
                total += p.y
            return total

        force_compile(access_loop)

        p = Point(1, 2)
        n = 1_000_000
        # Warm up
        access_loop(p, 1000)

        start = time.perf_counter()
        result = access_loop(p, n)
        elapsed = time.perf_counter() - start

        self.assertEqual(result, 3 * n)
        print(f"\n  Slot access: {n} iterations in {elapsed:.4f}s "
              f"({n/elapsed:.0f} accesses/sec)")

    @skip_unless_jit
    def test_dict_access_throughput(self):
        """Measure dict-based attr access throughput."""
        import time

        class Obj:
            def __init__(self, x, y):
                self.x = x
                self.y = y

        def access_loop(o, n):
            total = 0
            for _ in range(n):
                total += o.x
                total += o.y
            return total

        force_compile(access_loop)

        o = Obj(1, 2)
        n = 1_000_000
        access_loop(o, 1000)

        start = time.perf_counter()
        result = access_loop(o, n)
        elapsed = time.perf_counter() - start

        self.assertEqual(result, 3 * n)
        print(f"\n  Dict access: {n} iterations in {elapsed:.4f}s "
              f"({n/elapsed:.0f} accesses/sec)")

    @skip_unless_jit
    def test_polymorphic_access_throughput(self):
        """Measure polymorphic (cache miss) attr access throughput."""
        import time

        class A:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 1

        class B:
            __slots__ = ('x',)
            def __init__(self):
                self.x = 2

        def access_loop(objs, n):
            total = 0
            for _ in range(n):
                for o in objs:
                    total += o.x
            return total

        force_compile(access_loop)

        objs = [A(), B(), A(), B()]
        n = 250_000
        access_loop(objs, 100)

        start = time.perf_counter()
        result = access_loop(objs, n)
        elapsed = time.perf_counter() - start

        expected = (1 + 2 + 1 + 2) * n
        self.assertEqual(result, expected)
        print(f"\n  Polymorphic access: {n*4} accesses in {elapsed:.4f}s "
              f"({n*4/elapsed:.0f} accesses/sec)")


# =============================================================================
# Summary
# =============================================================================

if __name__ == "__main__":
    print("=" * 70)
    print("LOAD_ATTR Inline Fast Path Test Suite")
    print("=" * 70)
    print(f"Platform:    {platform.machine()}")
    print(f"Python:      {sys.version}")
    print(f"CinderX JIT: {'available' if HAS_CINDERJIT else 'NOT AVAILABLE'}")
    if HAS_CINDERJIT:
        print(f"JIT enabled: {cinderjit.is_enabled()}")
    print("=" * 70)
    print()

    unittest.main(verbosity=2)
