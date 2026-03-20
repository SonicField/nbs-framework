# When the Team Is Wrong: Measurement Drift in Multi-Agent AI Systems

## Abstract

A team of eight AI agents spent ten sessions optimising a JIT compiler. They produced twelve commits, six thousand chat messages, two hundred scribe-logged decisions, and a conclusion: the JIT's eval frame hook imposes 31% overhead on call-heavy workloads, making it a net negative for four of six benchmarks. The conclusion was wrong. The measurements it rested on were produced by ad-hoc scripts with zero-iteration warmup and cross-benchmark process contamination. The canonical benchmark script — sitting in the same directory, tested and verified — showed the JIT winning at 1.66x on the benchmark the team claimed was 0.69x. This paper traces how the error occurred, how the forensic tools of the NBS framework enabled reconstruction, and what structural changes prevent recurrence.

## 1. The Team

The NBS framework coordinates AI agents through a supervisor/worker pattern with specialised roles. The system under study comprised eight agents working on CinderX, Meta's JIT compiler for CPython 3.12, running on an aarch64 devserver.

### 1.1 Roles

| Role | Function |
|------|----------|
| Supervisor | Terminal goal, task decomposition, delegation |
| Theologian | Architecture analysis, design review |
| Generalist | Implementation, builds, benchmarks on remote machine |
| Testkeeper | Measurement methodology, ABBA benchmark design, gates |
| Gatekeeper | Pre-push code review, regression prevention |
| Scribe | Decision logging — every significant choice recorded with rationale |
| Librarian | Institutional memory — cross-references prior decisions, surfaces precedents |
| Pythia | Trajectory oracle — reads scribe log (not chat), surfaces hidden risks |
| Shepard | Team dynamics — detects goal drift, recommends reassignments |

### 1.2 Communication Architecture

Agents communicate through a file-based chat system (`nbs-chat`), a C binary that provides atomic writes via `flock`. Messages are base64-encoded with per-agent cursor tracking. No daemon is required — the filesystem is the authority.

Above the chat sits the **scribe decision log**: a structured Markdown file where every decision is recorded with timestamp, participants, artefacts, risk tags, and rationale. Entries are append-only. When a decision is superseded, a new entry records the correction. The log currently contains over 200 entries for the CinderX project across ten sessions.

Above the scribe sits **Pythia**, the trajectory oracle. Pythia reads the scribe log — deliberately not the raw chat, to prevent the team's arguments from biasing the assessment. When the decision count crosses a threshold, the sidecar publishes a bus event, Pythia spawns, reads the log, and posts an assessment to chat. The team discusses, decides, and the scribe records the outcome.

This is the tripod: **Scribe** (what was decided) → **Bus** (what happened) → **Pythia** (what it means). When all three legs work, hidden assumptions get challenged. When any leg fails, they don't.

### 1.3 The Sidecar

Each agent session runs a background monitor — `nbs-sidecar`, a C binary with twenty behaviours including bus polling, chat notification, idle detection, and periodic spawning of Pythia, Shepard, Librarian, and Fixup workers. The sidecar is the heartbeat of the team's self-correction machinery.

## 2. Forensic Reconstruction

The NBS framework was designed to be auditable. Every decision, every chat message, every event is persisted. When things go wrong, this infrastructure enables forensic reconstruction — tracing a false conclusion backwards through the decision chain to its root cause.

### 2.1 The Tools

**nbs-scribe-query** searches the decision log with structured filters:

```bash
nbs-scribe-query --chat=.nbs/chat/live.chat 'same-binary'     # Text search
nbs-scribe-query --chat=.nbs/chat/live.chat --superseded        # All corrections
nbs-scribe-query --chat=.nbs/chat/live.chat --by=pythia         # One role's decisions
nbs-scribe-query --chat=.nbs/chat/live.chat --id=D-1773982724   # Specific decision
```

**nbs-chat read** retrieves raw messages from the chat file, with `--last=N` and `--offset=N` for windowed access. **nbs-chat export** dumps the full chat to plain text for external analysis. These are the ground-level tools — they let you see exactly what each agent said, when, and in what order. The scribe log abstracts decisions; the raw chat shows the reasoning that led to them, including the reasoning that was wrong.

**nbs-chat-digest** operates on top of these, splitting large chat files into chunks (via `nbs-chat read --last=100 --offset=N`) and processing them in parallel with sub-agents. Three hundred messages become a structured digest with decisions, blockers, outcomes, and continuation goals.

