"""
nn_module_forward — Benchmark mimicking PyTorch nn.Module.forward() dispatch.

Targets: nn.Module attribute lookup pattern (__getattr__ → _parameters,
_modules, _buffers dict lookup), __call__ → forward() dispatch,
parameter iteration, and the training/eval mode pattern.

Motivation: This is THE hot path in PyTorch training. Every layer in a
neural network goes through Module.__call__ → Module.forward() on every
forward pass. The attribute access pattern (self.weight, self.bias,
self.layer1, etc.) goes through Module.__getattr__ because nn.Module
has a custom tp_getattro.

Structure:
  - Module base class mimicking nn.Module's __getattr__/__setattr__
  - Linear layer with weight/bias parameter access
  - Sequential container iterating over children
  - Forward pass exercising the full dispatch chain
"""

import time


class Parameter:
    """Mimics torch.nn.Parameter — a tensor with requires_grad."""
    def __init__(self, data):
        self.data = data
        self.grad = None
        self.requires_grad = True

    def __repr__(self):
        return f"Parameter({self.data})"


class Module:
    """Mimics torch.nn.Module's attribute lookup pattern."""

    def __init__(self):
        # These are the internal dicts nn.Module uses
        object.__setattr__(self, '_parameters', {})
        object.__setattr__(self, '_modules', {})
        object.__setattr__(self, '_buffers', {})
        object.__setattr__(self, 'training', True)

    def __getattr__(self, name):
        """Mimics nn.Module.__getattr__ — the hot path."""
        _parameters = self.__dict__.get('_parameters', {})
        if name in _parameters:
            return _parameters[name]
        _modules = self.__dict__.get('_modules', {})
        if name in _modules:
            return _modules[name]
        _buffers = self.__dict__.get('_buffers', {})
        if name in _buffers:
            return _buffers[name]
        raise AttributeError(f"'{type(self).__name__}' has no attribute '{name}'")

    def __setattr__(self, name, value):
        """Mimics nn.Module.__setattr__ — routes to internal dicts."""
        if isinstance(value, Parameter):
            self.__dict__.setdefault('_parameters', {})[name] = value
        elif isinstance(value, Module):
            self.__dict__.setdefault('_modules', {})[name] = value
        else:
            object.__setattr__(self, name, value)

    def __call__(self, *args, **kwargs):
        """Mimics nn.Module.__call__ — calls forward()."""
        return self.forward(*args, **kwargs)

    def parameters(self):
        """Yield all parameters (mimics nn.Module.parameters)."""
        for p in self._parameters.values():
            yield p
        for m in self._modules.values():
            for p in m.parameters():
                yield p

    def train(self, mode=True):
        self.training = mode
        for m in self._modules.values():
            m.train(mode)
        return self

    def eval(self):
        return self.train(False)


class Linear(Module):
    """Mimics torch.nn.Linear."""
    def __init__(self, in_features, out_features, bias=True):
        super().__init__()
        self.in_features = in_features
        self.out_features = out_features
        self.weight = Parameter(0.01 * in_features * out_features)
        if bias:
            self.bias = Parameter(0.01 * out_features)

    def forward(self, x):
        result = x * self.weight.data + (self.bias.data if hasattr(self, 'bias') else 0.0)
        return result


class ReLU(Module):
    """Mimics torch.nn.ReLU."""
    def forward(self, x):
        return max(0.0, x)


class Sequential(Module):
    """Mimics torch.nn.Sequential."""
    def __init__(self, *modules):
        super().__init__()
        for i, module in enumerate(modules):
            self._modules[str(i)] = module

    def forward(self, x):
        for module in self._modules.values():
            x = module(x)
        return x


class SimpleNet(Module):
    """A small network mimicking a typical PyTorch model."""
    def __init__(self):
        super().__init__()
        self.features = Sequential(
            Linear(64, 128),
            ReLU(),
            Linear(128, 64),
            ReLU(),
        )
        self.classifier = Linear(64, 10)
        self.dropout_rate = 0.5

    def forward(self, x):
        x = self.features(x)
        x = self.classifier(x)
        return x


def benchmark_nn_module_forward(iterations=2000):
    """Exercise the nn.Module dispatch chain in a hot loop."""
    model = SimpleNet()
    model.train()

    total = 0.0

    for i in range(iterations):
        # Forward pass — exercises __call__ → forward → attribute access chain
        x = float(i % 100) * 0.01
        output = model(x)
        total += output % 1000.0

        # Parameter access — exercises __getattr__ → _parameters dict lookup
        for p in model.parameters():
            total += p.data * 0.0001

        # Toggle train/eval every 100 iterations (mimics training loop phases)
        if i % 100 == 0:
            if model.training:
                model.eval()
            else:
                model.train()

        total = total % 10000.0

    return total


def main():
    # Warmup
    benchmark_nn_module_forward(iterations=100)

    # Timed run
    start = time.perf_counter_ns()
    result = benchmark_nn_module_forward(iterations=2000)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    print(f"nn_module_forward: {elapsed_ms:.3f}ms (result={result:.6f})")


if __name__ == "__main__":
    main()
