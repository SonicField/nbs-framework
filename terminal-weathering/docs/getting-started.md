# Getting Started with Terminal Weathering

## Prerequisites

1. **NBS framework installed.** Follow the instructions in the main [getting-started guide](../../docs/getting-started.md). The `/nbs-terminal-weathering` command must be available in Claude Code.

2. **Profiling tools.** The research phase requires profiling before any conversion begins. At least one of:
   - `py-spy` — sampling profiler for Python (recommended for CPU hotspots)
   - `cProfile` — built-in Python profiler
   - `tracemalloc` — built-in memory tracker
   - `memray` — detailed memory profiler
   - `perf` — Linux performance counters, essential for call protocol analysis:
     ```bash
     # Debian/Ubuntu
     sudo apt install linux-tools-common linux-tools-$(uname -r)

     # Verify
     perf --version
     ```

3. **Approach-specific toolchain.** Not required until the research phase selects an approach. Once selected:

   - **For C extension types** (typical approach when structural overhead is identified):
     - C compiler (GCC or Clang with C11 support): `gcc --version` or `clang --version`
     - CPython development headers: `python3-config --includes`
     - AddressSanitizer (built into GCC/Clang): `echo 'int main(){return 0;}' | gcc -fsanitize=address -x c - -o /dev/null`
     - Valgrind: `valgrind --version`
     - setuptools: `pip install setuptools`

   - **For Rust/PyO3** (body replacement):
     - Rust toolchain: `rustup show`
     - PyO3: `cargo add pyo3`
     - maturin: `pip install maturin`

   - **For algorithmic changes**: No additional toolchain — standard Python development environment.

---

## Starting a Session

Run the command:

```
/nbs-terminal-weathering
```

The tool detects context automatically and dispatches to the correct phase. On first run with no existing state, it begins with **goal setting**.

### Goal Setting

The tool asks for a terminal goal. This is not "rewrite in C" or "convert to Rust." It is a measurable system improvement:

- "Reduce P99 latency from 45ms to 15ms"
- "Reduce peak memory from 2GB to 500MB"
- "Eliminate call protocol overhead for Cell attribute access"
- "Achieve 2x throughput on the RB-tree benchmark"

If the goal is not falsifiable — "make it faster" without specifying faster than what, by how much, measured how — the tool pushes back. This is deliberate.

### State Creation

Once the goal is confirmed, the tool creates the state directory:

```
.nbs/terminal-weathering/
├── status.md          # Current phase, terminal goal, worker count
├── candidates.md      # Ranked conversion candidates (populated after survey)
├── trust-levels.md    # Trust gradient per conversion type
├── patterns.md        # Compressed learnings (initially empty)
└── conversions/       # One file per attempted conversion
```

All state lives in these files. Not in conversation history, not in memory. The tool reads them on every invocation to determine what to do next.

---

## The Research Phase

With the goal set, the tool moves to the research phase. This is the critical phase that determines the entire approach.

### What happens

1. **Profile the system.** The tool asks you to run profiling tools against a representative workload. The profile identifies where time is actually spent — not where you think it is spent.

2. **Classify the overhead.** Based on the profile, the overhead is classified:
   - *Structural*: Object model — attribute access, type checking, memory layout
   - *Dispatch*: Call protocol — type slot dispatch, MRO walk, frame setup
   - *Computational*: Loop bodies — the actual work inside functions
   - *Algorithmic*: Complexity — O(n²) where O(n log n) is possible

3. **Form a hypothesis.** A falsifiable statement with a quantitative prediction: "The overhead mechanism is X, because Y. Intervention Z should reduce it by approximately W."

4. **Run a falsification experiment.** Speed-bump tests, boundary-crossing benchmarks, or synthetic workloads that isolate the hypothesised mechanism.

5. **Select an approach — or stop.** If the experiment supports the hypothesis, select an architectural approach (C extension types, type slot replacement, body replacement, algorithm change). If not, the tool honestly reports that no intervention will help. Both are valid outcomes.

### Toolchain verification

Once the approach is selected, the tool verifies the required toolchain is available. For C extension types, this means checking for a C compiler, CPython headers, and ASan. For Rust, this means checking for the Rust toolchain and PyO3. The toolchain check is deferred to this point — we don't check for a C compiler before we know C is the right approach.

