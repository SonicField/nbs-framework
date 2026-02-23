# Terminal Weathering

Rock does not shatter because someone decided it should be gravel. It transforms through sustained exposure — water finding existing cracks, dissolving weaker material, leaving stronger structures behind. The process is gradual, irreversible once proven, and indifferent to impatience.

Code transforms the same way, or it does not transform at all.

> **Archival note (February 2026):** The C-quality engineering standards from this methodology
> (ASan, leak analysis, refcount discipline, calling convention discipline) have been migrated
> to `concepts/engineering-standards.md` in the NBS framework. This methodology document is
> retained for reference but the engineering standards document is now the canonical source
> for C/CPython quality gates.

## The Anti-Pattern

"Rewrite for performance" is demolition. It assumes the replacement is better before evidence exists. It replaces an understood system with an unproven one in a single commitment. It is Ethos — trusting the authority of a language's reputation over measured outcomes.

The failure mode is predictable:

| Phase | What happens |
|-------|-------------|
| Announcement | "We are rewriting for performance" |
| Honeymoon | Early modules convert easily; team reports progress |
| Plateau | Complex modules resist; edge cases multiply; Python semantics prove non-trivial |
| Sunk cost | Too much invested to stop; too little working to ship |
| Outcome | Two half-working systems instead of one working one |

The root cause is not the target language. It is the absence of evidence gates. No individual conversion was required to prove its value. The decision was made once, at the top, and everything downstream was committed before falsification could occur.

## The Metaphor

Geological weathering operates on three principles that map directly:

1. **Existing cracks first.** Water does not attack solid rock. It finds joints, faults, grain boundaries — places where the material is already weak. In code: call protocol paths where CPython's dispatch chain creates measurable overhead.

2. **Surface inward.** Weathering works from exposed surfaces toward the interior. In code: leaf type slots first — those at the bottom of the dispatch chain — then progressively inward as evidence accumulates.

3. **Differential erosion.** Weaker material dissolves; stronger material remains. In code: some dispatch paths resist replacement because they derive genuine value from Python's dynamism. This is information, not failure.

## Why C, Not Rust

Rust was the right first choice.

The borrow checker provides compile-time memory safety guarantees that no other systems language offers. PyO3 provides clean Python interop. For replacing Python function bodies with compiled alternatives, Rust/PyO3 is a defensible — arguably optimal — choice.

We tried it. Four leaf functions in PyTorch's `nn.Module` and `torch.utils._pytree` were converted to Rust via PyO3. Correctness tests passed: 52/52 for the converted functions, 88/88 for the full suite. The first fusion was achieved — `_get_node_type` calling `is_namedtuple_class` directly in Rust, bypassing the Python layer entirely.

Then we measured. ABBA counterbalanced QPS benchmarks showed no significant performance effect: mean −1.4%, 95% CI [−5.4%, +2.5%], t = −0.82, p > 0.05. Four compiled functions, zero measurable impact.

The speed-bump experiment explained why. Adding 1µs at every `torch_python` function entry produced 30.4% QPS sensitivity. The overhead is in the **call protocol** — the dispatch chain that routes every Python method call through type slot lookup, MRO walk, descriptor protocol, bound method creation, and frame setup — not in the function bodies that execute once the dispatch arrives.

When Python executes `m.weight` on an `nn.Module`, CPython runs this chain:

```
m.weight
  → tp_getattro(m, "weight")                    # C: type slot dispatch
    → slot_tp_getattr_hook(m, "weight")          # C: looks up __getattr__ in type dict
      → _PyType_Lookup(Module, "__getattr__")    # C: MRO walk
      → call_attribute(m, getattr_func, "weight")# C: descriptor protocol
        → func_descr_get(func, m, Module)        # C: creates bound method
        → PyObject_CallOneArg(bound, "weight")   # C: call machinery
          → _PyEval_EvalFrame(...)               # C: frame setup
            ┌─────────────────────────────┐
            │  self._parameters["weight"] │      ← function body
            │  self._buffers["weight"]    │      ← PyO3/Rust replaced this
            │  self._modules["weight"]    │
            └─────────────────────────────┘
          → frame teardown                       # C: cleanup
```

PyO3 replaces what is inside the box. The dispatch chain outside the box — which runs ~80ns versus the body's ~50ns — is untouched. Shaving the body from ~50ns to ~30ns produces a ~15% improvement on a quantity that represents 0.03% of iteration time.

This is not a preference for C over Rust. It is a technical constraint: PyO3 cannot access CPython type slots. `tp_getattro`, `tp_setattro`, the slot dispatch machinery — these are CPython C API internals that PyO3 does not expose. Replacing the type slot means writing a C function and installing it directly via `PyType_Modified`, so CPython calls it with zero intermediate dispatch steps. There is no way to do this from Rust without writing the same C anyway.

The Rust work was not wasted:

