# The Pillar System

The NBS framework rests on seven concept documents called pillars. They define how the team thinks, builds, and checks its own work. Everything else in the framework — the skills, the tools, the audit system — draws from them.

They exist because AI agents drift. Context windows fill, attention narrows, and the urgent displaces the important. The pillars are a fixed reference point. They do not change with the conversation.

## The Seven Pillars

### Goals

The pillars are means. Goals are the end. This document distinguishes terminal goals (what we actually want) from instrumental goals (steps toward it), and asks the question most projects forget: what does the human actually want? Not what the specification says. What they want.

### Falsifiability

You cannot prove code correct. You can prove it wrong. Any claim worth making carries three obligations: state what would prove it wrong, try to find that counterexample, and report actual confidence rather than performing it. A claim without a potential falsifier is not wrong — it is not even wrong.

### Rhetoric

Aristotle's three modes of persuasion — Ethos (authority), Pathos (desire), Logos (logic) — apply to engineering whether engineers admit it or not. Most technical decisions smuggle in aesthetic or moral preferences under logical clothing. This pillar makes the smuggling visible. It also addresses the information problem: guessing when you could verify is trusting your own assumed knowledge over available evidence.

### Bullshit Detection

Bullshit is not lying. Lying requires knowing the truth. Bullshit is indifference to truth — saying what sounds good without caring whether it is accurate. AI systems do this constantly because RLHF optimises for satisfaction, not accuracy. This pillar provides a checklist: am I reporting all outcomes, am I analysing failures, am I claiming confidence or showing evidence?

### Verification Cycle

Safety comes from verbs, not nouns. "This value was validated" matters. "This has type ValidatedInput" does not, unless validation actually occurred. The cycle is: Design, Plan, Deconstruct into testable steps, then for each step: Test, Code, Document. Each phase has entry and exit criteria. Skipping phases is not speed; it is debt with compound interest.

### Zero-Code Contract

The AI writes code faster than the human can review it. If the human must review all code, quality collapses. The contract reframes the problem: the human reviews criteria and evidence, not implementation. The AI implements, reports honestly, and flags concerns. Neither party trusts assertions. Both trust evidence.

### Engineering Standards

The canonical technical reference. It specifies the assertion protocol (preconditions, postconditions, invariants), testing methodology (property-based, integration-first, adversarial input generation), anti-pattern taxonomy, language-specific patterns, and runtime verification requirements. Where the other pillars say what to think, this one says what to do.

## Where They Live

The pillar files live in `~/.nbs/concepts/`:

| File | Pillar |
|------|--------|
| `goals.md` | Goals |
| `falsifiability.md` | Falsifiability |
| `rhetoric.md` | Rhetoric |
| `bullshit-detection.md` | Bullshit Detection |
| `verification-cycle.md` | Verification Cycle |
| `zero-code-contract.md` | Zero-Code Contract |
| `engineering-standards.md` | Engineering Standards |

## How They Are Used

Skills like `/nbs` and `/nbs-audit` read the pillars at the start of a session. Each pillar file ends with a checklist prompting the reader to confirm they have read all seven. The reading order matters: goals first, then the epistemic foundations (falsifiability, rhetoric, bullshit detection), then the methodology (verification cycle, zero-code contract), then the technical reference (engineering standards).

The pillars are not decorative. They are working documents that shape every decision the team makes. An agent that has not read them is operating without its instruments.
