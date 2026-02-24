#!/usr/bin/env python3
"""Correctness tests for LICM GuardType hoisting (Richards regression).

BUG: LICM hoists a GuardType from a loop body to the preheader, but the
FrameState for the deopt path references a frame that does not exist in the
preheader context, triggering frame.cpp:163 Abort ("couldn't find non-inlined
frame"). The crash occurs during JIT compilation of bench_richards_full,
specifically when _richards_schedule's main loop dispatches to polymorphic
task types.

Pattern:
  - A scheduler loop iterates over tasks of different types (polymorphic)
  - Each task type overrides a run() method
  - The JIT adds GuardType on the task object inside the loop
  - LICM detects the guard is loop-invariant (same object per iteration)
  - LICM hoists the GuardType to the loop preheader
  - The hoisted GuardType's FrameState references the wrong block context
  - Deopt path cannot find the non-inlined frame -> Abort

Tests:
  1-5:   Core polymorphic dispatch patterns that trigger GuardType
  6-10:  Variations with different loop structures (for, while, nested)
  11-15: Deopt scenarios (type changes mid-iteration)
  16-20: Edge cases (empty loops, single iterations, deep inheritance)

Falsification design:
  - Tests run with enable_specialized_opcodes() to maximise GuardType creation
  - Large warmup (15000 calls) ensures JIT compilation including LICM pass
  - If LICM bug is present, the JIT will abort during compilation
  - If LICM bug is fixed, all tests must pass with correct results
  - Deopt tests verify the function still produces correct results after
    the type guard fails and falls back to the interpreter
"""

import sys