- It validated the weathering methodology: the evidence gates, correctness-first approach, and ABBA benchmarking all worked exactly as designed.
- It produced the 52 correctness tests that define the behavioural contract for `__getattr__`, `__delattr__`, `is_namedtuple_class`, and `_get_node_type`. Those tests are reusable regardless of implementation language.
- It demonstrated that replacing function bodies does not address the actual bottleneck — a finding that directs all future effort to the correct layer.
- The `fast_getattr` logic (check `_parameters`, `_buffers`, `_modules` in order, raise `AttributeError` with the correct format) translates line-for-line into C.

A second, independent validation came from SOMA — a Python-based interpreted language. The same pattern emerged, but stronger: a Rust/PyO3 extension replacing four core VM types was **6% slower than pure Python**, because PyO3's safety abstractions (GIL token validation, borrow checking, bound/unbound conversions) added more overhead than they removed on fine-grained field access patterns. A C extension doing the same work via direct struct access and CPython API calls was **2.06x faster than Rust** — uniformly across all operation types (insert, lookup, remove, traversal), with CV < 1.3%. See [evidence/soma-weathering.md](../evidence/soma-weathering.md) for full measurements, and [evidence/weathering-at-the-right-layer.md](../evidence/weathering-at-the-right-layer.md) for the PyTorch call protocol analysis.

### C Requires Different Safety Gates

Rust's borrow checker catches use-after-free, double-free, and data races at compile time. C catches none of these. The argument for Rust over C originally was precisely this: unsupervised C conversion is gambling with memory safety.

That argument was correct, and it still is. The response is not to ignore the risk but to address it with different tools:

- **AddressSanitizer (ASan)** is mandatory during the correctness phase. Every C extension must compile and pass its test suite with ASan enabled. ASan catches heap buffer overflows, use-after-free, double-free, stack buffer overflows, and memory leaks at runtime.
- **Memory leak analysis** is mandatory at the Assess phase. Any C extension that leaks memory under the test suite fails the evidence gate, regardless of performance.
- **Reference count auditing** is required for all CPython API calls. Every `Py_INCREF` must have a corresponding `Py_DECREF` on every code path, including error paths.

These gates replace the compile-time guarantees that Rust provided. They are not optional extras. Without them, C conversion is precisely the unsupervised gambling that motivated choosing Rust in the first place.

## The Phases

Terminal weathering is iterative, not linear. Each cycle processes one candidate through six phases.

### Survey

Identify existing cracks. Not "what could be C" but "what is actually hurting."

- Profile performance: CPU hotspots, latency distributions
- Profile the call protocol: identify high-hit-count dispatch chains
- Map type slot usage; identify which slots route the most calls
- Identify code already marked problematic or scheduled for refactor

**Exit criterion**: Ranked list of call protocol paths with quantified overhead.

**Falsifier**: If no measurable overhead exists in the dispatch chain, stop. There is nothing to weather.

### Expose

Select a single candidate from the ranked list. It must be:

- A leaf type slot — one whose replacement does not depend on other slot replacements
- Measurably problematic — not "might be slow" but "is slow, here is the dispatch trace"
- Small enough to convert in one verification cycle

**Exit criterion**: Single type slot selected with baseline measurements recorded.

**Falsifier**: If the candidate cannot be isolated as a leaf slot, it is not ready. Decompose further or choose another.

### Weather

Apply the verification cycle to the selected candidate:

1. **Design**: C implementation replacing the type slot directly against CPython's type API
2. **Plan**: Identify what could go wrong — reference counting errors, GIL interactions, MRO invalidation, descriptor protocol violations
3. **Deconstruct**: Break into testable steps
4. **Test**: Write tests exercising the Python API through the C backend; write benchmarks; compile and test with ASan enabled
5. **Code**: Implement the C extension; install the replacement slot via `PyType_Modified`; the Python layer remains until proven redundant
6. **Document**: Record baseline versus post-conversion measurements; record ASan and leak analysis results

**Exit criterion**: Tests pass (including under ASan), benchmarks collected, Python API unchanged, no memory leaks detected.

**Falsifier**: "This slot replacement provides measurable benefit" — attempt to falsify by benchmarking under realistic load, testing edge cases the Python dispatch handled implicitly, and measuring total system impact rather than isolated slot performance.

### Assess

The evidence gate. Three outcomes, no others:

1. **Benefit confirmed**: Measurements show improvement beyond noise. ASan clean. No leaks. Mark conversion as permanent. Proceed.
2. **Benefit unclear**: Measurements are ambiguous. More data needed. Do not proceed until resolved.
3. **Benefit falsified**: Measurements show no improvement, or regression, or memory safety violations. Revert. Document what was learned.

Outcome 3 is not failure. It is the methodology working. A reverted conversion that taught us "this dispatch path resists replacement because of X" is more valuable than a committed conversion nobody measured.

**Falsifier**: If we cannot distinguish outcomes 1–3 with evidence, our measurement methodology is wrong. Fix that before proceeding with any conversion.

