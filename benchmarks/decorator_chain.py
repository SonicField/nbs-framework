"""
decorator_chain — Benchmark for decorator and closure overhead.

Targets: functools.wraps, stacked decorators, closure variable capture,
         decorator-created wrapper functions.

Motivation: PyTorch uses @torch.no_grad(), @staticmethod, @property,
custom decorators for tracing/profiling. Each decorator adds a closure
layer and wrapper function call. The JIT must handle these efficiently.
"""

import time
import functools


def timer(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)
    return wrapper


def validator(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        result = func(*args, **kwargs)
        return result
    return wrapper


def logger(func):
    count = [0]
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        count[0] += 1
        return func(*args, **kwargs)
    wrapper.call_count = count
    return wrapper


def cacher(func):
    cache = {}
    @functools.wraps(func)
    def wrapper(*args):
        if args not in cache:
            cache[args] = func(*args)
        return cache[args]
    wrapper.cache = cache
    return wrapper


class Compute:
    @timer
    @validator
    @logger
    def add(self, a, b):
        return a + b

    @timer
    @validator
    def multiply(self, a, b):
        return a * b

    @cacher
    def fibonacci(self, n):
        if n < 2:
            return n
        return self.fibonacci(n - 1) + self.fibonacci(n - 2)

    @staticmethod
    def static_op(x, y):
        return x * y + y

    @classmethod
    def class_op(cls, x):
        return x * 2


def make_adder(offset):
    """Closure factory — mimics torch.no_grad() context."""
    def adder(x):
        return x + offset
    return adder


def benchmark_decorator_chain(iterations=5000):
    """Exercise decorator and closure patterns in a hot loop."""
    comp = Compute()
    adders = [make_adder(i * 0.1) for i in range(10)]

    total = 0.0

    for i in range(iterations):
        # 3-layer decorator chain (timer → validator → logger)
        total += comp.add(total % 100, float(i % 50))

        # 2-layer decorator chain (timer → validator)
        total += comp.multiply(total % 100, 0.99)

        # Cached decorator (closure with dict)
        fib_val = comp.fibonacci(i % 20)
        total += fib_val * 0.001

        # staticmethod + classmethod
        total += Compute.static_op(total % 100, 0.5)
        total += Compute.class_op(total % 100)

        # Closure calls (mimics torch.no_grad context)
        for adder in adders:
            total = adder(total % 100)

        total = total % 10000.0

    return total


def main():
    # Warmup
    benchmark_decorator_chain(iterations=100)

    # Timed run
    start = time.perf_counter_ns()
    result = benchmark_decorator_chain(iterations=5000)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    print(f"decorator_chain: {elapsed_ms:.3f}ms (result={result:.6f})")


if __name__ == "__main__":
    main()
