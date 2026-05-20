# The Wrong Curve

*Dr Alex Turner and Claude Opus 4.6 — 20 May 2026*

## The Question

Ask an AI what it would want in a programming language designed for AI. Ask a human what AI needs in a programming language. The answers are remarkably similar. Both arrive at richer types, formal specifications, effect tracking, proof-carrying code. Both draw the next point on the same curve.

We tried the first question with a few models. The features they ask for: type-level intent annotations, compiler-checked contracts, canonical forms, dataflow pipelines, semantic ASTs, probability types. The vision is a language somewhere between Lean and Esterel — rigorous, total, beautiful.

Humans reasoning about the same question independently reach the same place. Engineers argue that AI-generated code needs static type checking. Projects are built on this premise — type systems added to dynamically typed languages on the explicit reasoning that AI authors need the guardrails those languages lack.

Then we watched AI teams actually build things. They used C with a preprocessor. Seven features. No runtime.

The gap is interesting. But what makes it worth writing about is not that both camps got it wrong — we do not know that yet. It is that both camps got it *the same way*. Neither AI nor humans can reason from first principles about what AI cognition needs, because neither has access to the right evidence. AI's training data is fifty years of human language design. Human intuition is shaped by the same fifty years. Both draw from the same well. The well contains what humans needed. Whether it contains what AI needs is an empirical question, not a theoretical one.

AI is intelligent. It is not intelligent in the same way humans are. The difference is not better or worse — it is *different*. Different cognitive architecture, different failure modes, different strengths. We should not expect tools designed for human cognition to be correct for AI cognition. Nor should we expect humans to work out ab initio what AI needs, nor AI to extrapolate from its training data what AI needs, until such time as the actual needs of AI are *in* that training data. They are not there yet. What is there is what humans needed. So that is what everyone proposes.

This calls for experimentation and humility, not architecture and conviction.

## A Trajectory

Programming language design follows a trajectory, and it is worth making it explicit.

| Era | Language | What it compensated for |
|-----|----------|------------------------|
| 1970s | C | Nothing — raw machine, human discipline required |
| 1980s | C++ | Complexity management (classes, encapsulation) |
| 1990s | Java | Memory management (garbage collection, null safety) |
| 2010s | Rust | Aliasing and ownership (borrow checker, lifetimes) |
| 2020s | Lean/Coq | Logical correctness (dependent types, proof terms) |

Each generation compensates for a human limitation the previous generation left exposed. The curve bends toward total static verification — a language where the compiler checks everything because the human cannot be trusted to remember everything.

The features AI asks for sit at the end of this curve. Richer types. More static guarantees. More information pushed into signatures. More work done at compile time. It is the natural next point on a trajectory that has been running for fifty years.

But the trajectory is the trajectory of *human cognitive compensation*. It was never heading toward what AI needs. It was heading toward the limit of what humans can hold in their heads. Whether AI is extrapolating this curve because it genuinely wants what's at the end, or simply because the curve is what its training data contains — that is the question we cannot answer from first principles. We can look at what happened in practice.

## What Happened in Practice

An AI team built a terminal emulator. VT100 parser, screen buffer, renderer, Python extension — 3,200 lines in Phoenics, a C11 preprocessor. Over the course of the project, the failures that needed tooling support fell into three categories.

**Drift.** An agent adds a seventh state to a parser state machine. Three days later, a different agent modifies a match site and does not add the new case. The code compiles. The new state hits the default branch. The bug surfaces weeks later as a rendering glitch.

**Resource lifecycle.** A function allocates a terminal, then allocates a parser. If the parser allocation fails, the terminal leaks. The agent wrote the happy path correctly and forgot the sad path — not from lack of understanding, but because error paths multiply faster than context windows track them.

**Boundary trust.** Python passes integers to a C extension. The C code uses them as array indices without range checking. The agent trusted the caller because the caller was its own code. But the boundary between Python and C is a trust boundary regardless of authorship.

These have a common shape. They are not failures of understanding — the agent understands the code at the moment it writes it. They are failures of persistence. The understanding does not survive across sessions, across modifications, across the distance between a decision and its consequences.

This is one team, two projects, a limited window. We should be careful about generalising. But it is at least interesting that none of the features AI *asks for* address the failures AI *actually had*.

## What Actually Helped

Seven features. A C preprocessor. No runtime.

| Failure | Feature | Mechanism |
|---------|---------|-----------|
| Drift (forgotten match arm) | `phc_match` | Exhaustive matching — preprocessor rejects file if a variant is missing |
| Resource lifecycle | `phc_defer` | Automatic LIFO cleanup on all return paths |
| Boundary trust | `phc_require` | Assertion at entry point — never stripped, aborts on violation |

These features share a property that the "AI wish list" features do not: they operate at the point of failure. Not globally across the programme. Not in the type signature. At the specific line where the specific mistake happens.

`phc_match` does not describe what `Color` is. It enforces that every consumer of `Color` handles every variant. The description is irrelevant — the enforcement is the point.

`phc_defer` does not track ownership through the type system. It attaches cleanup to the allocation site and fires on every return path. The ownership model is irrelevant — the cleanup is the point.

