"""
context_manager — Benchmark for with-statement protocol overhead.

Targets: __enter__/__exit__, nested context managers, contextlib patterns,
         exception handling in __exit__.

Motivation: PyTorch training loops use torch.no_grad(), torch.autocast(),
torch.cuda.amp.autocast() as context managers. These are called thousands
of times per epoch. The with-statement protocol involves __enter__ and
__exit__ calls through C slot dispatch.
"""

import time
import contextlib


class NoGrad:
    """Mimics torch.no_grad() — sets/restores a global flag."""
    _enabled = True

    def __enter__(self):
        self._prev = NoGrad._enabled
        NoGrad._enabled = False
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        NoGrad._enabled = self._prev
        return False


class Autocast:
    """Mimics torch.autocast() — sets/restores precision mode."""
    _mode = 'float32'

    def __init__(self, mode='float16'):
        self._target = mode

    def __enter__(self):
        self._prev = Autocast._mode
        Autocast._mode = self._target
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        Autocast._mode = self._prev
        return False


class ProfileScope:
    """Mimics profiler scope — tracks entry/exit counts."""
    _depth = 0
    _total = 0

    def __init__(self, name):
        self._name = name

    def __enter__(self):
        ProfileScope._depth += 1
        ProfileScope._total += 1
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        ProfileScope._depth -= 1
        return False


@contextlib.contextmanager
def training_mode(model_dict, mode=True):
    """Mimics model.train()/model.eval() as context manager."""
    prev = model_dict.get('training', True)
    model_dict['training'] = mode
    try:
        yield model_dict
    finally:
        model_dict['training'] = prev


def benchmark_context_manager(iterations=5000):
    """Exercise context manager patterns in a hot loop."""
    model = {'training': True, 'weight': 1.0, 'bias': 0.0}
    total = 0.0

    for i in range(iterations):
        # Single context manager (mimics with torch.no_grad():)
        with NoGrad():
            total += model['weight'] * float(i % 100) + model['bias']

        # Nested context managers (mimics training loop)
        with NoGrad():
            with Autocast('float16'):
                total += total % 1000 * 0.99

        # Triple nesting (mimics profiled autocast inference)
        with ProfileScope('forward'):
            with NoGrad():
                with Autocast('bfloat16'):
                    total = (total % 1000) + 0.001

        # contextlib-based context manager
        with training_mode(model, mode=False) as m:
            total += m['weight'] * 0.5

        # Rapid enter/exit (mimics per-layer context)
        for j in range(5):
            with ProfileScope(f'layer_{j}'):
                total = (total + float(j)) % 10000

    return total


def main():
    # Warmup
    benchmark_context_manager(iterations=100)

    # Timed run
    start = time.perf_counter_ns()
    result = benchmark_context_manager(iterations=5000)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    print(f"context_manager: {elapsed_ms:.3f}ms (result={result:.6f})")


if __name__ == "__main__":
    main()
