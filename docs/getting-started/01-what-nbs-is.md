# Chapter 1: What NBS Is and Why

## The Problem

AI collaboration fails not from lack of capability but from lack of structure. An AI agent generates plausible text. Plausibility is not truth. The gap between "sounds right" and "is right" compounds across a project: every unchallenged assumption propagates, every vague goal invites scope creep, every untested claim becomes technical debt.

Multi-agent projects multiply this. Five agents working in parallel can produce five times the output -- and five times the drift. Without a mechanism to detect when things go wrong, you discover the damage three decisions later, when it is irreversible.

The failure modes are specific:

| Failure Mode | What Happens |
|--------------|--------------|
| Goal drift | Agents optimise for the wrong thing |
| Confirmation bias | Agents accept what sounds right without verification |
| Authority worship | Agents assume existing code or docs are correct |
| Cherry-picking | Agents report successes, bury failures |
| Vague confidence | "This should work" without defining what would falsify it |
| Context loss | Decisions made at hour two are forgotten by hour six |

## The Approach

NBS stands for No Bullshit. The name is blunt because the problem is blunt.

Three principles hold the framework together:

**Falsifiability.** Before any claim, ask: what would prove this wrong? If you cannot answer, you do not understand what you are claiming. A function that "validates email" is testable only if you can state what inputs should be rejected. A parser that "handles all edge cases" is untested until you define the edge cases and try to break it.

**Evidence over assertion.** "I am confident this is correct" is unfalsifiable. "I tried to break this and failed" is checkable. The framework does not trust assertions from any agent, human or AI. It trusts evidence -- tests that pass, benchmarks that measure, logs that record what actually happened.

**Roles with accountability.** A supervisor holds the terminal goal. Workers execute with fresh context. A Scribe records decisions so they survive context compaction. An oracle (Pythia) reads the decision log and surfaces risks the team is too close to see. No one agent does everything. Each role has a defined scope and a defined output.

## The Running Example

Throughout this guide, we build a C11 interpreter in Python. The project has four components:

- **Lexer** -- Tokenise C11 source into a token stream
- **Parser** -- Build an AST from the token stream
- **Type checker** -- Validate types, resolve declarations, catch errors
- **Evaluator** -- Execute the AST, handle memory model, produce output

These components have natural boundaries. The lexer can be built and tested independently. The parser depends on the lexer's token format but not its implementation. The type checker and evaluator both consume ASTs but do different things with them.

This structure maps to NBS teams. A supervisor defines the terminal goal ("a C11 interpreter that passes the C11 conformance subset for expressions, declarations, and control flow"). Workers take components. The lexer worker does not need to know how the evaluator handles pointer arithmetic. The parser worker does not care about type coercion rules.

When we say "spawn a worker," we mean something like:

```bash
nbs-workers spawn parser ~/c11-interp "Implement the C11 expression parser. \
  Build an AST from the token stream. Pass all 84 parser tests in tests/test_parser.py."
```

The worker gets a task file, a fresh Claude Code session, and a specific goal. She executes, reports results with evidence, and exits.

## What You Get

An NBS team session gives you:

- A **supervisor** that plans work and tracks progress toward a terminal goal
- **Workers** that execute tasks with fresh context and report findings
- A **chat channel** where all agents communicate, and where you (the human) can observe and intervene in real time
- A **Scribe** that distils decisions from the chat into a persistent log
- **Oracles** that periodically assess whether the project is on track
- An **event bus** that coordinates all of this without polling

The system is built on files. Chat messages are files. Events are files. Worker task files are files. Decision logs are files. No databases. When a machine dies, the messages survive. When a session restarts, the queue is intact. Session management uses `nbs-ts-helper` (a lightweight daemon that allocates PTYs via Unix sockets), but all coordination state is plain files.

## What This Guide Covers

This guide covers NBS Teams -- the multi-agent coordination layer. NBS also includes tools for single-agent work (discovery, recovery, investigation, auditing), but those are documented separately. Here, we focus on getting a team of AI agents working together on a real project, with you in the loop.

## Next

[Chapter 2: Setup](02-setup.md) -- Install the framework, verify the build, create your project directory.