### Worked examples

**SOMA (success path)**: Profile → structural overhead (field access). Hypothesis → "C extension types, ~2x speedup." Experiment → Rust/PyO3 boundary-crossing benchmark falsified Rust (6% slower). C extension types confirmed (2.06x faster). Approach selected: C extension types for data containers. Toolchain verified. Proceeded to Survey.

**PyTorch (stop path)**: Profile → dispatch overhead (call protocol). Hypothesis → "Type slot replacement, ~60% per-call reduction." Experiment → speed-bump test confirmed mechanism. But type slot replacement produced no measurable whole-system effect because the dynamism was load-bearing. Approach: stop. No toolchain needed.

---

## What Happens Next: Survey

With the research phase complete and an approach selected, the tool moves to the survey phase. It profiles the specific domain identified by the research phase to find what is actually hurting.

Key activities during survey:
- **Within the identified domain**: Profile the specific overhead mechanism. If structural, identify which types have the highest access frequency. If dispatch, identify which dispatch chains have the highest hit counts.
- **Dependency mapping**: Identify leaf candidates — those whose replacement does not depend on other unreplaced units.
- **Quantified ranking**: Rank candidates by total overhead contribution (frequency × per-operation cost).

The output is a ranked list of candidates with quantified overhead. If profiling reveals that the overhead is negligible in the identified domain, the survey says so. There is nothing to weather. This is an honest outcome, not a failure.

---

## A Single Conversion Cycle

Here is what one cycle looks like end to end, assuming the research phase has selected an approach and the survey has produced candidates.

### 1. Expose

The tool selects the highest-ranked candidate — a leaf in the dependency graph (no deeper dependencies that must be replaced first) and measurably problematic. It records baseline measurements and creates a branch:

```bash
git checkout -b weathering/<target>/<component>
```

A conversion record is created in `.nbs/terminal-weathering/conversions/` with the hypothesis, falsifier, and baseline numbers.

### 2. Weather

The verification cycle runs against the candidate:

- **Design** — Implementation using the approach selected by the research phase. For C extension types: C struct with direct field access. For type slot replacement: C function installed at the slot level. For body replacement: compiled implementation via the appropriate toolchain.
- **Plan** — Identify what could go wrong: the risks are approach-specific (reference counting for C, ownership for Rust, semantic drift for any replacement).
- **Deconstruct** — Break into testable steps.
- **Test** — Tests exercising the Python API through the replacement backend, plus benchmarks. **Mandatory safety gates** appropriate to the approach (ASan/Valgrind/refcount for C; clippy/miri for Rust; full test suite for all).
- **Code** — Implement the replacement. The Python layer remains as an overlay until proven redundant.
- **Document** — Record baseline versus post-conversion measurements. Include safety gate output as evidence artefacts.

At the initial trust level (Tight), every step is confirmed with the human. As trust is earned, oversight reduces.

### 3. Assess

The evidence gate. Mandatory checks appropriate to the approach, then three performance outcomes:

| Verdict | What Happens |
|---------|-------------|
| **Benefit confirmed** | Merge branch. Mark conversion permanent. Proceed. |
| **Benefit unclear** | More data needed. Do not merge. |
| **Benefit falsified** | Revert. Document what was learned. Choose next candidate. |

The actual result is compared against the research phase prediction. If the result does not match, the discrepancy is flagged.

### 4. Advance

Back on main. The dependency graph is updated — proven replacements may expose new accessible candidates. The candidate list is re-ranked. The next cycle begins.

---

## Resuming a Session

Run `/nbs-terminal-weathering` again. The tool reads `.nbs/terminal-weathering/status.md`, detects which phase you are in, and resumes from there. No reconfiguration needed.

| Signal | What the Tool Does |
|--------|-------------------|
| No state directory | Starts goal setting |
| Directory exists, no `research.md` | Runs research phase |
| `research.md` exists, candidates empty | Runs survey |
| On `main`/`master`, candidates ranked | Selects next candidate (Expose) |
| On a `weathering/*` branch | Continues the in-progress conversion (Weather) |
| Conversion complete on branch | Runs the evidence gate (Assess) |
| Back on main, goal not met | Advances to next candidate |
| Terminal goal met | Produces final report |
