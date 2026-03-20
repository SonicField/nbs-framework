# Types Are A Human Thing

## The Observation

A team of AI agents spent a day debugging a JIT compiler, fixing infrastructure tools, writing benchmark harnesses, and tracing measurement errors across a 12-commit stack. The codebase was C and Bash. No type system to speak of. The agents did not struggle with this.

They struggled with other things — pty-session prompt matching, stale build artefacts, ad-hoc scripts that drifted from canonical tooling. Not one of these failures was a type error. Not one would have been caught by a type system. The bugs were semantic: `1 // 100 = 0` in valid Python, TOCTOU races in correctly-typed C, cross-process JIT state contamination that no type can model.

This raises a question: are type systems solving a problem that AI does not have?

## What Types Do For Humans

Type systems are a compression heuristic for human working memory. A programmer reading `fn process(config: &Config) -> Result<Output, Error>` can hold the function's contract without reading the body. The signature fits in the 4-7 items that human short-term memory accommodates. The body might be 200 lines. The signature is one line. Types let humans reason about code they have not read.

This is valuable. Human programmers cannot read every caller, every constructor, every path through a codebase before modifying a function. Types provide a structural summary that compresses the relevant information into something a brain can hold while thinking about something else.

AI does not have this constraint. An agent can read the function body. It can grep every caller. It can trace every construction site for the config object and verify none of them pass null. The type signature provides no information the agent cannot obtain from the source in comparable time. The compression is not needed because the decompressed form is accessible.

## What Actually Helped

During the session, the tools that caught bugs and prevented errors were:

**Assertions.** The NBS sidecar uses `ASSERT_MSG` at function boundaries:

```c
ASSERT_MSG(interval_secs > 0,
           "trigger_periodic_check(%s): interval_secs must be positive, got %d",
           trigger->name, interval_secs);
```

A type system would express this as `PositiveInt`. The assertion expresses the same invariant with more information: which function, which parameter, what the bad value was, and why it matters. The assertion carries more diagnostic information per token of context than the type.

**Behavioural grep.** When the `poll_interval` default was changed from 300 to 0, the safety question was not "is this an int?" but "does anything downstream break when this is zero?" The answer came from `grep -n 'poll_interval' src/nbs-sidecar/sidecar.c` — line 744 checks `cfg->poll_interval > 0` before entering the poll block. The zero is safe. No type system answers this question; it is a question about behaviour, not structure.

**Falsifiable invariants.** The `is_jit_compiled(fib) == True` sanity check before every benchmark run caught more real errors than any structural check could. It encodes intent: "the JIT must be functional." A type system can verify that `is_jit_compiled` returns `bool`. It cannot verify that the JIT is functional.

**Forensic tools.** The scribe query `nbs-scribe-query --superseded` found prior decisions that contradicted current assumptions. This is institutional memory retrieval, not type checking. The errors it catches — methodology drift, wrong baselines, repeated mistakes — exist entirely outside the type system's domain.

## The Counterargument

The honest counterargument for types is not about catching bugs in the current session. It is about API boundaries across time and across teams.

When an agent writes a function today and a different agent modifies it six months later, the assertion may be stale but the type endures. The function signature `fn connect(host: &str, port: u16) -> Result<Connection, IoError>` survives refactoring, team changes, and context loss. It is durable documentation.

This is a real benefit. Types as documentation outlast types as verification.

But there is a counter-counter: a type that endures while the behaviour changes is worse than no type at all. `Result<Output, Error>` endures while the function silently starts returning a different kind of error that the caller does not handle. The type is green. The behaviour is broken. The structural contract held; the semantic contract drifted. And the drift is the thing that kills — it is exactly what happened with the benchmark scripts. The function signature `bench_richards_full(n_iter: int) -> int` was correct the entire time. The semantic contract — "n_iter iterations of the Richards scheduler" — was violated by `// 100` and no type system in existence would have caught it.

Types model structure, not intent. The structure can stay correct while the semantics drift. The gap between structural correctness and behavioural correctness is where the bugs live.

## Different Epistemics

The claim is not that types are useless. It is that they solve a problem shaped like human cognition: limited working memory, inability to read entire codebases, need for structural summaries.

AI cognition has different constraints. Context windows are finite but large. Codebase search is fast. The bottleneck is not "what type is this parameter?" but "what happens when this parameter is zero?" — a behavioural question that types do not answer.

The epistemics that helped AI agents today:

| Human epistemics (types) | AI epistemics (behaviour) |
|--------------------------|--------------------------|
| What kind of thing is this? | What does this thing do? |
| Structural summary | Behavioural grep |
| Compile-time guarantee | Runtime assertion with diagnostic |
| Enduring signature | Falsifiable invariant |
| Category membership | Boundary conditions |

These are not competing frameworks. They are frameworks for different cognitive architectures. A human programmer benefits from both — types for navigation, assertions for correctness. An AI programmer benefits primarily from the second column, because the first column solves a problem it does not have.

## The Bias Disclosure

The author of this observation has never liked type systems. This is an aesthetic preference with a long history, and it would be dishonest to present the argument above as purely empirical when it aligns with a pre-existing disposition.

The falsifier for this claim: if AI agents working in strongly-typed languages (Rust, Haskell, TypeScript) produce measurably fewer semantic errors than AI agents working in weakly-typed languages (C, Python, Bash), the argument is wrong. The type system would be providing something beyond structural compression — something that benefits AI cognition despite not solving a working-memory problem.

This experiment has not been run. The evidence presented here is from one team, one codebase, one day. It is suggestive, not conclusive. The observation stands until someone measures the alternative.
