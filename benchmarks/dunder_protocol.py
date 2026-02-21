"""
dunder_protocol — Benchmark for Python dunder/magic method dispatch.

Targets: __getattr__, __setattr__, __call__, __len__, __iter__, __next__,
         __contains__, __repr__, __bool__.

Motivation: PyTorch's nn.Module uses __getattr__ for parameter/submodule
lookup, __call__ for forward(), __repr__ for model printing, and __setattr__
for parameter registration. These dispatch through C slot functions
(tp_getattro, tp_call, etc.) which are Phase 3 C→C inlining targets.

Structure:
  - A Module-like class with attributes stored in a dict (mimics nn.Module)
  - A Container class with __len__, __iter__, __contains__ (mimics ParameterList)
  - A Callable class with __call__ (mimics Module.forward)
  - Hot loop exercising all dunder paths

Expected JIT behaviour:
  - Without C→C inlining: each dunder call goes through tp_* slot dispatch
  - With C→C inlining: slot functions expanded inline, eliminating call overhead
"""

import time


class Module:
    """Mimics nn.Module's attribute lookup pattern."""

    def __init__(self, name, depth=0):
        # Store in _parameters dict, accessed via __getattr__
        object.__setattr__(self, '_parameters', {})
        object.__setattr__(self, '_modules', {})
        object.__setattr__(self, '_name', name)
        for i in range(5):
            self._parameters[f'weight_{i}'] = float(i) * 0.1
            self._parameters[f'bias_{i}'] = float(i) * 0.01

        # Create submodules (mimics nn.Sequential children)
        if depth < 3:
            for i in range(3):
                self._modules[f'layer_{i}'] = Module(f'{name}_L{i}', depth + 1)

    def __getattr__(self, name):
        if name in self.__dict__.get('_parameters', {}):
            return self._parameters[name]
        if name in self.__dict__.get('_modules', {}):
            return self._modules[name]
        raise AttributeError(f"'{type(self).__name__}' has no attribute '{name}'")

    def __setattr__(self, name, value):
        if isinstance(value, float):
            self.__dict__.setdefault('_parameters', {})[name] = value
        elif isinstance(value, Module):
            self.__dict__.setdefault('_modules', {})[name] = value
        else:
            object.__setattr__(self, name, value)

    def __call__(self, x):
        # Mimics Module.forward dispatch
        return x + self._parameters.get('weight_0', 0.0)

    def __repr__(self):
        params = len(self._parameters)
        modules = len(self._modules)
        return f"Module({self._name}, params={params}, modules={modules})"

    def __bool__(self):
        return True


class Container:
    """Mimics ParameterList/ModuleList iteration patterns."""

    def __init__(self, items):
        self._items = list(items)

    def __len__(self):
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __contains__(self, item):
        return item in self._items

    def __getitem__(self, idx):
        return self._items[idx]


def benchmark_dunder_protocol(iterations=2000):
    """Exercise all dunder paths in a hot loop."""

    # Setup: create a Module tree (mimics model = nn.Sequential(...))
    model = Module("root")
    params = Container(model._parameters.values())

    total = 0.0

    for _ in range(iterations):
        # __getattr__ (parameter lookup)
        w0 = model.weight_0
        w1 = model.weight_1
        b0 = model.bias_0
        total += w0 + w1 + b0

        # __setattr__ (parameter update, mimics optimizer step)
        model.weight_0 = w0 * 0.99
        model.bias_0 = b0 * 0.99

        # __call__ (forward pass dispatch)
        result = model(total)
        total = result % 1000.0  # Bound to prevent overflow

        # __len__ + __iter__ (parameter iteration, mimics param groups)
        n = len(params)
        for p in params:
            total += p * 0.001

        # __contains__ (parameter membership check)
        if 0.1 in params:
            total += 0.001

        # __repr__ (model printing, mimics logging)
        _ = repr(model)

        # __bool__ (truthiness check, mimics if model: ...)
        if model:
            total += 0.0001

        # __getattr__ on submodule (deep attribute chain)
        layer = model.layer_0
        sub_layer = layer.layer_1
        total += sub_layer(total)

    return total


def main():
    # Warmup
    benchmark_dunder_protocol(iterations=100)

    # Timed run
    start = time.perf_counter_ns()
    result = benchmark_dunder_protocol(iterations=2000)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    print(f"dunder_protocol: {elapsed_ms:.3f}ms (result={result:.6f})")


if __name__ == "__main__":
    main()
