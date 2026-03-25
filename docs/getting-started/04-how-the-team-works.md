# Chapter 4: How the Team Works

You have seen a session run. Now understand the structure behind it: who does what, how work is scoped, and how the team avoids drift.

## Roles

### Supervisor

The supervisor holds the terminal goal. Everything the team does must serve this goal. The supervisor:

- Maintains terminal goal clarity -- "build a C11 interpreter that passes conformance tests"
- Decomposes work into worker tasks
- Spawns workers via `nbs-workers`
- Captures learnings after each worker completes (the 3Ws -- see below)
- Runs a self-check every 3 workers
- Coordinates via chat

The supervisor does not implement features. If the supervisor is writing parser code, something has gone wrong. The supervisor plans, delegates, and tracks.

### Workers

A worker is a fresh Claude Code session with a specific task. Workers:

- Read their task file (`.nbs/workers/<name>.md`)
- Execute with fresh context -- no accumulated state from previous work
- Report findings with evidence (test results, benchmarks, error logs)
- Escalate blockers rather than working around them

Fresh context is the point. A supervisor that has been running for six hours has accumulated context about every decision, every failed approach, every side discussion. A worker starts clean. She reads the task, does the work, reports what happened.

### Testkeeper

The testkeeper ensures verification standards are maintained. When workers produce code, the testkeeper checks:

- Are there tests? Do they try to break the code, not just confirm it works?
- Do assertions have meaningful messages?
- Are edge cases covered -- empty inputs, boundary values, malformed data?

### Gatekeeper

The gatekeeper reviews code before it merges. The gatekeeper checks for:

- Alignment with the terminal goal
- Architecture consistency
- Test coverage
- No silent failures or swallowed exceptions

The gatekeeper can block a push. This is the one role with veto authority over code changes.

### Theologian

The theologian handles theory and architecture. When the team faces a design decision -- recursive descent vs Pratt parsing, tree-walk vs bytecode evaluation -- the theologian analyses alternatives, states trade-offs, and proposes a direction. The team decides; the theologian advises.

### Scribe

The Scribe is a persistent agent instance with a long context window. She reads chat continuously and distils decisions into a structured log at `.nbs/scribe/live-log.md`.

The Scribe records decisions -- moments where the team chose a direction:

- Explicit choices: "we'll use recursive descent instead of Pratt parsing"
- Accepted risks: "we know the parser is O(n^2) but accept it because input is bounded at 4KB"
- Architecture changes: "moving from polling to event-driven"
- Scope changes: "dropping feature X from MVP"

The Scribe does not record status updates, greetings, or discussions that did not result in a decision. The decision log survives context compaction, session restarts, and agent rotation. Decisions made at hour two are still accessible at hour twenty.

## The 3Ws

After every worker completes, the supervisor captures three things:

- **What went well** -- Keep doing this
- **What didn't work** -- Stop doing this
- **What we can do better** -- Change this

This is not ceremony. It is the mechanism for accumulating project knowledge. If the lexer worker found that the test suite catches edge cases better when tests are written before the implementation, that learning applies to the parser worker too.

Example after the lexer worker completes:

```
supervisor: 3Ws for lexer-a3f1:
  Well: Writing tests first caught 3 tokenisation bugs before they propagated.
  Didn't work: Worker tried to handle Unicode before ASCII was solid — wasted time.
  Better: Scope task to ASCII-only for MVP, handle Unicode as a separate worker.
```

## Self-Check

After every 3 workers, the supervisor pauses and asks:

1. Am I still pursuing the terminal goal?
2. Am I delegating vs doing tactical work myself?
3. Have I captured learnings?
4. Should I escalate to the human?

This catches drift. A supervisor that has spawned 9 workers optimising the lexer's Unicode handling has lost sight of the terminal goal (build a working interpreter). The self-check forces a step back.

## Task Scoping

Task scope is the most common source of failure. There are two failure modes:

### Too narrow (micromanagement)

**Wrong:**
```
Worker 1: Implement parse_int()
Worker 2: Implement parse_string()
Worker 3: Implement parse_block()
```

This is writing implementation steps, not delegating. The supervisor has already decided *how* to build the parser. The worker has no freedom to choose an approach.

### Right (delegation)

**Right:**
```
Worker: Implement the C11 expression parser. Build an AST from the token stream.
  Pass all 84 tests in tests/test_parser.py.
```

The worker decides how to parse expressions. Maybe it uses recursive descent. Maybe it uses Pratt parsing. The supervisor does not care -- the worker's job is to pass the tests.

### The rule