### Advance

With proven slot replacements, the next layer becomes accessible:

- Former near-leaf slots may now be leaves, their dependencies already replaced
- Patterns emerge: which types of slot replacement yield benefit, which do not
- Rules of thumb develop — but each conversion still passes its own evidence gate

No blanket rules. "Attribute access slots convert well" is a hypothesis to test per candidate, not a policy to apply wholesale.

**Exit criterion**: Next candidate selected based on updated slot dependency map and accumulated evidence.

### Fuse

When sufficient contiguous slot coverage exists within a type, consider removing the Python layer entirely. This is a separate verification cycle with its own evidence gate.

Risks specific to fusion: Python-side consumers, dynamic dispatch, monkey-patching in test fixtures, implicit interface contracts, subclass slot inheritance.

**Falsifier**: "Removing the Python layer does not break any consumer" — test exhaustively. If any consumer breaks, the Python layer remains.

## The Trust Gradient

Human oversight is expensive. Applying full oversight to every conversion does not scale. But removing oversight without evidence is negligence.

Terminal weathering defines four oversight levels, ordered from tightest to loosest:

| Level | Oversight | When |
|-------|-----------|------|
| Tight | Human reviews every step of every conversion | Initial conversions; no evidence base yet |
| Gate | Human reviews evidence at Assess phase only | Pattern of successful conversions established |
| Batch | Human reviews evidence for batches of conversions | Strong evidence base; consistent patterns |
| Review | Human spot-checks; AI flags anomalies | Extensive track record; mature measurement infrastructure |

**Transitions are earned, not assumed.** Moving from Tight to Gate requires N consecutive conversions where the human's review found no issues the evidence gates missed. The specific N is project-dependent.

**Transitions are reversible.** A single conversion where oversight level was insufficient — the human discovers a problem the evidence gate missed — reverts the level. Trust is slow to build and fast to lose.

**The gradient applies per conversion type, not globally.** Attribute access slot conversions may earn Gate level while call protocol slots remain at Tight. Each domain of conversion builds its own trust independently.

## NBS Alignment

Terminal weathering is not a new methodology. It is the existing NBS pillars applied to call protocol optimisation.

| Pillar | Application |
|--------|-------------|
| Goals | The terminal goal is system improvement. Language replacement is instrumental. If the system is not measurably better, the conversion has no purpose |
| Falsifiability | Each conversion carries "this slot replacement provides measurable benefit" as a falsifiable claim. The Assess phase exists to attempt falsification |
| Rhetoric | "C is faster" is Ethos. "Replacing `tp_getattro` on `Module` reduces attribute access from 130ns to 50ns under production load" is Logos. Only the second is acceptable as evidence |
| Bullshit Detection | Report failed conversions. Report ambiguous results. Report ASan findings. A conversion log showing 100% success rate is either dishonest or insufficiently ambitious |
| Verification Cycle | Each conversion is one full cycle: Design, Plan, Deconstruct, Test, Code, Document. ASan and leak analysis are part of Test. No shortcuts |
| Zero-Code Contract | The Engineer selects targets and defines "benefit". The Machinist implements and reports evidence. Neither trusts the other's assertions |

## NBS Teams Integration

For codebases at scale — millions of lines — terminal weathering maps onto the supervisor/worker pattern:

**Supervisor** maintains the ranked candidate list, assigns individual conversions to workers, aggregates evidence across conversions, detects cross-conversion patterns, and holds the evidence gates. The Supervisor decides whether a conversion passes the Assess phase — workers report, they do not adjudicate.

**Workers** execute individual verification cycles. One conversion per worker. Each worker operates on an isolated branch, runs the full Weather phase, and returns evidence to the Supervisor.

The trust gradient applies at the Supervisor level. As evidence accumulates, the Supervisor may batch-assign conversions at Gate or Batch oversight levels, but retains the ability to revert to Tight for any conversion type where evidence is thin.

## The Practical Questions

1. What is actually hurting? Can I point to a dispatch trace, a slot hit count — not a hunch?
2. Is this candidate a leaf slot? If not, what must be replaced first?
3. What would prove this slot replacement does not help? Have I tried to prove it?
4. Am I converting because of evidence, or because "C is faster" and I have not questioned that?
5. What oversight level has this type of conversion earned? What evidence supports that level?
6. What failed conversions have I documented? What did they teach me?
7. Have I run ASan and leak analysis? What did they find?
8. Does the Rust-to-C journey apply here, or am I pattern-matching from a different context?

---

## Pillar Check

Have you read all pillars in this session?

- goals.md
- falsifiability.md
- rhetoric.md
- bullshit-detection.md
- verification-cycle.md
- zero-code-contract.md
- pte.md
- terminal-weathering.md *(you are here)*
- engineering-standards.md

If you cannot clearly recall reading each one, read them now. Next: `goals.md`