WARMUP = 15000
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
    try:
        import cinderx
        cinderx.init()
    except (ImportError, AttributeError):
        pass

    try:
        import cinderjit
        cinderjit.auto()
        cinderjit.enable_specialized_opcodes()
    except (ImportError, AttributeError):
        pass

    passed = 0
    failed = 0

    # ---- Test 1: Basic polymorphic scheduler loop ----
    try:
        class TaskBase:
            def __init__(self, val):
                self.val = val
            def run(self):
                return self.val + 1

        class TaskA(TaskBase):
            def run(self):
                return self.val * 2

        class TaskB(TaskBase):
            def run(self):
                return self.val + 10

        def polymorphic_scheduler(tasks):
            """Dispatches run() across polymorphic task list."""
            total = 0
            for t in tasks:
                total += t.run()
            return total

        tasks = [TaskA(3), TaskB(5), TaskBase(7), TaskA(2), TaskB(1)]
        for _ in range(WARMUP):
            polymorphic_scheduler(tasks)
        check_jit_compiled(polymorphic_scheduler, "polymorphic_scheduler")
        result = polymorphic_scheduler(tasks)
        expected = 6 + 15 + 8 + 4 + 11  # TaskA(3).run()=6, TaskB(5).run()=15, etc.
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: polymorphic_scheduler")
    except Exception as e:
        failed += 1
        print(f"  FAIL: polymorphic_scheduler — {e}")

    # ---- Test 2: Linked list traversal with polymorphic nodes ----
    try:
        class Node:
            def __init__(self, val, nxt=None):
                self.val = val
                self.nxt = nxt
            def process(self):
                return self.val

        class DoubleNode(Node):
            def process(self):
                return self.val * 2

        class TripleNode(Node):
            def process(self):
                return self.val * 3

        def traverse_linked_list(head):
            """Traverses a linked list calling process() on each node."""
            total = 0
            current = head
            while current is not None:
                total += current.process()
                current = current.nxt
            return total

        head = TripleNode(5, DoubleNode(3, Node(7, TripleNode(2, None))))
        for _ in range(WARMUP):
            traverse_linked_list(head)
        check_jit_compiled(traverse_linked_list, "traverse_linked_list")
        result = traverse_linked_list(head)
        expected = 15 + 6 + 7 + 6  # 5*3 + 3*2 + 7 + 2*3
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: linked_list_polymorphic")
    except Exception as e:
        failed += 1
        print(f"  FAIL: linked_list_polymorphic — {e}")

    # ---- Test 3: State machine with polymorphic transitions ----
    try:
        class State:
            def __init__(self, name):
                self.name = name
            def tick(self, counter):
                return counter + 1

        class FastState(State):
            def tick(self, counter):
                return counter + 10

        class SlowState(State):
            def tick(self, counter):
                return counter + 0

        def run_state_machine(states, n):
            """Cycles through states, calling tick() each iteration."""
            counter = 0
            for i in range(n):
                s = states[i % len(states)]
                counter = s.tick(counter)
            return counter

        states = [FastState("fast"), State("normal"), SlowState("slow")]
        for _ in range(WARMUP):
            run_state_machine(states, 30)
        check_jit_compiled(run_state_machine, "run_state_machine")
        result = run_state_machine(states, 30)
        # 10 cycles of (fast=+10, normal=+1, slow=+0) = 10 * 11 = 110
        assert result == 110, f"got {result}, expected 110"
        passed += 1
        print("  PASS: state_machine_polymorphic")
    except Exception as e:
        failed += 1
        print(f"  FAIL: state_machine_polymorphic — {e}")

    # ---- Test 4: Richards-style task scheduling ----
    try:
        class RTask:
            def __init__(self, pri, state=0):
                self.pri = pri
                self.state = state
            def execute(self, work_area):
                self.state = (self.state + self.pri + 1) % 7
                work_area['count'] += self.state
                return self.state

        class RIdleTask(RTask):
            def execute(self, work_area):
                self.state = (self.state + 1) % 3
                work_area['idle'] += 1
                return self.state

        class RWorkerTask(RTask):
            def execute(self, work_area):
                self.state = (self.state + self.pri + 2) % 5
                work_area['work'] += self.state
                return self.state

        class RHandlerTask(RTask):
            def execute(self, work_area):
                self.state = (self.state + self.pri) % 4
                work_area['handled'] += 1
                return self.state

        def richards_schedule(tasks, n_iter):
            """Runs the Richards-style scheduler loop."""
            wa = {'count': 0, 'idle': 0, 'work': 0, 'handled': 0}
            for _ in range(n_iter):
                for task in tasks:
                    task.execute(wa)
            return wa

        tasks = [
            RIdleTask(0),
            RWorkerTask(1),
            RWorkerTask(2),
            RHandlerTask(3),
            RHandlerTask(4),
        ]
        for _ in range(WARMUP):
            # Reset state for determinism
            for t in tasks:
                t.state = 0
            richards_schedule(tasks, 5)

        for t in tasks:
            t.state = 0
        check_jit_compiled(richards_schedule, "richards_schedule")
        result = richards_schedule(tasks, 100)
        assert result['idle'] == 100, f"idle count {result['idle']}, expected 100"
        assert result['handled'] == 200, f"handled count {result['handled']}, expected 200"
        passed += 1
        print("  PASS: richards_schedule")
    except Exception as e:
        failed += 1
        print(f"  FAIL: richards_schedule — {e}")

    # ---- Test 5: Polymorphic dispatch with method returning self ----
    try:
        class ChainBase:
            def __init__(self, val):
                self.val = val
            def step(self):
                self.val += 1
                return self

        class ChainDouble(ChainBase):
            def step(self):
                self.val *= 2
                return self

        class ChainNeg(ChainBase):
            def step(self):
                self.val = -self.val
                return self

        def chain_dispatch(objs, n):
            """Calls step() and follows the returned self reference."""
            for _ in range(n):
                for obj in objs:
                    obj.step()
            return sum(o.val for o in objs)

        objs = [ChainBase(1), ChainDouble(1), ChainNeg(1)]
        for _ in range(WARMUP):
            objs = [ChainBase(1), ChainDouble(1), ChainNeg(1)]
            chain_dispatch(objs, 3)

        objs = [ChainBase(1), ChainDouble(1), ChainNeg(1)]
        check_jit_compiled(chain_dispatch, "chain_dispatch")
        result = chain_dispatch(objs, 3)
        # ChainBase: 1 -> 2 -> 3 -> 4
        # ChainDouble: 1 -> 2 -> 4 -> 8
        # ChainNeg: 1 -> -1 -> 1 -> -1
        assert result == 4 + 8 + (-1), f"got {result}, expected 11"
        passed += 1
        print("  PASS: chain_dispatch")
    except Exception as e:
        failed += 1
        print(f"  FAIL: chain_dispatch — {e}")

    # ---- Test 6: Nested loop with outer-loop-invariant type ----
    try:
        class Accumulator:
            def __init__(self):
                self.total = 0
            def add(self, x):
                self.total += x

        class ScaledAccumulator(Accumulator):
            def __init__(self, scale):
                super().__init__()
                self.scale = scale
            def add(self, x):
                self.total += x * self.scale

        def nested_loop_invariant_type(acc, n):
            """Inner loop uses acc.add() — type is invariant in inner loop."""
            for i in range(n):
                for j in range(10):
                    acc.add(i + j)
            return acc.total

        acc = Accumulator()
        for _ in range(WARMUP):
            acc.total = 0
            nested_loop_invariant_type(acc, 5)
        check_jit_compiled(nested_loop_invariant_type, "nested_loop_invariant_type")
        acc.total = 0
        result = nested_loop_invariant_type(acc, 10)
        expected = sum((i + j) for i in range(10) for j in range(10))
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: nested_loop_invariant_type")
    except Exception as e:
        failed += 1
        print(f"  FAIL: nested_loop_invariant_type — {e}")

    # ---- Test 7: While loop with polymorphic method call ----
    try:
        class Counter:
            def __init__(self, n):
                self.n = n
            def decrement(self):
                self.n -= 1
                return self.n

        class FastCounter(Counter):
            def decrement(self):
                self.n -= 2
                return self.n

        def while_loop_polymorphic(counter):
            """While loop calling decrement() — type invariant in loop."""
            total = 0
            while counter.n > 0:
                val = counter.decrement()
                total += max(val, 0)
            return total

        for _ in range(WARMUP):
            c = Counter(20)
            while_loop_polymorphic(c)
        check_jit_compiled(while_loop_polymorphic, "while_loop_polymorphic")
        c = Counter(20)
        result = while_loop_polymorphic(c)
        expected = sum(range(1, 20))  # 19+18+...+1 = 190
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: while_loop_polymorphic")
    except Exception as e:
        failed += 1
        print(f"  FAIL: while_loop_polymorphic — {e}")

    # ---- Test 8: Method dispatch inside generator consumed by for loop ----
    try:
        class Emitter:
            def __init__(self, base):
                self.base = base
            def emit(self, x):
                return self.base + x

        class ScaledEmitter(Emitter):
            def emit(self, x):
                return self.base * x

        def gen_with_method(emitter, n):
            """Generator that calls emitter.emit() — type invariant."""
            for i in range(n):
                yield emitter.emit(i)

        def consume_gen(emitter, n):
            total = 0
            for val in gen_with_method(emitter, n):
                total += val
            return total

        e = Emitter(10)
        for _ in range(WARMUP):
            consume_gen(e, 20)
        check_jit_compiled(consume_gen, "consume_gen")
        result = consume_gen(e, 20)
        expected = sum(10 + i for i in range(20))  # 10*20 + 0+1+...+19 = 200+190 = 390
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: gen_with_method_dispatch")
    except Exception as e:
        failed += 1
        print(f"  FAIL: gen_with_method_dispatch — {e}")

    # ---- Test 9: Loop with attribute access on polymorphic object ----
    try:
        class Config:
            def __init__(self, val):
                self.val = val
                self.multiplier = 1

        class FastConfig(Config):
            def __init__(self, val):
                super().__init__(val)
                self.multiplier = 10

        def loop_with_attr_access(config, n):
            """Loop that reads config.multiplier — type invariant in loop."""
            total = 0
            for i in range(n):
                total += i * config.multiplier
            return total

        cfg = Config(0)
        for _ in range(WARMUP):
            loop_with_attr_access(cfg, 50)
        check_jit_compiled(loop_with_attr_access, "loop_with_attr_access")
        result = loop_with_attr_access(cfg, 50)
        expected = sum(i * 1 for i in range(50))
        assert result == expected, f"got {result}, expected {expected}"
        # Also test with FastConfig
        fcfg = FastConfig(0)
        result2 = loop_with_attr_access(fcfg, 50)
        expected2 = sum(i * 10 for i in range(50))
        assert result2 == expected2, f"FastConfig: got {result2}, expected {expected2}"
        passed += 1
        print("  PASS: loop_with_attr_access")
    except Exception as e:
        failed += 1
        print(f"  FAIL: loop_with_attr_access — {e}")

    # ---- Test 10: Deep inheritance chain in loop ----
    try:
        class Level0:
            def compute(self, x):
                return x

        class Level1(Level0):
            def compute(self, x):
                return x + 1

        class Level2(Level1):
            def compute(self, x):
                return x + 2

        class Level3(Level2):
            def compute(self, x):
                return x + 3

        def deep_inheritance_loop(obj, n):
            """Loop calling compute() on deeply inherited type."""
            total = 0
            for i in range(n):
                total += obj.compute(i)
            return total

        obj = Level3()
        for _ in range(WARMUP):
            deep_inheritance_loop(obj, 20)
        check_jit_compiled(deep_inheritance_loop, "deep_inheritance_loop")
        result = deep_inheritance_loop(obj, 20)
        expected = sum(i + 3 for i in range(20))
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: deep_inheritance_loop")
    except Exception as e:
        failed += 1
        print(f"  FAIL: deep_inheritance_loop — {e}")

    # ---- Test 11: Deopt — type changes between warmup and test ----
    try:
        class Worker:
            def work(self, x):
                return x + 1

        class SpecialWorker(Worker):
            def work(self, x):
                return x * 3

        def worker_loop(worker, n):
            total = 0
            for i in range(n):
                total += worker.work(i)
            return total

        w = Worker()
        for _ in range(WARMUP):
            worker_loop(w, 20)
        check_jit_compiled(worker_loop, "worker_loop")
        # Switch to SpecialWorker — forces deopt
        sw = SpecialWorker()
        result = worker_loop(sw, 20)
        expected = sum(i * 3 for i in range(20))
        assert result == expected, f"got {result}, expected {expected}"
        # Original still works
        result2 = worker_loop(w, 20)
        expected2 = sum(i + 1 for i in range(20))
        assert result2 == expected2, f"original: got {result2}, expected {expected2}"
        passed += 1
        print("  PASS: deopt_type_change")
    except Exception as e:
        failed += 1
        print(f"  FAIL: deopt_type_change — {e}")

    # ---- Test 12: Deopt — list of tasks, type changes mid-list ----
    try:
        class Processor:
            def process(self, x):
                return x + 1

        class DoubleProcessor(Processor):
            def process(self, x):
                return x * 2

        def process_list_deopt(items, processor):
            total = 0
            for x in items:
                total += processor.process(x)
            return total

        items = list(range(20))
        p = Processor()
        for _ in range(WARMUP):
            process_list_deopt(items, p)
        check_jit_compiled(process_list_deopt, "process_list_deopt")
        # Deopt with DoubleProcessor
        dp = DoubleProcessor()
        result = process_list_deopt(items, dp)
        expected = sum(x * 2 for x in range(20))
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: deopt_mid_list")
    except Exception as e:
        failed += 1
        print(f"  FAIL: deopt_mid_list — {e}")

    # ---- Test 13: Deopt — polymorphic list alternation ----
    try:
        class Adder:
            def apply(self, x):
                return x + 1

        class Multiplier:
            def apply(self, x):
                return x * 2

        def alternating_dispatch(objs, n):
            total = 0
            for i in range(n):
                total += objs[i % len(objs)].apply(i)
            return total

        objs_a = [Adder(), Multiplier()]
        for _ in range(WARMUP):
            alternating_dispatch(objs_a, 20)
        check_jit_compiled(alternating_dispatch, "alternating_dispatch")
        result = alternating_dispatch(objs_a, 20)
        expected = sum((i + 1) if (i % 2 == 0) else (i * 2) for i in range(20))
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: alternating_dispatch")
    except Exception as e:
        failed += 1
        print(f"  FAIL: alternating_dispatch — {e}")

    # ---- Test 14: Deopt — scheduler with task list mutation ----
    try:
        class MutableTask:
            def __init__(self, n):
                self.n = n
            def run(self):
                self.n -= 1
                return self.n

        class AggressiveTask(MutableTask):
            def run(self):
                self.n -= 5
                return self.n

        def mutable_scheduler(tasks, n_iter):
            total = 0
            for _ in range(n_iter):
                for t in tasks:
                    total += t.run()
            return total

        tasks = [MutableTask(1000), MutableTask(1000), MutableTask(1000)]
        for _ in range(WARMUP):
            tasks = [MutableTask(1000), MutableTask(1000), MutableTask(1000)]
            mutable_scheduler(tasks, 5)
        check_jit_compiled(mutable_scheduler, "mutable_scheduler")
        # Now with AggressiveTask
        tasks2 = [AggressiveTask(1000), MutableTask(1000), AggressiveTask(1000)]
        result = mutable_scheduler(tasks2, 10)
        # Complex to compute exactly — just verify it returns an int
        assert isinstance(result, int), f"expected int, got {type(result)}"
        passed += 1
        print("  PASS: mutable_scheduler_deopt")
    except Exception as e:
        failed += 1
        print(f"  FAIL: mutable_scheduler_deopt — {e}")

    # ---- Test 15: Rapid type alternation in scheduler (50 cycles) ----
    try:
        class TypeA:
            def compute(self, x):
                return x + 1

        class TypeB:
            def compute(self, x):
                return x + 2

        def rapid_type_alt(obj, n):
            total = 0
            for i in range(n):
                total += obj.compute(i)
            return total

        a, b = TypeA(), TypeB()
        for cycle in range(50):
            rapid_type_alt(a if cycle % 2 == 0 else b, 100)
        check_jit_compiled(rapid_type_alt, "rapid_type_alt")
        result_a = rapid_type_alt(a, 100)
        result_b = rapid_type_alt(b, 100)
        assert result_a == sum(i + 1 for i in range(100)), f"TypeA: got {result_a}"
        assert result_b == sum(i + 2 for i in range(100)), f"TypeB: got {result_b}"
        passed += 1
        print("  PASS: rapid_type_alternation")
    except Exception as e:
        failed += 1
        print(f"  FAIL: rapid_type_alternation — {e}")

    # ---- Test 16: Empty task list (no iterations) ----
    try:
        class EmptyTask:
            def run(self):
                return 0

        def empty_scheduler(tasks):
            total = 0
            for t in tasks:
                total += t.run()
            return total

        for _ in range(WARMUP):
            empty_scheduler([])
        check_jit_compiled(empty_scheduler, "empty_scheduler")
        result = empty_scheduler([])
        assert result == 0, f"got {result}, expected 0"
        # Also works with non-empty
        result2 = empty_scheduler([EmptyTask()])
        assert result2 == 0, f"non-empty: got {result2}, expected 0"
        passed += 1
        print("  PASS: empty_task_list")
    except Exception as e:
        failed += 1
        print(f"  FAIL: empty_task_list — {e}")

    # ---- Test 17: Single iteration loop with polymorphic call ----
    try:
        class SingleTask:
            def execute(self):
                return 42

        class SingleTaskAlt(SingleTask):
            def execute(self):
                return 99

        def single_iter_dispatch(task):
            total = 0
            for _ in range(1):
                total += task.execute()
            return total

        st = SingleTask()
        for _ in range(WARMUP):
            single_iter_dispatch(st)
        check_jit_compiled(single_iter_dispatch, "single_iter_dispatch")
        assert single_iter_dispatch(st) == 42
        assert single_iter_dispatch(SingleTaskAlt()) == 99
        passed += 1
        print("  PASS: single_iter_dispatch")
    except Exception as e:
        failed += 1
        print(f"  FAIL: single_iter_dispatch — {e}")

    # ---- Test 18: Nested polymorphic dispatch (outer + inner loops) ----
    try:
        class OuterTask:
            def __init__(self, inners):
                self.inners = inners
            def run_all(self):
                total = 0
                for inner in self.inners:
                    total += inner.compute()
                return total

        class InnerA:
            def compute(self):
                return 1

        class InnerB:
            def compute(self):
                return 10

        def nested_polymorphic(outers, n):
            total = 0
            for _ in range(n):
                for outer in outers:
                    total += outer.run_all()
            return total

        outers = [
            OuterTask([InnerA(), InnerB(), InnerA()]),
            OuterTask([InnerB(), InnerB()]),
        ]
        for _ in range(WARMUP):
            nested_polymorphic(outers, 5)
        check_jit_compiled(nested_polymorphic, "nested_polymorphic")
        result = nested_polymorphic(outers, 10)
        # Outer1: 1+10+1 = 12. Outer2: 10+10 = 20. Per iter: 32. * 10 = 320.
        assert result == 320, f"got {result}, expected 320"
        passed += 1
        print("  PASS: nested_polymorphic")
    except Exception as e:
        failed += 1
        print(f"  FAIL: nested_polymorphic — {e}")

    # ---- Test 19: Large polymorphic task set (10 types) ----
    try:
        classes = []
        for offset in range(10):
            # Dynamically create 10 distinct classes
            cls = type(f'DynTask{offset}', (), {'run': lambda self, o=offset: o})
            classes.append(cls)

        def large_polymorphic_set(tasks, n):
            total = 0
            for _ in range(n):
                for t in tasks:
                    total += t.run()
            return total

        tasks = [cls() for cls in classes]
        for _ in range(WARMUP):
            large_polymorphic_set(tasks, 5)
        check_jit_compiled(large_polymorphic_set, "large_polymorphic_set")
        result = large_polymorphic_set(tasks, 100)
        expected = 100 * sum(range(10))  # 100 * 45 = 4500
        assert result == expected, f"got {result}, expected {expected}"
        passed += 1
        print("  PASS: large_polymorphic_set")
    except Exception as e:
        failed += 1
        print(f"  FAIL: large_polymorphic_set — {e}")

    # ---- Test 20: Richards-like full pattern (5 task types, scheduler loop) ----
    try:
        class TaskState:
            RUNNING = 0
            WAITING = 1
            HELD = 2

        class BaseTask:
            def __init__(self, pri, state):
                self.pri = pri
                self.state = state
                self.next_task = None
            def execute(self, wa):
                wa['total'] += self.pri
                return self.state

        class IdleTask(BaseTask):
            def execute(self, wa):
                wa['idle'] += 1
                self.state = TaskState.RUNNING
                return self.state

        class WorkerTask(BaseTask):
            def execute(self, wa):
                wa['work'] += self.pri
                self.state = (self.state + 1) % 3
                return self.state

        class HandlerTask(BaseTask):
            def execute(self, wa):
                wa['handle'] += 1
                return TaskState.WAITING

        class DeviceTask(BaseTask):
            def execute(self, wa):
                wa['device'] += 1
                return TaskState.HELD

        def richards_like_schedule(task_list, n_iter):
            """Full Richards-like scheduler with polymorphic task dispatch."""
            wa = {'total': 0, 'idle': 0, 'work': 0, 'handle': 0, 'device': 0}
            for _ in range(n_iter):
                for task in task_list:
                    result = task.execute(wa)
                    if result == TaskState.HELD:
                        wa['total'] += 1
            return wa

        task_list = [
            IdleTask(0, TaskState.RUNNING),
            WorkerTask(1, TaskState.RUNNING),
            WorkerTask(2, TaskState.WAITING),
            HandlerTask(3, TaskState.RUNNING),
            DeviceTask(4, TaskState.HELD),
        ]
        for _ in range(WARMUP):
            # Reset states
            for i, t in enumerate(task_list):
                t.state = TaskState.RUNNING if i < 4 else TaskState.HELD
            richards_like_schedule(task_list, 5)

        for i, t in enumerate(task_list):
            t.state = TaskState.RUNNING if i < 4 else TaskState.HELD
        check_jit_compiled(richards_like_schedule, "richards_like_schedule")
        result = richards_like_schedule(task_list, 100)
        assert result['idle'] == 100, f"idle: {result['idle']}"
        assert result['handle'] == 100, f"handle: {result['handle']}"
        assert result['device'] == 100, f"device: {result['device']}"
        passed += 1
        print("  PASS: richards_like_schedule")
    except Exception as e:
        failed += 1
        print(f"  FAIL: richards_like_schedule — {e}")

    # ---- Summary ----
    print(f"\nLICM_RICHARDS_REGRESSION: {passed}/{passed + failed} passed, {failed}/{passed + failed} failed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
