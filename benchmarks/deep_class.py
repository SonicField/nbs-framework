"""
deep_class — Benchmark for deep class hierarchy with super() chains.

Targets: MRO resolution, super().__init__(), multiple inheritance,
         isinstance checks, class attribute lookup through inheritance chain.

Motivation: PyTorch's nn.Module hierarchy is typically 3-5 levels deep.
Every Module.__init__ calls super().__init__(). Every forward() call
resolves through the MRO. isinstance() checks are used extensively
for type dispatch in nn.utils and optim.

Structure:
  - 5-level class hierarchy (Base → Layer → Block → Network → Model)
  - Each level adds attributes and calls super().__init__()
  - Hot loop: create instances, call methods, check isinstance
"""

import time


class Base:
    def __init__(self, name):
        self.name = name
        self.training = True
        self._forward_hooks = []

    def parameters(self):
        return [v for k, v in self.__dict__.items() if isinstance(v, float)]

    def train(self, mode=True):
        self.training = mode
        return self


class Layer(Base):
    def __init__(self, name, in_features, out_features):
        super().__init__(name)
        self.in_features = in_features
        self.out_features = out_features
        self.weight = 0.01 * in_features * out_features
        self.bias = 0.01 * out_features

    def forward(self, x):
        return x * self.weight + self.bias


class Block(Layer):
    def __init__(self, name, features, num_layers=3):
        super().__init__(name, features, features)
        self.num_layers = num_layers
        self.scale = 1.0 / num_layers
        self.layers = [Layer(f"{name}_sub_{i}", features, features)
                       for i in range(num_layers)]

    def forward(self, x):
        residual = x
        for layer in self.layers:
            x = layer.forward(x) * self.scale
        return x + residual


class Network(Block):
    def __init__(self, name, features, num_blocks=2):
        super().__init__(name, features, num_layers=3)
        self.num_blocks = num_blocks
        self.blocks = [Block(f"{name}_block_{i}", features)
                       for i in range(num_blocks)]

    def forward(self, x):
        for block in self.blocks:
            x = block.forward(x)
        return x


class Model(Network):
    def __init__(self, name, features=64, num_blocks=2):
        super().__init__(name, features, num_blocks)
        self.classifier_weight = 0.01 * features
        self.classifier_bias = 0.001

    def forward(self, x):
        x = super().forward(x)
        return x * self.classifier_weight + self.classifier_bias

    def __repr__(self):
        return (f"Model({self.name}, features={self.in_features}, "
                f"blocks={self.num_blocks})")


def benchmark_deep_class(iterations=500):
    """Exercise deep class hierarchy in a hot loop."""
    total = 0.0

    for _ in range(iterations):
        # Object creation through 5-level __init__ chain
        model = Model("bench", features=32, num_blocks=2)

        # Method resolution through MRO
        result = model.forward(1.0)
        total += result % 100.0

        # isinstance chain (mimics type dispatch)
        if isinstance(model, Model):
            total += 0.001
        if isinstance(model, Network):
            total += 0.001
        if isinstance(model, Block):
            total += 0.001
        if isinstance(model, Layer):
            total += 0.001
        if isinstance(model, Base):
            total += 0.001

        # Attribute lookup through inheritance
        _ = model.training     # from Base
        _ = model.in_features  # from Layer
        _ = model.num_layers   # from Block
        _ = model.num_blocks   # from Network

        # Method call on base class
        model.train(False)
        params = model.parameters()
        total += len(params) * 0.001

        # repr through MRO
        _ = repr(model)

    return total


def main():
    # Warmup
    benchmark_deep_class(iterations=50)

    # Timed run
    start = time.perf_counter_ns()
    result = benchmark_deep_class(iterations=500)
    elapsed_ns = time.perf_counter_ns() - start
    elapsed_ms = elapsed_ns / 1_000_000

    print(f"deep_class: {elapsed_ms:.3f}ms (result={result:.6f})")


if __name__ == "__main__":
    main()