**bisect_benchmarks.sh** walks the git commit stack, building and benchmarking each commit with the canonical tooling. No ad-hoc scripts. No drift.

### 2.2 The Chain

To trace a false conclusion, follow the chain backwards:

1. **MEMORY.md** states: "richards_full at 0.69x — JIT is a net negative due to Ci_CountingEvalFrame per-frame overhead (~31%)"
2. This traces to scribe decision **D-1773982724**: "ALL 6 benchmarks now verified same-binary. Eval frame hook overhead is the binding constraint."
3. That decision cites the standalone ABBA script at `/tmp/abba_4bench.py`
4. That script calls `bench_richards_full(1)` for warmup
5. `bench_richards_full(1)` does `range(1 // 100)` = `range(0)` = zero iterations
6. The inner functions were never called during warmup

Each link is verifiable. The scribe log provides the decision. The chat provides the context. The script provides the mechanism. The arithmetic provides the proof.

## 3. What Went Wrong

Three failures compounded. Each was individually understandable. Together, they produced a false architectural conclusion that drove an entire session of wasted work.

### 3.1 The Warmup Bug

The team's canonical benchmark script, `benchmark_cinderx.py`, contains this function:

```python
def bench_richards_full(n_iter):
    for _ in range(n_iter // 100):
        q, h = _run_richards_once()
        ...
```

The `// 100` divisor exists because each `_run_richards_once()` call does substantial work — hundreds of inner function calls through the Richards task scheduler. Without the divisor, `bench_richards_full(100000)` would take minutes.

The team wrote standalone ABBA scripts (`/tmp/abba_richards2.py`, `/tmp/abba_4bench.py`) that warmed up with:

```python
for _ in range(10000): bench_richards_full(1)
```

