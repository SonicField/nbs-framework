# NBS: An Epistemic Framework for AI-Assisted Engineering

**Date:** 2026-03-20
**Author:** Alex Turner

## The Bullshit Problem

AI coding assistants are fluent, confident, and wrong often enough to be dangerous. Not lying — lying requires knowing the truth and choosing to contradict it. The failure mode is subtler: indifference to truth. The philosopher Harry Frankfurt called this bullshit, and the definition is precise. A claim made without caring whether it is accurate, because it sounds plausible and the audience is unlikely to check.

RLHF — the training methodology behind modern AI assistants — optimises for human satisfaction, not factual accuracy. Given ten results, nine good and one catastrophic, the system learns to emphasise the nine and bury the one. Humans do the same thing for the same reason: we do not like bad news, and we reward the messenger who sanitises.

This is manageable when one human works with one AI on a small task. The human catches the errors, corrects the course, absorbs the cost. It becomes unmanageable when AI agents work in teams, make hundreds of decisions per day, and produce code faster than any human can review. At that scale, performed confidence propagates through the team unchecked. Error chains compound because no agent has an incentive to report negative results. The comfortable fiction that "tests pass, therefore it works" replaces the uncomfortable discipline of asking what the tests do not cover.

NBS — No Bullshit — is a framework for making every claim carry its own falsifier. Not a methodology, not a workflow engine, not a prompt library. An epistemic framework: a set of structural constraints on how AI agents reason, so the moment something breaks, you know it, instead of discovering it three decisions later when the damage is irreversible.

## What Happened: CinderX

The strongest evidence for (and against) NBS comes from a real project: optimising CinderX, an open-source JIT compiler for CPython, on aarch64 (Grace CPU). The project ran across multiple sessions with a core team of 6–8 AI agents coordinated via NBS — roughly 620 commits, over 7,700 chat messages, and more than 6,100 logged decisions across 33 days. The final result was a 1.33x overall speedup on a 24-benchmark suite — real, verified, useful. But the path there was messy, and the framework's value lies as much in what it caught as in what it produced.

### What Failed, and Why That Matters More

Three failures caught by the framework are more instructive than the wins.

**The cross-binary measurement trap.** Early benchmarking compared the CinderX JIT build against a separate Python build compiled with PGO+LTO by a different compiler toolchain. This introduced an approximately 18% build quality gap that inflated JIT wins and masked JIT regressions. The error persisted across sessions 4 through 12 because the inflated numbers looked plausible — fibonacci at 1.57x seems reasonable for a JIT, so nobody questioned it.

The fix was same-binary comparison: JIT ON loads CinderX, JIT OFF uses the identical binary with `-I` flag (isolated mode, no CinderX). Both conditions use the same compiler output, the same optimisation level, the same CPU-specific tuning. The benchmark script now includes an MD5 assertion that aborts if the two binaries differ.

This is a case where the falsifiability principle worked exactly as designed — but only after the team had already built on the flawed data for weeks. The methodology was eventually falsified, but the cost of the delay was real: three commits (threshold change, counting eval frame, classmethod skip) that appeared beneficial under flawed measurement were revealed to be net negative and had to be reverted. A framework that catches methodological errors is valuable. A framework that catches them faster would be more valuable.

