#!/usr/bin/env python3
"""
test_call_bound_method_general.py — Correctness and deopt tests for
CALL_BOUND_METHOD_GENERAL specialisation.

Targets: CALL_BOUND_METHOD_GENERAL (formerly CALL_NO_KW_METHOD_DESCRIPTOR_O
in some naming schemes).

CALL_BOUND_METHOD_GENERAL is the fallback specialisation for bound method
calls that do not match CALL_BOUND_METHOD_EXACT_ARGS. It handles cases
where the call involves:
  - Default arguments being used (caller provides fewer args than params)
  - *args or **kwargs in the method signature
  - Keyword arguments at the call site
  - Any combination that prevents the exact-args fast path

The adaptive interpreter still specialises these calls because the
receiver type is stable, but the call convention is more general
(hence the name). CinderX JIT emits GuardType on the receiver and
dispatches through the general Python call machinery.

Deopt triggers:
  - Receiver type changes (different class at same call site)
  - Method is overridden on instance or subclass
  - Method replaced with non-Python callable

Tests cover:
  - Method with default args (some provided, some defaulted)
  - Method with all defaults used
  - Method with *args
  - Method with **kwargs
  - Method with *args and **kwargs
  - Method called with keyword arguments
  - Method with positional-only params and defaults
  - Method with keyword-only params
  - Deopt: different class with same method name
  - Deopt: instance method replaced at runtime
  - Deopt: subclass overrides method signature
  - Method with complex default expressions
  - Rapid calls with varying arg counts (1000)
  - Stability — 10000 calls
  - Method with mixed positional and keyword args
  - Method with None sentinel default pattern
  - Method returning *args and **kwargs for inspection
  - Chained calls with defaults
  - Multiple instances, same method with defaults
  - Equivalence: obj.method(args) vs Type.method(obj, args)

FALSIFICATION DESIGN:
  Each test verifies:
  1. Correct result when JIT-compiled (warmup -> JIT -> call -> check)
  2. Result matches interpreter semantics exactly
  3. Deopt cases produce correct results after type change

  A test PASSES only if all assertions hold.
  A test FAILS if any assertion fires or an unexpected exception occurs.

Usage:
  python3 test_call_bound_method_general.py
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
    # Test 1: Method with default args (some provided, some defaulted)
    # ------------------------------------------------------------------
    try:
        class Formatter:
            def __init__(self, prefix):
                self.prefix = prefix
            def format(self, text, sep=": ", suffix=""):
                return f"{self.prefix}{sep}{text}{suffix}"

        def call_format(obj, text):
            return obj.format(text)

        f = Formatter("INFO")
        for _ in range(WARMUP):
            call_format(f, "msg")
        check_jit_compiled(call_format, "call_format")

        assert call_format(Formatter("INFO"), "hello") == "INFO: hello"
        assert call_format(Formatter("ERR"), "fail") == "ERR: fail"
        assert call_format(Formatter(""), "bare") == ": bare"

        # Also test with explicit override of defaults
        def call_format_full(obj, text, sep, suffix):
            return obj.format(text, sep, suffix)

        for _ in range(WARMUP):
            call_format_full(f, "msg", " - ", "!")
        assert call_format_full(Formatter("X"), "y", " - ", "!") == "X - y!"
        print("  PASS: test_default_args_partial")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_default_args_partial — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 2: Method with all defaults used
    # ------------------------------------------------------------------
    try:
        class Config:
            def __init__(self):
                self.calls = 0
            def configure(self, debug=False, verbose=False, level=0):
                self.calls += 1
                return (debug, verbose, level)

        def call_configure(obj):
            return obj.configure()

        c = Config()
        for _ in range(WARMUP):
            call_configure(c)
        check_jit_compiled(call_configure, "call_configure")

        result = call_configure(Config())
        assert result == (False, False, 0)

        # Override all defaults
        def call_configure_all(obj, d, v, l):
            return obj.configure(d, v, l)

        for _ in range(WARMUP):
            call_configure_all(c, True, True, 5)
        assert call_configure_all(Config(), True, True, 5) == (True, True, 5)
        print("  PASS: test_all_defaults_used")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_all_defaults_used — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 3: Method with *args
    # ------------------------------------------------------------------
    try:
        class Collector:
            def __init__(self):
                self.items = []
            def add_many(self, *args):
                self.items.extend(args)
                return len(self.items)

        def call_add_many(obj, *args):
            return obj.add_many(*args)

        col = Collector()
        for _ in range(WARMUP):
            col.items.clear()
            call_add_many(col, 1, 2, 3)
        check_jit_compiled(call_add_many, "call_add_many")

        c = Collector()
        assert call_add_many(c, 1, 2, 3) == 3
        assert call_add_many(c, 4) == 4
        assert call_add_many(c) == 4
        assert c.items == [1, 2, 3, 4]
        print("  PASS: test_varargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_varargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 4: Method with **kwargs
    # ------------------------------------------------------------------
    try:
        class Options:
            def __init__(self):
                self.opts = {}
            def set_options(self, **kwargs):
                self.opts.update(kwargs)
                return dict(self.opts)

        def call_set_options(obj, **kwargs):
            return obj.set_options(**kwargs)

        o = Options()
        for _ in range(WARMUP):
            o.opts.clear()
            call_set_options(o, x=1)
        check_jit_compiled(call_set_options, "call_set_options")

        o2 = Options()
        r1 = call_set_options(o2, debug=True, level=3)
        assert r1 == {"debug": True, "level": 3}
        r2 = call_set_options(o2, verbose=True)
        assert r2 == {"debug": True, "level": 3, "verbose": True}
        print("  PASS: test_kwargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_kwargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 5: Method with *args and **kwargs
    # ------------------------------------------------------------------
    try:
        class Logger:
            def __init__(self, name):
                self.name = name
            def log(self, *args, **kwargs):
                parts = [self.name] + [str(a) for a in args]
                for k, v in sorted(kwargs.items()):
                    parts.append(f"{k}={v}")
                return " ".join(parts)

        def call_log(obj, *args, **kwargs):
            return obj.log(*args, **kwargs)

        lg = Logger("APP")
        for _ in range(WARMUP):
            call_log(lg, "test")
        check_jit_compiled(call_log, "call_log")

        assert call_log(Logger("APP"), "start") == "APP start"
        assert call_log(Logger("DB"), "query", "SELECT") == "DB query SELECT"
        assert call_log(Logger("X"), level="info") == "X level=info"
        assert call_log(Logger("Y"), "msg", code=42) == "Y msg code=42"
        print("  PASS: test_args_and_kwargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_args_and_kwargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 6: Method called with keyword arguments
    # ------------------------------------------------------------------
    try:
        class Rect:
            def __init__(self, w, h):
                self.w = w
                self.h = h
            def resize(self, width=None, height=None):
                if width is not None:
                    self.w = width
                if height is not None:
                    self.h = height
                return (self.w, self.h)

        def call_resize_kw(obj, **kwargs):
            return obj.resize(**kwargs)

        r = Rect(10, 20)
        for _ in range(WARMUP):
            call_resize_kw(r, width=10, height=20)
        check_jit_compiled(call_resize_kw, "call_resize_kw")

        r2 = Rect(10, 20)
        assert call_resize_kw(r2, width=50) == (50, 20)
        assert call_resize_kw(r2, height=100) == (50, 100)
        assert call_resize_kw(r2, width=1, height=1) == (1, 1)
        assert call_resize_kw(r2) == (1, 1)
        print("  PASS: test_keyword_args_at_callsite")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_keyword_args_at_callsite — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 7: Method with positional-only params and defaults
    # ------------------------------------------------------------------
    try:
        class Maths:
            def power(self, base, exp=2, /):
                return base ** exp

        def call_power_default(obj, base):
            return obj.power(base)

        m = Maths()
        for _ in range(WARMUP):
            call_power_default(m, 3)
        check_jit_compiled(call_power_default, "call_power_default")

        assert call_power_default(Maths(), 3) == 9
        assert call_power_default(Maths(), 5) == 25
        assert call_power_default(Maths(), 0) == 0
        assert call_power_default(Maths(), -2) == 4

        def call_power_explicit(obj, base, exp):
            return obj.power(base, exp)

        for _ in range(WARMUP):
            call_power_explicit(m, 2, 3)
        assert call_power_explicit(Maths(), 2, 10) == 1024
        print("  PASS: test_positional_only_with_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_positional_only_with_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 8: Method with keyword-only params
    # ------------------------------------------------------------------
    try:
        class Query:
            def __init__(self, table):
                self.table = table
            def select(self, *columns, where=None, limit=None):
                parts = [f"SELECT {','.join(columns) or '*'} FROM {self.table}"]
                if where:
                    parts.append(f"WHERE {where}")
                if limit is not None:
                    parts.append(f"LIMIT {limit}")
                return " ".join(parts)

        def call_select(obj, *cols, **kw):
            return obj.select(*cols, **kw)

        q = Query("users")
        for _ in range(WARMUP):
            call_select(q, "name")
        check_jit_compiled(call_select, "call_select")

        assert call_select(Query("users"), "name") == "SELECT name FROM users"
        assert call_select(Query("t"), "a", "b", where="x=1") == "SELECT a,b FROM t WHERE x=1"
        assert call_select(Query("t"), limit=10) == "SELECT * FROM t LIMIT 10"
        assert call_select(Query("t"), "id", where="active", limit=5) == "SELECT id FROM t WHERE active LIMIT 5"
        print("  PASS: test_keyword_only_params")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_keyword_only_params — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 9: Deopt — different class with same method name
    # ------------------------------------------------------------------
    try:
        class EngineA:
            def run(self, speed=10):
                return f"A:{speed}"

        class EngineB:
            def run(self, speed=20):
                return f"B:{speed}"

        class EngineC:
            def run(self, speed=30, boost=False):
                return f"C:{speed}:{'boost' if boost else 'normal'}"

        def call_run(obj):
            return obj.run()

        ea = EngineA()
        for _ in range(WARMUP):
            call_run(ea)
        check_jit_compiled(call_run, "call_run")

        assert call_run(EngineA()) == "A:10"
        # Deopt: EngineB (same method name, different default)
        assert call_run(EngineB()) == "B:20"
        # Deopt: EngineC (same method name, extra param)
        assert call_run(EngineC()) == "C:30:normal"
        # Back to EngineA
        assert call_run(EngineA()) == "A:10"
        print("  PASS: test_deopt_different_class")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_different_class — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 10: Deopt — instance method replaced at runtime
    # ------------------------------------------------------------------
    try:
        class Processor:
            def process(self, data, mode="fast"):
                return f"{mode}:{data}"

        def call_process(obj, data):
            return obj.process(data)

        p = Processor()
        for _ in range(WARMUP):
            call_process(p, "x")
        check_jit_compiled(call_process, "call_process")

        assert call_process(p, "test") == "fast:test"

        # Replace method on instance
        p.process = lambda data, mode="slow": f"{mode}:{data}"
        assert call_process(p, "test") == "slow:test"

        # New instance has original method
        p2 = Processor()
        assert call_process(p2, "data") == "fast:data"
        print("  PASS: test_deopt_instance_method_replaced")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_instance_method_replaced — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 11: Deopt — subclass overrides method signature
    # ------------------------------------------------------------------
    try:
        class BaseHandler:
            def handle(self, event, priority=0):
                return f"base:{event}:{priority}"

        class ChildHandler(BaseHandler):
            def handle(self, event, priority=0, verbose=False):
                tag = "verbose" if verbose else "quiet"
                return f"child:{event}:{priority}:{tag}"

        def call_handle(obj, event):
            return obj.handle(event)

        bh = BaseHandler()
        for _ in range(WARMUP):
            call_handle(bh, "click")
        check_jit_compiled(call_handle, "call_handle")

        assert call_handle(BaseHandler(), "click") == "base:click:0"
        assert call_handle(ChildHandler(), "click") == "child:click:0:quiet"
        assert call_handle(BaseHandler(), "submit") == "base:submit:0"
        print("  PASS: test_deopt_subclass_override_signature")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_deopt_subclass_override_signature — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 12: Method with complex default expressions
    # ------------------------------------------------------------------
    try:
        _SENTINEL = object()

        class Builder:
            def build(self, items=None, count=1, transform=str):
                if items is None:
                    items = []
                return [transform(i) for i in range(count)] + items

        def call_build_default(obj):
            return obj.build()

        b = Builder()
        for _ in range(WARMUP):
            call_build_default(b)
        check_jit_compiled(call_build_default, "call_build_default")

        assert call_build_default(Builder()) == ["0"]

        def call_build_custom(obj, items, count, transform):
            return obj.build(items, count, transform)

        for _ in range(WARMUP):
            call_build_custom(b, [], 2, str)
        assert call_build_custom(Builder(), ["x"], 3, str) == ["0", "1", "2", "x"]
        assert call_build_custom(Builder(), [], 2, lambda x: x * 10) == [0, 10]
        print("  PASS: test_complex_default_expressions")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_complex_default_expressions — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 13: Rapid calls with varying arg counts (1000)
    # ------------------------------------------------------------------
    try:
        class Adder:
            def __init__(self):
                self.total = 0
            def add(self, *values):
                s = sum(values)
                self.total += s
                return self.total

        def call_add(obj, *values):
            return obj.add(*values)

        a = Adder()
        for _ in range(WARMUP):
            a.total = 0
            call_add(a, 1)
        check_jit_compiled(call_add, "call_add")

        a2 = Adder()
        expected = 0
        for i in range(1000):
            result = call_add(a2, i)
            expected += i
            assert result == expected
        assert a2.total == sum(range(1000))
        print("  PASS: test_rapid_1000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_rapid_1000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 14: Stability — 10000 calls
    # ------------------------------------------------------------------
    try:
        class Tracker:
            def __init__(self):
                self.n = 0
            def tick(self, amount=1):
                self.n += amount

        def call_tick(obj):
            obj.tick()

        t = Tracker()
        for _ in range(WARMUP):
            call_tick(t)
        check_jit_compiled(call_tick, "call_tick")

        t2 = Tracker()
        for _ in range(10000):
            call_tick(t2)
        assert t2.n == 10000
        print("  PASS: test_stability_10000")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_stability_10000 — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 15: Method with mixed positional and keyword args
    # ------------------------------------------------------------------
    try:
        class Report:
            def generate(self, title, *, format="text", pages=1):
                return f"{title}:{format}:{pages}"

        def call_gen_default(obj, title):
            return obj.generate(title)

        def call_gen_kw(obj, title, **kw):
            return obj.generate(title, **kw)

        r = Report()
        for _ in range(WARMUP):
            call_gen_default(r, "test")
        check_jit_compiled(call_gen_default, "call_gen_default")

        assert call_gen_default(Report(), "Q1") == "Q1:text:1"

        for _ in range(WARMUP):
            call_gen_kw(r, "test", format="pdf")
        assert call_gen_kw(Report(), "Q1", format="pdf") == "Q1:pdf:1"
        assert call_gen_kw(Report(), "Q2", format="html", pages=5) == "Q2:html:5"
        print("  PASS: test_mixed_positional_keyword")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_mixed_positional_keyword — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 16: Method with None sentinel default pattern
    # ------------------------------------------------------------------
    try:
        class Cache:
            def __init__(self):
                self.store = {}
            def get(self, key, default=None):
                return self.store.get(key, default)

        def call_get(obj, key):
            return obj.get(key)

        def call_get_with_default(obj, key, default):
            return obj.get(key, default)

        c = Cache()
        c.store = {"a": 1, "b": 2}
        for _ in range(WARMUP):
            call_get(c, "a")
        check_jit_compiled(call_get, "call_get")

        assert call_get(c, "a") == 1
        assert call_get(c, "missing") is None

        for _ in range(WARMUP):
            call_get_with_default(c, "a", -1)
        assert call_get_with_default(c, "missing", -1) == -1
        assert call_get_with_default(c, "a", -1) == 1
        print("  PASS: test_none_sentinel_default")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_none_sentinel_default — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 17: Method returning *args and **kwargs for inspection
    # ------------------------------------------------------------------
    try:
        class Echo:
            def echo(self, *args, **kwargs):
                return (args, kwargs)

        def call_echo(obj, *args, **kwargs):
            return obj.echo(*args, **kwargs)

        e = Echo()
        for _ in range(WARMUP):
            call_echo(e, 1, 2, x=3)
        check_jit_compiled(call_echo, "call_echo")

        assert call_echo(Echo()) == ((), {})
        assert call_echo(Echo(), 1, 2, 3) == ((1, 2, 3), {})
        assert call_echo(Echo(), a=1, b=2) == ((), {"a": 1, "b": 2})
        assert call_echo(Echo(), 10, key="val") == ((10,), {"key": "val"})
        print("  PASS: test_echo_args_kwargs")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_echo_args_kwargs — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 18: Chained calls with defaults
    # ------------------------------------------------------------------
    try:
        class Chain:
            def __init__(self, val=0):
                self.val = val
            def add(self, n=1):
                self.val += n
                return self
            def mul(self, n=2):
                self.val *= n
                return self

        def run_chain_defaults(obj):
            return obj.add().mul()

        c = Chain(10)
        for _ in range(WARMUP):
            c.val = 10
            run_chain_defaults(c)
        check_jit_compiled(run_chain_defaults, "run_chain_defaults")

        c1 = Chain(10)
        result = run_chain_defaults(c1)
        assert result is c1
        assert c1.val == 22  # (10 + 1) * 2

        c2 = Chain(0)
        run_chain_defaults(c2)
        assert c2.val == 2  # (0 + 1) * 2

        c3 = Chain(5)
        run_chain_defaults(c3)
        assert c3.val == 12  # (5 + 1) * 2
        print("  PASS: test_chained_calls_with_defaults")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_chained_calls_with_defaults — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Test 19: Multiple instances, same method with defaults
    # ------------------------------------------------------------------
    try:
        class Scaler:
            def __init__(self, factor):
                self.factor = factor
            def scale(self, x, offset=0):
                return x * self.factor + offset

        def call_scale(obj, x):
            return obj.scale(x)

        s = Scaler(2)
        for _ in range(WARMUP):
            call_scale(s, 5)
        check_jit_compiled(call_scale, "call_scale")

        instances = [Scaler(i) for i in range(10)]
        for i, inst in enumerate(instances):
            result = call_scale(inst, 7)
            assert result == i * 7, f"Scaler({i}).scale(7) = {result}, expected {i * 7}"

        # With explicit offset
        def call_scale_offset(obj, x, offset):
            return obj.scale(x, offset)

        for _ in range(WARMUP):
            call_scale_offset(s, 5, 10)
        assert call_scale_offset(Scaler(3), 4, 100) == 112  # 4*3 + 100
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
            def combine(self, sep=","):
                return f"{self.a}{sep}{self.b}"

        def via_instance(obj):
            return obj.combine()

        def via_type(obj):
            return Pair.combine(obj)

        def via_instance_sep(obj, sep):
            return obj.combine(sep)

        def via_type_sep(obj, sep):
            return Pair.combine(obj, sep)

        p = Pair("x", "y")
        for _ in range(WARMUP):
            via_instance(p)
            via_type(p)
            via_instance_sep(p, "-")
            via_type_sep(p, "-")
        check_jit_compiled(via_instance, "via_instance")
        check_jit_compiled(via_type, "via_type")

        test_pairs = [Pair("a", "b"), Pair("1", "2"), Pair("", ""), Pair("hello", "world")]
        for pair in test_pairs:
            r1 = via_instance(pair)
            r2 = via_type(pair)
            assert r1 == r2, f"Default sep mismatch: {r1} vs {r2}"

            for sep in ["-", " ", ":", "||"]:
                s1 = via_instance_sep(pair, sep)
                s2 = via_type_sep(pair, sep)
                assert s1 == s2, f"Sep '{sep}' mismatch: {s1} vs {s2}"
        print("  PASS: test_equivalence_instance_vs_type_call")
        passed += 1
    except Exception as e:
        print(f"  FAIL: test_equivalence_instance_vs_type_call — {e}")
        failed += 1

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    total = passed + failed
    print(f"\nCALL_BOUND_METHOD_GENERAL: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
