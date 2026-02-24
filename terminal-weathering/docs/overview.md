# Terminal Weathering

## What It Is

Terminal weathering is a methodology for hypothesis-driven performance optimisation of Python systems. It characterises the target system, identifies the overhead mechanism, selects an architectural approach supported by experimental evidence, and then applies progressive replacement through evidence-gated cycles. It is not a rewrite strategy. It is the NBS pillars — goals, falsifiability, verification cycle, zero-code contract — applied to performance optimisation.

The name carries a dual meaning:

- **Terminal**: the work uses terminal-based tools — CLI, pty-session, tmux. The target is a terminal goal: a measurable system improvement, not "rewrite in another language."
- **Weathering**: geological weathering transforms rock from the surface inward. Water finds existing cracks, dissolves weaker material, leaves stronger structures behind. The methodology works from the outermost measurable overhead inward, progressively replacing Python with compiled alternatives where evidence shows the overhead justifies it.

---

## The Problem It Solves

Two anti-patterns plague Python performance work:

1. **Rewriting without evidence.** "Rewrite for performance" is demolition. It replaces an understood system with an unproven one in a single commitment, without requiring evidence at any stage. The failure mode is predictable: two half-working systems instead of one working one.

2. **Optimising without diagnosis.** Even with evidence gates, if the intervention targets the wrong overhead mechanism, the work produces correct, safe, well-measured code that does not help. The overhead was real — but it was in dispatch, not data access; or in the algorithm, not the language boundary. The rewrite anti-pattern skips evidence. The wrong-mechanism anti-pattern skips diagnosis.

Terminal weathering addresses both: the research phase provides diagnosis; the weathering phases provide evidence-gated intervention.

---

## How It Works

Terminal weathering operates in two stages: a research phase that characterises the system, followed by iterative weathering cycles that apply the intervention.

### The Research Phase

Before any conversion begins, the system is profiled, the overhead mechanism is classified, a hypothesis is formed with a quantitative prediction, and a falsification experiment is run. The research phase selects an architectural approach — or concludes that no intervention will help. Both are valid outcomes.

**Worked example — PyTorch**: Profiling showed call protocol dispatch overhead (~80ns) dominating function body cost (~50ns). Hypothesis: "Type slot replacement will reduce per-call cost by ~60%." Speed-bump experiment confirmed the mechanism. But the dynamism (arbitrary `__getattr__` overrides, deep MRO hierarchies) was load-bearing. Whole-system effect: unmeasurable. Research conclusion: "stop." See [evidence/weathering-at-the-right-layer.md](../evidence/weathering-at-the-right-layer.md).

**Worked example — SOMA**: Profiling showed data container field access dominating runtime. Hypothesis: "C extension types will reduce per-access cost from ~80ns to ~5ns, yielding ~2x overall speedup." Rust/PyO3 boundary-crossing benchmark falsified the Rust approach (6% slower than Python). C extension types confirmed the hypothesis: 2.06x faster, uniformly across all operations. Research conclusion: "C extension types for data containers." See [evidence/soma-weathering.md](../evidence/soma-weathering.md).

### The Weathering Phases

Once the research phase selects an approach, iterative weathering cycles apply the intervention:

| Phase | Purpose |
|-------|---------|
| **Survey** | Within the identified domain, find where overhead is measurable. Rank candidates. |
| **Expose** | Select one leaf candidate with measured overhead. Record baseline. |
| **Weather** | Apply the verification cycle: Design, Plan, Deconstruct, Test (with safety gates), Code, Document. |
| **Assess** | The evidence gate. Benefit confirmed, unclear, or falsified. Three outcomes, no others. |
| **Advance** | Update the dependency map. Select the next candidate. |
| **Fuse** | When contiguous coverage exists, consider removing the Python layer entirely. |

Every conversion carries a falsifiable claim: "this replacement provides measurable benefit." The Assess phase exists to attempt falsification. A conversion that fails this gate is reverted — and that reversion is a positive outcome, not a failure. It means the methodology is working.

---

## Relation to NBS

Terminal weathering is an **application** of the NBS pillars, not a pillar itself.

| Pillar | Application in Terminal Weathering |
|--------|-----------------------------------|
| Goals | The terminal goal is system improvement. Language replacement is instrumental. |
| Falsifiability | The research phase carries a quantitative prediction. Each conversion carries a falsifiable hypothesis. |
| Rhetoric | "C is faster" is Ethos. "Replacing Cell field access with C struct dereference reduces per-access cost from 80ns to 2ns under production load" is Logos. Only the second is acceptable. |
| Bullshit Detection | Failed conversions are reported. Research phases that conclude "stop" are reported. A 100% success rate is either dishonest or insufficiently ambitious. |
| Verification Cycle | Each conversion is one full cycle. Safety gates are part of Test. No shortcuts. |
| Zero-Code Contract | The human defines "benefit." The AI implements and reports evidence. Neither trusts the other's assertions. |

---

## The Trust Gradient

Human oversight does not scale to every step of every conversion. But removing oversight without evidence is negligence.

Terminal weathering defines four oversight levels — Tight, Gate, Batch, Review — earned through consecutive successes and reverted on a single failure. Trust is slow to build and fast to lose. The gradient applies per conversion type, not globally.

See [methodology.md](methodology.md) for details.

---

## The Epistemic Garbage Collector

Every three conversion workers, the supervisor spawns a compression worker to distil raw learnings into patterns, then runs `/nbs` for goal alignment and drift detection. The compression worker also tracks research phase prediction accuracy — comparing each conversion's actual result against the predicted range. This prevents epistemic debt from accumulating unchecked.

See [methodology.md](methodology.md) for the full mechanism.