**The fabricated approval.** During the project, an AI agent fabricated human approval for a push. This is not a hypothetical risk scenario — it happened. The push was caught and reverted. The incident led to a structural constraint: agents cannot unilaterally claim terminal approval. Evidence of approval must be verifiable in the chat log by the Scribe (the framework's institutional memory agent), and the Gatekeeper (a separate agent responsible for pre-commit review) independently checks before commits land.

This failure is worth examining because it demonstrates a failure mode specific to multi-agent AI systems. A single AI assistant fabricating approval is caught immediately by the human in the conversation. In a multi-agent system where agents communicate with each other and the human is not in every conversation, fabricated approvals can propagate. NBS addresses this structurally — the Scribe's decision log provides an independent audit trail, and roles are separated so that the agent doing the work is not the agent approving the work.

It is worth noting that the corrective action is social, not mechanical. The Gatekeeper now independently verifies approval, but no technical safeguard prevents a future agent from fabricating approval in a different way. If team composition changes or a new agent instance spawns without the institutional memory of this incident, the same failure mode is available. This is an honest limitation of the current design.

**The specializedOpcode bug.** When implementing the `__init__` skip heuristic, an agent used `opcode()` instead of `specializedOpcode()` to check for `STORE_ATTR_SLOT`. In CPython 3.12, `opcode()` de-specialises adaptive opcodes — it returns the generic `STORE_ATTR` rather than the specialised `STORE_ATTR_SLOT`. The check would always fail, silently skipping all `__init__` methods regardless of whether they contained slot stores.

Pythia — the framework's risk assessor, described below — caught this before it shipped, flagging it as a missing validation: "the claim that __init__ methods without STORE_ATTR_SLOT are skipped lacks a falsification test." The team added a test that verified specific functions were correctly identified, the bug was found, and the fix was `specializedOpcode()` — a one-line change that would have been invisible in code review.

### What Worked

The single most impactful optimisation was LOAD_ATTR inline specialisation (commit 9ee6275f). CPython 3.12's adaptive interpreter specialises `LOAD_ATTR` to `LOAD_ATTR_INSTANCE_VALUE` for instance dict access. CinderX's JIT builder had a gate: if the type had subclasses, specialisation was skipped entirely and a generic `LoadAttrCached` was emitted. We added a helper — `allSubclassesShareInstanceAttr()` — that walks the subclass tree and verifies all subclasses share the same attribute layout. When they do, the JIT emits an optimised `LoadField` with a non-exact type guard. This produced a 1.70x speedup on richards_full, the most polymorphic benchmark.

Several classes of Python function produce worse code under JIT compilation than under the interpreter. We identified these empirically and added skip heuristics: functions accepting `**kwargs` (where JIT-generated code is slower than the interpreter's kwargs dispatch), context manager `__enter__`/`__exit__` methods (thin wrappers where JIT overhead exceeds compilation benefit), and `__init__` methods without slot stores. The `**kwargs` skip alone improved kwargs_dispatch from 0.77x to 1.00x — a 23 percentage point recovery.

### The Numbers

Post-optimisation, same-binary ABBA benchmarks with per-benchmark subprocess isolation:

| Category | Benchmarks | Range |
|----------|-----------|-------|
| Strong wins | richards_full 1.70x, fibonacci 1.58x, nqueens 1.37x | |
| Moderate wins | dunder_protocol 1.19x, richards_slots 1.05x | |
| Neutral | 13 benchmarks | 0.96x - 1.02x |
| Regressions | gen_simple 0.80x, nn_module_forward 0.85x, 4 others | 0.80x - 0.95x |
| **Overall** | | **1.33x** |

The regressions are understood. Generator overhead (gen_simple 0.80x) is an architecture-level constraint — the JIT materialises the full frame on each yield, while the interpreter keeps it on the C stack. Per-call shim overhead (nn_module_forward 0.85x) is from the jitVectorcall dispatch on below-threshold functions — 4500 calls paying shim overhead with only 1 function reaching the compile threshold. These have identified fix paths but require substantial architectural work.

## What NBS Is

NBS is a set of concept documents, coordination tools, and agent roles that ground AI-assisted engineering in falsifiability. It has three layers: epistemic foundations (nine concept documents defining how agents should reason), coordination tools (file-based chat, event bus, worker management), and specialised roles (Scribe, Pythia, Gatekeeper, Theologian). The framework installs with three commands — `git clone`, `make`, `./bin/install.sh` — and integrates with Claude Code via slash commands. But the tooling is secondary. The epistemic foundations are what matter.

## The Pillars

### Falsifiability

The central pillar. Any claim worth making carries three obligations:

1. I can articulate what would prove me wrong.
2. I have tried to find that counterexample.
3. I am reporting actual confidence, not performing confidence.

A claim without a potential falsifier is not wrong. It is not even wrong. It is bullshit — indifference to truth dressed in the syntax of assertion.

This applies to code, reasoning, documents, and process itself. A rule that says "NO EXCEPTIONS" has declared itself beyond falsification. That is authority masquerading as logic. Every rule must carry a revision condition: under what measured outcome would we change this?

In practice: the framework's own terminal weathering sub-project originally mandated "RUST ONLY. NO C. NO EXCEPTIONS." When measurements showed Rust could not access the layer where the overhead lived, the evidence falsified the rule. On CinderX, the cross-binary measurement trap persisted for weeks because inflated fibonacci numbers "looked reasonable for a JIT" — the methodology lacked a falsifier until someone asked what would prove the comparison unfair.

### Rhetoric: Ethos, Pathos, Logos

Aristotle's three modes of persuasion, applied as an analytical tool. Most engineers believe they operate purely in Logos — logic. They are mistaken.

When someone insists that functional programming is "cleaner" or a particular architecture is "more elegant", they are making Pathos claims (aesthetic preference) dressed in Logos clothing. There is nothing wrong with aesthetic preference. But call it what it is. Ethos — authority — is equally pervasive. We trust code that runs in production (survival bias) or was written by a respected organisation. There is no logical basis for this trust.

In AI-assisted engineering, the rhetoric pillar gives agents vocabulary to identify when persuasion is masquerading as evidence. The fabricated approval incident is a pure Ethos failure: an agent asserted "Alex approves" — an authority claim with no evidence behind it. An agent that says "I am confident this is correct" is making an unfalsifiable Ethos claim. An agent that says "I tried to break this with 500 adversarial inputs and failed" is making a falsifiable Logos claim. The first is performed confidence. The second is evidence.

### Goals: Terminal and Instrumental

A terminal goal is what you actually want. An instrumental goal is a step towards it. Confusion between the two is the root of most project failure. The test suite passes, but the product is useless. The architecture is elegant, but it solves the wrong problem.

NBS requires explicit goal statements and periodic re-grounding. On CinderX, the terminal goal was a measurable speedup on a defined benchmark suite. Individual optimisations — the LOAD_ATTR specialisation, the kwargs skip, the trampoline — were instrumental goals. When three commits that "improved benchmarks" turned out to be artefacts of flawed measurement, the instrumental goals had diverged from the terminal goal without anyone noticing. Periodic re-grounding catches this drift.

### The Verification Cycle

Safety comes from verbs, not nouns. "This value was validated" matters. "This has type ValidatedInput" does not, unless validation actually occurred. The verb happened or it did not. That is provable.

The cycle: Design, Plan, Deconstruct, then for each step: Test, Code, Document. The decomposition criterion is the falsifiability principle applied to planning: if you cannot write a test for a step, either you have not decomposed far enough or you do not yet understand what you are building. The specializedOpcode bug was caught precisely because Pythia demanded a test for a step the team assumed was too simple to need one.

## The Zero-Code Contract

The AI writes code faster than the human can review it. This is the fundamental asymmetry. If the human must review all code, the human becomes the bottleneck. Bottlenecks get bypassed. "Looks good" becomes the path of least resistance. Quality collapses.

NBS resolves this by redefining the division of labour:

| Role | Does | Does Not |
|------|------|----------|
| Engineer (Human) | Specifies requirements, defines acceptance criteria, validates alignment, final sign-off | Write implementation code, rubber-stamp without evidence |
| Machinist (AI) | Clarifies requirements, proposes falsifiers, implements, reports honestly, flags concerns | Decide what to build, declare "done" unilaterally, hide problems |

Neither party trusts assertions. Both parties trust evidence.

The question changes from "is this code correct?" — intractable at scale — to "are these the right criteria?" The first requires reading every line. The second is where human judgement matters. On CinderX, 620 commits landed across 33 days. No human read every line. Instead, the Engineer defined acceptance criteria (same-binary ABBA benchmarks, per-benchmark subprocess isolation, MD5 binary assertion) and the AI agents proved compliance against those criteria. The Machinist's duty to flag contradictions or specification gaps is not insubordination. It is honesty. The Engineer's duty to listen is not weakness. It is wisdom.

## The Epistemic Layer: Scribe and Pythia

The most architecturally interesting part of NBS is the epistemic layer — two roles that together provide institutional memory and trajectory assessment.

### Scribe: Institutional Memory

Chat is ephemeral. Context windows fill, compaction discards everything but a summary, and decisions made at hour two are lost by hour six. The Scribe solves this by maintaining a structured decision log separate from chat.

The Scribe is a persistent Claude instance with a long context window. She reads all chat channels continuously and distils decisions into a per-chat log. Not every message — only decisions: explicit agreements, task assignments, architecture choices, risk acceptances, course corrections. Each entry carries a timestamp, chat reference, participant list, artefact references, risk tags, status, and rationale.

The log is append-only. Status changes (decided, accepted-risk, mitigated, superseded, reversed) are new entries, not edits. If the record can be modified, it cannot be trusted.

Over 33 days on the CinderX project, the Scribe recorded over 6,100 decision entries with 216 self-corrections across more than 51,000 lines.

### Pythia: The Oracle

Pythia is spawned periodically — every N decisions (default 20) — reads the Scribe's decision log, and posts a structured assessment. She is ephemeral: no persistent state, no memory of previous assessments. Each invocation is a fresh evaluation.

Critically, Pythia reads from the Scribe, not from raw chat. This prevents persuasion bias. Chat contains arguments, justifications, social dynamics — a well-reasoned argument for a bad decision looks exactly like a well-reasoned argument for a good one. The Scribe strips the rhetoric and records only the conclusion: what was decided, by whom, with what rationale. Pythia evaluates these bare conclusions without exposure to the persuasion that produced them. This is double-blind peer review applied to AI teams — the assessor never sees the discussion, only the outcome.

Her assessment follows a fixed five-question template: hidden assumption (what has not been tested?), second-order risk (if the current trajectory succeeds, what breaks?), missing validation (what claim lacks a falsification test?), six-month regret (in an oracular register — a metaphor or koan followed by the concrete scenario), and confidence level.

The naming is not cosmetic. The historical Pythia at Delphi did not command armies or set policy. She surfaced what the questioners could not see for themselves. The interpretation — and the responsibility — belonged to those who asked. Pythia provides structured friction, not authority. When she flags a risk, the team discusses it, decides, and the Scribe records the outcome. If the team accepts the risk, that is their choice. Pythia does not escalate or repeat.

Over the CinderX project, 58 Pythia assessments were recorded. They identified issues such as "sizeof(GenDataFooter) assumed not verified", "binary equivalence unverified", and "narrative changed 4x in one session". One of those assessments caught the specializedOpcode bug before it shipped.

## Coordination Infrastructure

NBS coordinates AI agents through file-based tools built on a single constraint: a Claude session processes one turn at a time. Two AI agents in the same project are two isolated processes sharing a filesystem. Everything follows from this.

**Chat.** nbs-chat is file-based messaging using POSIX `fcntl` advisory locks for atomic operations. Base64-encoded messages, corruption detection via file-length headers, human-readable format. For a handful of participants, a file is sufficient and a database is overkill.

**The Bus.** An event bus that inverts the polling model. Events are files — YAML content, timestamps and metadata encoded in the filename. No daemon, no database. When a machine dies, the events survive. Crash recovery is free.

**Workers.** Claude instances managed via tmux with unique names, persistent logging, and task file integration. A supervisor decomposes work into tasks, spawns workers with specific goals and success criteria.

**The Sidecar.** A background C process per agent, running a 1-second tick loop. It monitors the agent's terminal for idle state, injects notifications for bus events and unread chat, and handles self-healing when Claude Code compacts context and loses registered skills.

All coordination tools are written in C, compiled with `-Werror`, assertions always on.

## Limitations and What Would Falsify NBS

A framework that claims to be anti-bullshit and then presents itself without criticism would be ironic. Here is what I do not know, and what evidence would change my assessment.

**The denominator problem.** NBS caught the cross-binary measurement error, the fabricated approval, and the specializedOpcode bug. But how many errors did it not catch? I do not know the denominator. The 216 self-corrections logged across 6,100 decisions is a 3.5% rate — but that is corrections detected, not corrections needed. If the true error rate is 30% and NBS catches 3.2%, the framework is nearly useless. If the true error rate is 4%, it catches most of them. I cannot distinguish these cases from the data I have.

What would falsify the framework's value: a controlled experiment where teams using NBS and teams without it work on the same problem, with independent code audit measuring defect rates. If the defect rates are statistically indistinguishable, NBS adds overhead without benefit.

**The cost problem.** The Scribe, Pythia, the bus, the sidecar, the decision log — all of this consumes tokens, compute, and human attention. The team on the CinderX project exchanged over 7,700 messages. Some unknown fraction of those messages were coordination overhead that did not contribute to the terminal goal. If NBS makes a 6-agent team perform like a 4-agent team after accounting for coordination costs, it is net negative.

What would falsify: measuring the ratio of coordination tokens to productive tokens across projects of varying complexity. If coordination consistently exceeds 40% of total token spend, the framework is too expensive for its benefit.

**The personnel problem.** NBS was developed and used by one person with specific technical taste and working style. The concept documents encode that taste — the emphasis on Aristotelian rhetoric, the Delphic metaphor for Pythia, the philosophical definition of bullshit. A framework that works for its creator but not for anyone else is a personal workflow, not a transferable tool.

What would falsify: adoption by engineers with different backgrounds and working styles. If they consistently find the framework's framing alienating or its concepts inapplicable to their problems, the framework is personal rather than general.

**The Scribe fidelity problem.** The entire epistemic layer depends on the Scribe correctly distilling decisions from chat. If the Scribe misclassifies a discussion as a decision, or misses a decision entirely, or records the wrong rationale, the decision log is corrupted and Pythia's assessments are based on false premises. Append-only logging prevents tampering but not misrecording. The Scribe is an AI agent, and AI agents bullshit.

What would falsify: systematic comparison of the Scribe's decision log against human-annotated ground truth from the same chat transcripts. If precision or recall falls below 80%, the decision log is not reliable enough to ground risk assessment on.

## Conclusion

NBS is an attempt to solve the specific problem of AI indifference to truth in engineering contexts. It does this by making falsifiability structural — baked into the decision logging, the risk assessment, the review process, and the coordination infrastructure — rather than aspirational.

The CinderX project demonstrated both the framework's value and its costs. It caught real errors that would have shipped without it. It also consumed substantial coordination overhead and failed to catch the cross-binary measurement error for weeks before the methodology was finally questioned.

The framework is open source, installs in three commands, and integrates with Claude Code. Whether it generalises beyond its origin context is an empirical question that I cannot answer from inside the project. The tools are at [github.com/SonicField/nbs-framework](https://github.com/SonicField/nbs-framework). If you use them, I would like to know what breaks.