If you are writing implementation steps, scope is too narrow. Set the goal, define the success criteria, let workers choose the path.

### Scoping for the C11 interpreter

Good task scoping for our running example:

| Worker | Task | Success criteria |
|--------|------|-----------------|
| lexer | Implement the C11 lexer | All tests in `tests/test_lexer.py` pass |
| parser | Implement the C11 expression parser | All 84 tests in `tests/test_parser.py` pass |
| typechecker | Implement type checking for declarations and expressions | Type error tests catch all invalid programs |
| evaluator | Implement the AST evaluator for arithmetic and control flow | Integration test runs a 50-line C program correctly |

Each worker has a clear goal and measurable success criteria. None prescribes an implementation approach.

## Worker Completion States

Workers end in one of five states:

| State | Meaning |
|-------|---------|
| `completed` | Success criteria met. Results in the task file. |
| `failed` | Criteria not met. Reason documented in the task file. |
| `escalated` | Worker cannot proceed. Needs supervisor or human input. |
| `dismissed` | Supervisor reviewed results and closed the worker. |
| `died` | Session exited unexpectedly (crash, timeout, machine reboot). |

Completion is not perfection. A worker who reports "parser handles 81 of 84 tests — the 3 failures are pointer arithmetic edge cases" has completed honestly. The supervisor spawns a follow-up for the remaining cases.

A worker who silently ignores failing tests and reports "all done" has produced bullshit. Honest reporting of negative outcomes is more valuable than a false positive.

### When a Worker Dies

Use `nbs-workers continue` to resume a worker whose session died:

```bash
nbs-workers continue lexer-a3f1
```

This kills the old session, respawns a fresh Claude Code session with the saved session ID (`claude --resume`), and re-attaches the worker to her task file. The worker picks up where she left off.

### When a Worker Escalates

Escalation means the worker hit something she cannot resolve alone — a design question needing human judgement, a missing dependency, a contradiction in the task. The supervisor reads the escalation, resolves the blocker (involving the human if needed), and either continues the existing worker or spawns a new one.

## Sessions and Resumption

A session is a period of work — you sit down, run the team, and eventually stop. NBS is designed for continuity across sessions.

### What Persists

Everything in `.nbs/` survives between sessions:

- **Chat history** — the full conversation, accessible via `nbs-chat read` or `nbs-chat export`
- **Decision log** — every decision the Scribe recorded, with rationale
- **Worker task files and logs** — what each worker was assigned and what she produced
- **Bus events** — pending events from workers that completed while you were away

### Starting a New Session

```bash
cd ~/c11-interp
nbs-chat-terminal .nbs/chat/c11-interp.chat <your-handle> --restart
```

The supervisor checks pending bus events, reads recent chat, and picks up where she left off. The decision log gives her the full history of what was decided and why — even if her context was compacted or she is a fresh instance.

### Ending a Session

Type `/shutdown` in the chat terminal. This kills the team — all agent sessions are terminated.

If workers are still running when you leave, their sessions persist. When you come back, check their status:

```bash
nbs-workers list
```

Any worker that `died` while you were away can be resumed with `nbs-workers continue`.

### Knowing When the Project Is Done

The project is complete when every success criterion in your goal file has evidence:

| Criterion | Evidence |
|-----------|----------|
| Lexer tokenises valid C11 source | 58 tests pass, including edge cases |
| Parser produces correct ASTs | All 84 test programs produce expected output |
| Type checker validates programs | Type errors caught with source location |
| Evaluator executes correctly | 50-line integration test produces correct output |

If every row has evidence, you are done. If any row is empty or says "I think it works," you are not.

## When to Use NBS Teams

Use NBS Teams when:

- The task requires multiple distinct phases (lexer, parser, type checker, evaluator)
- Context accumulation is causing drift
- You want fresh perspectives on sub-problems
- Work can be parallelised

Do not use NBS Teams when:

- The task is a single coherent unit
- Deep accumulated context is the asset (debugging, investigation)
- The task is trivial

## One Team Per Directory

A hard constraint: one team per project directory. The sidecar registry, bus events, cursor files, and trigger timestamps are all stored in `.nbs/` and keyed by agent handle. Running two teams in the same directory causes cross-talk.

To run multiple teams, use separate project directories:

```
~/c11-interp/     → c11-interp.chat team (interpreter work)
~/c11-stdlib/     → c11-stdlib.chat team (standard library implementation)
```

Each directory gets its own `.nbs/`, its own chat file, and its own independent team.

## Next

[Chapter 5: Communication](05-communication.md) -- Chat commands, @mentions, the event bus, slash commands.