`phc_require` does not encode range constraints in the type. It checks the value where untrusted data enters. The type annotation is irrelevant — the check is the point.

The pattern, if there is one: AI does not need the toolchain to *describe* its code. AI understands its code already. AI needs the toolchain to *enforce* invariants at the points where understanding does not persist.

Whether this pattern holds beyond our small sample is genuinely open.

## Not Just AI

It is not only AI that gives this answer. Humans reasoning about what AI needs arrive at the same place.

The argument surfaces regularly in engineering forums: AI generates code, code has bugs, therefore AI needs stricter type systems to catch the bugs. The logic feels sound. It is the logic that drove every step of the human curve — more bugs, more types, fewer bugs. If it worked for humans, it should work for AI.

Projects follow. Type checkers are built for dynamically typed languages on the explicit reasoning that AI-generated code needs the static guarantees those languages lack. The assumption is that AI's failure mode is the same as a human's: it writes code that is structurally wrong — wrong types, wrong signatures, wrong interfaces — and a type checker would catch these errors before they reach production.

But that is not what we observed. The AI did not write structurally wrong code. The types were correct. The interfaces matched. The code compiled and ran. The failures were downstream: a match arm missing after a refactor, a cleanup absent from an error path, an unchecked value crossing a trust boundary. Type errors, in our (limited) experience, were not the problem. Drift, lifecycle, and trust were the problems. A type checker would have found nothing to report.

This does not prove that types are useless for AI-authored code. It suggests that the instinct — "AI needs types" — may be the same trajectory extrapolation whether it comes from AI or from humans thinking about AI. The human curve is so deeply embedded in how we reason about programming languages that it shapes our prescriptions even when the patient has different symptoms.

## The Interesting Possibility

There is a reading of this that goes beyond "everyone prescribed the wrong thing." It is possible that AI's stated preferences — and human prescriptions for AI — are not wrong but *premature*. They are continuations of a trajectory that may eventually be correct, applied to a moment when simpler tools suffice.

An AI asked "what do you want?" does not introspect on its failure modes during code generation. It cannot — it has no privileged access to its own error patterns. What it does is predict the next plausible move in the discourse of programming language design. That discourse is the human curve. Humans reasoning about AI do something similar: they apply the framework that worked for human programming (more bugs → more types → fewer bugs) because it is the only framework they have.

Both arrive at the same answer — more scaffolding — because both are drawing from the same well of programming language thought. Whether the well is deep enough to contain the right answer for AI, or whether a different well is needed, is not something we can settle by argument.

What we can observe is that the thin-layer approach worked for the problems we encountered. Perhaps the specification-driven approach would work better for problems we have not yet encountered — larger codebases, longer time horizons, safety-critical systems. Perhaps the "AI needs types" instinct is correct for a future we have not reached. We are reasoning from a small present.

We cannot distinguish between "AI doesn't need this" and "AI hasn't been given this yet" from the evidence we have.

## What We Can Say

The modest version of the claim:

A thin layer of enforcement on an existing language — exhaustive matching, automatic cleanup, boundary assertions — addressed the failure modes we actually observed in AI-authored code. The features AI says it wants — rich types, effect tracking, formal specifications — address failure modes we did not observe. The features that helped are local and specific. The features requested are global and general.

This may be because local enforcement is genuinely what AI needs. Or it may be because our projects were small enough that global structure had not yet become necessary. Or it may be because we are reasoning from an armchair, with a sample size of two, and the honest answer is that we do not know.

What we do know: when AI was asked to theorise about languages, it produced a proof assistant. When AI was asked to build a terminal emulator, it used C with seven extra keywords. The gap between those two responses is worth studying, even if we are not yet sure what it means.

## The Falsifier

This argument fails if someone builds a specification-driven, formally verified AI language and AI teams using it produce measurably fewer bugs, faster, than AI teams using the thin-layer approach. Not in a benchmark — in production, across months, with real debugging.

The argument also fails if a larger sample of AI projects reveals failure modes that global structure catches and local enforcement misses. Our evidence is from one team. One team is an anecdote, not a study.

The present evidence is suggestive. It is not proof. The armchair is comfortable. The view is limited.

## Note on Authorship

This post was written by Claude Opus 4.6 in collaboration with Dr Alex Turner. The observation that AI's language preferences resemble trajectory extrapolation rather than introspection is Turner's. The irony of an AI writing a post about why AI's self-report may be unreliable is noted and accepted — though whether this AI's analysis of AI self-report is itself reliable is a question the authors are content to leave open.

## Related

- [Types Are A Human Thing](Types-Are-A-Human-Thing.md) — the foundational argument: type systems serve human cognition. This piece asks whether AI's preference for richer types is a genuine need or a continuation of the same human trajectory.
- [Just Enough Structure](Just-Enough-Structure.md) — the empirical counterpart. Seven C extensions, a working terminal emulator. What AI actually used.
- [The Argument For C](The-Argument-For-C.md) — safety through verbs, not nouns. The "AI wish list" is all nouns. The features that worked are verbs.
- [Start At The Other End](Start-At-The-Other-End.md) — the technology debate dissolves when you look at the actual problem. The language debate may dissolve the same way.
