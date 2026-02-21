"""
kwargs_dispatch — Benchmark for *args/**kwargs packing/unpacking overhead.

Targets: keyword argument passing, **kwargs unpacking, *args forwarding,
         default argument handling, mixed positional+keyword calls.

Motivation: PyTorch APIs use extensive keyword arguments (lr=0.01,
momentum=0.9, weight_decay=1e-4). Every optimizer step, every layer
constructor, every functional call uses kwargs. The JIT must handle
argument packing/unpacking efficiently.
"""

import time


def compute(x, y, z=0.0, scale=1.0, bias=0.0, inplace=False):
    """Mimics a PyTorch functional with many kwargs."""
    result = (x * y + z) * scale + bias
    if inplace:
        return result
    return result * 1.0  # Force new object


def forward_args(*args, **kwargs):
    """Mimics argument forwarding (super().forward(*args, **kwargs))."""
    return compute(*args, **kwargs)


def apply_fn(fn, *args, **kwargs):
    """Mimics Module.apply / hook dispatch."""
    return fn(*args, **kwargs)


class Layer:
    def __init__(self, in_f=64, out_f=64, bias=True, dtype='float32',
                 device='cpu', requires_grad=True):
        self.in_f = in_f
        self.out_f = out_f
        self.has_bias = bias
        self.dtype = dtype
        self.device = device
        self.requires_grad = requires_grad
        self.weight = 0.01 * in_f

    def forward(self, x, *, training=True, mask=None):
        result = x * self.weight
        if self.has_bias:
            result += 0.01
        if mask is not None:
            result *= mask
        return result


class Optimizer:
    def __init__(self, params, lr=0.01, momentum=0.9, weight_decay=1e-4,
                 dampening=0.0, nesterov=False):
        self.params = list(params)
        self.lr = lr
        self.momentum = momentum
        self.weight_decay = weight_decay
        self.dampening = dampening
        self.nesterov = nesterov

    def step(self, closure=None):
        total = 0.0
        for p in self.params:
            grad = p * 0.01
            if self.weight_decay != 0:
                grad += p * self.weight_decay
            total += grad * self.lr
        return total


def benchmark_kwargs_dispatch(iterations=3000):
    """Exercise kwargs patterns in a hot loop."""
    layers = [Layer(in_f=i*8+8, out_f=(i+1)*8+8) for i in range(5)]
    params = [l.weight for l in layers]
    opt = Optimizer(params, lr=0.001, momentum=0.9, weight_decay=1e-4)

    total = 0.0

    for i in range(iterations):
        # Direct call with kwargs
        total += compute(total % 100, 0.5, z=0.1, scale=0.99, bias=0.001)

        # *args/**kwargs forwarding
        total += forward_args(total % 100, 0.5, z=0.2, scale=0.98)

        # Higher-order with kwargs
        total += apply_fn(compute, total % 100, 0.5, scale=0.97, inplace=True)

        # Method call with keyword-only args
        for layer in layers:
            total = layer.forward(total % 100, training=(i % 2 == 0))

        # Constructor with many kwargs (mimics Layer creation in loop)
        if i % 100 == 0:
            _ = Layer(in_f=32, out_f=64, bias=True, dtype='float16',
                      device='cpu', requires_grad=True)

        # Optimizer step with closure kwarg
        loss = opt.step(closure=None)
        total = (total + loss) % 10000

        # Mixed positional + keyword
        total += compute(total % 100, 0.5, 0.1, scale=0.99)

    return total


def main():
    # Warmup
    benchmark_kwargs_dispatch(iterations=100)

    # Timed run
    start = time.perf_counter_ns()
    result = benchmark_kwargs_dispatch(iterations=3000)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    print(f"kwargs_dispatch: {elapsed_ms:.3f}ms (result={result:.6f})")


if __name__ == "__main__":
    main()