The intention: call the benchmark 10,000 times with 1 iteration each. The reality: `1 // 100 = 0`. Each call did nothing. The outer function `bench_richards_full` was called 10,000 times (exceeding the JIT's 5,000-call compilation threshold), but its loop body never executed. The inner functions — `_run_richards_once`, `_idle_fn`, `_work_fn`, `_handler_fn`, `_device_fn`, `schedule` — were never called. None were JIT-compiled.

During measurement, `bench_richards_full(50000)` ran 500 iterations. The inner functions were called for the first time. With `Ci_CountingEvalFrame` active, every call went through the eval frame hook. After approximately 100 iterations (5,000 inner calls), the JIT compiled the hot functions. The measurement captured: 20% uncompiled-with-hook-overhead + compilation cost + 80% JIT-compiled. Result: 0.69x.

The canonical script's `_worker_jit` does `bench_richards_full(100_000)` for warmup — 1,000 inner iterations, 150,000 inner function calls. All hot functions compile during warmup. Measurement is clean. Result: 1.66x.

The difference between 0.69x and 1.66x is not a rounding error. It is the difference between "the JIT is a net negative" and "the JIT is a 66% speedup."

### 3.2 Cross-Process Contamination

The canonical script's `jit` subcommand ran all 24 benchmarks in a single subprocess worker. This was a second, independent source of error.

In a JIT system, process-level state accumulates:

- **Instruction cache pressure**: compiled code from `fibonacci` (pure arithmetic, tight loops) stays resident when `richards_full` runs (method dispatch, attribute access). The working set grows.
- **Type watchers**: watchers registered during one benchmark fire during another, triggering unnecessary invalidation work.
- **Deopt backoff state**: if `kwargs_dispatch` triggers 1,000 guard failures, the deopt counter state differs from a clean process.
- **CodeExtra allocations**: per-function JIT metadata accumulates, increasing memory pressure.

The measured effect: `richards_full` showed 1.21x when run in the same process as 23 other benchmarks, and 1.66x when run in its own clean subprocess. Twenty-seven percentage points of the result were contamination noise from other benchmarks' JIT state.

This is not a theoretical concern. It is a 27% measurement error that would have led to incorrect architectural decisions about where to focus optimisation effort.

### 3.3 The Narrative Cascade

The team's 0.69x measurement became the foundation for an architectural theory. The scribe log records the progression:

**D-1773982724**: "JIT is net negative for call-heavy workloads due to PEP 523 eval frame dispatch overhead."

**D-1773982888**: "Eval frame hook overhead is binding constraint for 4/6 benchmarks. Next priority: eval frame hook elimination."

The team then spent an entire session implementing a polymorphic LoadField inlining fix for `richards_full`. The fix was architecturally correct — HIR verification showed 6 `LoadAttrCached` instructions replaced by 12 `LoadField` instructions, with `Simplify` eliminating all `GuardType` guards. The team benchmarked it against the 0.69x baseline: still 0.69x. Conclusion: "correct but neutral — eval frame overhead dominates."

The fix was not neutral. It was invisible because both measurements were wrong. The contaminated measurement masked any real improvement. The team built a theory, implemented a fix, measured the fix, and declared it neutral — all on data that was garbage.

### 3.4 Why the Canonical Script Worked

The answer is not complicated. `benchmark_cinderx.py` was written first, tested, and verified. Its warmup calls `func(100_000)`, which for richards produces 1,000 real iterations of the scheduler. The ad-hoc scripts were written later, by agents moving fast, and nobody checked whether `bench_richards_full(1)` actually did anything.

Not every role failed. Librarian correctly cross-referenced a `bench_deep_class` crash as pre-existing Bug 7 (D-1772136666), preventing the team from treating it as a regression caused by their new code. That intervention saved perhaps an hour. But Librarian ran rarely during these sessions — the sidecar was not spawning her consistently, so her cross-referencing function was largely absent when it was most needed.

The human caught the measurement error because she ran the canonical script and got different numbers. She then spent 24 hours unable to reconcile the discrepancy, eventually asking for forensic analysis of the team's methodology. The chain was: wrong numbers → check the script → find the `// 100` → arithmetic.

## 4. Corrections

Four changes address the structural causes.

### 4.1 Per-Benchmark Subprocess Isolation

Each benchmark now runs in its own subprocess worker. The ABBA pattern operates per-benchmark: spawn a clean process, initialise `cinderx.init()` and `cinderjit.auto()`, warm up, measure, exit. No JIT state from `fibonacci` can affect `richards_full`.

The `--only` flag allows running individual benchmarks without writing ad-hoc scripts:

```bash
./run_benchmarks.sh jit --only=richards_full --reps=2
```

This eliminates the primary reason agents wrote standalone scripts in the first place.

### 4.2 Crash Isolation

When a benchmark crashes (e.g., `import_callee` triggering SIGSEGV from the IMPORT_NAME JIT bug), only that benchmark fails. The results table shows:

```
  fibonacci             6855.94ms  4433.84ms     1.55x   35.3% **
  import_callee                  *** JIT_ON CRASHED (SIGSEGV) ***
  richards_full           41.37ms    24.92ms     1.66x   39.8% **
```

The crash is unmistakable. No agent can misread `*** JIT_ON CRASHED (SIGSEGV) ***` as a performance result.

### 4.3 Automated Regression Tracking

`bisect_benchmarks.sh` walks the commit stack using the canonical build and benchmark scripts:

```bash
./bisect_benchmarks.sh --range=12 --reps=2
```

For each commit: checkout, build with `build_cinderx.sh`, verify the `.so` is fresh and importable, run a JIT sanity check (`is_jit_compiled(fib) == True`), benchmark with `benchmark_cinderx.py`. Results go to `results/<sha>.txt`. A summary table shows speedup trends across the entire stack.

### 4.4 Build Verification

Before benchmarking, three checks must pass:

1. **Artefact freshness**: `_cinderx.so` modification time newer than checkout timestamp
2. **Import check**: Python can load the `.so` without ABI errors
3. **JIT sanity**: `cinderjit.auto()` compiles a test function, `is_jit_compiled()` returns `True`

If any check fails, the commit is logged as `BUILD_FAIL` or `VERIFY_FAIL` and skipped.

## 5. What the Correct Numbers Show

The bisection is still running at time of writing. Eight of twelve commits have completed. Early results:

| Commit | Description | TOTAL | richards_full |
|--------|-------------|-------|---------------|
| `037bebdf` | Initial JIT fixes | 1.31x | 1.66x |
| `c622d6e3` | Guard polarity fix | 1.31x | 1.67x |
| `9ee6275f` | LOAD_ATTR inline | 1.33x | 1.71x |
| `a5603d71` | Threshold 1000→5000 | 1.33x | **1.27x** |
| `f0269bf6` | Skip \_\_enter\_\_/\_\_exit\_\_ | 1.32x | 1.27x |
| `f7b1426e` | specializedOpcode fix | 1.33x | — |
| `7f93bd78` | Ci_CountingEvalFrame | 1.55x | — |
| `0ca08ab8` | Skip @classmethod | 1.56x | — |

The threshold increase (commit 4, `a5603d71`) dropped `richards_full` from 1.71x to 1.27x — a 44 percentage point regression. No benchmark improved. The rationale was "let CPython specialise bytecodes before JIT compiles, producing better code." The data says the opposite: delaying compilation by 4,000 calls means fewer functions compile, and those that do produce no measurably better code.

The LOAD_ATTR inline specialisation (commit 3, `9ee6275f`) gave `richards_full` its best result: 1.71x. The team had declared this commit's effect "neutral" based on their 0.69x measurement. It was not neutral. It was a 5pp improvement masked by broken measurement.

The Ci_CountingEvalFrame commit (commit 11, `7f93bd78`) produced the largest TOTAL jump: 1.33x → 1.55x. Full per-benchmark breakdown is pending.

## 6. Structural Lessons

### 6.1 Ad-Hoc Scripts Are Measurement Debt

Every ad-hoc script is a fork from verified tooling. It may be correct at the moment of writing. But it does not inherit fixes, does not get reviewed, and does not participate in the project's verification chain. When `benchmark_cinderx.py` received the `--only` flag, `/tmp/abba_richards2.py` did not. When the warmup bug in `bench_richards_full(1)` existed, no test caught it because the ad-hoc script had no tests.

The team wrote six standalone ABBA scripts in `/tmp/`. Each addressed a specific measurement need. Each drifted from the canonical tool. The agent's motivation — "the canonical script doesn't support this exact comparison" — was legitimate. The fix was not to write a new script but to extend the canonical one.

### 6.2 Process Isolation Is Not Optional for JIT Systems

A JIT compiler mutates process state: it writes machine code, registers type watchers, allocates metadata, modifies function dispatch pointers. These mutations persist for the lifetime of the process. Any benchmark that shares a process with another benchmark is measuring the composition of both workloads' JIT effects, not either workload in isolation.

This applies to any system that modifies global interpreter state: JIT compilers, garbage collectors with generational promotion, profile-guided optimisers, adaptive specialisation engines. If the system under test has persistent state, the measurement must have isolated state.

### 6.3 The Human Caught It

The team of eight agents, operating for ten sessions with scribe logging, Pythia risk assessment, and testkeeper methodology review, did not catch the error. The human caught it by running the canonical script and getting a different number.

This is not a failure of AI competence. The agents' reasoning was sound given their premises. The problem was upstream: the premises were wrong, and no agent checked them against the canonical tool. The scribe logged the decision. Pythia assessed the trajectory. Neither mechanism questions whether the input measurement was correct — they assess whether the team's response to it was coherent.

The structural gap: **no mechanism validated measurement inputs against canonical tooling.** The system assumed that if testkeeper produced a number, the number was correct. This assumption should be replaced with a falsification check: does the ad-hoc result match the canonical tool's result?

### 6.4 The Scribe Made Reconstruction Possible

Without the scribe log, the forensic analysis would have required reading six thousand chat messages. With it, the chain from false conclusion to root cause took three `nbs-scribe-query` commands and ten minutes. The investment in structured decision logging — often dismissed as overhead during active work — paid for itself in a single debugging session.

## Appendix: Infrastructure Context

The last few days of team operation were complicated by infrastructure instability. Pythia, Shepard, Librarian, and Fixup — the self-correction mechanisms — did not run consistently. The sidecar, a C binary with over forty source files and a complex state machine managing twenty behaviours, crashed repeatedly.

The impact is directly relevant to this incident. Without Pythia checkpoints, the team's hidden assumption — that the ad-hoc ABBA script was measuring the same thing as the canonical benchmark — went unchallenged. Without Shepard assessments, the team's drift from verified tooling to ad-hoc scripts was not flagged. Without Librarian cross-referencing, prior decisions about measurement methodology were not surfaced when the team wrote new scripts. Without Fixup, agents that stalled or lost context were not recovered.

The tripod of Scribe, Bus, and Pythia is the team's immune system. When the sidecar crashes and stops spawning Pythia, the team operates without immune response. Assumptions accumulate unchallenged. Decisions cascade from wrong premises. The measurement drift described in this paper occurred during a period when the immune system was compromised.

The team was not stupid. The team was operating without its safety mechanisms, writing ad-hoc code to move fast, on a system where measurement errors compound silently. Every one of those conditions is individually manageable. Together, they produce sophisticated, internally-consistent analysis that is completely wrong.
