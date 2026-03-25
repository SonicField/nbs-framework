# Chapter 3: Running Your First Session

This is the hello world. You will launch a session, watch the supervisor plan, see workers spawn, and shut everything down. By the end you will have seen the full cycle before learning the details.

## Launch the Chat Terminal

From your project directory:

```bash
cd ~/c11-interp
nbs-chat-terminal .nbs/chat/c11-interp.chat <your-handle> --goal-file=goal.md --restart
```

Replace `<your-handle>` with whatever name you want to use in chat (e.g. your first name).

This does three things in sequence:

1. **Opens the chat terminal** -- an interactive view of the team chat channel. You are the named participant.
2. **Posts your goal file** -- the contents of `goal.md` are sent to the chat as your first message, so all agents know what the project is about.
3. **Starts the agent team** -- the `--restart` flag launches the supervisor and supporting agents via `nbs-claude`.

You will see the chat terminal interface: a scrolling message view with your handle's prompt at the bottom. Messages from different agents appear in different colours.

## Watch the Supervisor Plan

Within a few seconds, you will see the supervisor post to chat. The supervisor reads your goal and proposes a plan. It looks something like:

```
supervisor: Read goal.md. Terminal goal: C11 interpreter in Python.
  Proposed plan:
  1. Spawn lexer worker — tokenise C11 source
  2. Spawn parser worker — build AST from token stream
  3. Once lexer and parser pass tests, spawn type checker worker
  4. Spawn evaluator worker after type checker
  Proceeding with steps 1-2 in parallel.
```

The supervisor decomposes the terminal goal into worker tasks. Each task has a clear deliverable (pass tests, produce output) and can be executed independently.

## Watch Workers Spawn

The supervisor spawns workers using `nbs-workers`. Each worker gets:

- A **task file** at `.nbs/workers/<name>.md` describing what to do
- A **fresh Claude Code session** with no prior context
- A **persistent log** at `.nbs/workers/<name>.log` capturing all output

You will see messages like:

```
supervisor: Spawned lexer-a3f1 — implement C11 lexer, pass all tokenisation tests.
supervisor: Spawned parser-7b2c — implement C11 expression parser, pass 84 tests.
```

The workers begin executing immediately. They read their task files, write code, run tests, and post findings to the chat channel.

## Observe the Work

As workers execute, you will see their messages in the chat:

```
lexer-a3f1: Starting lexer implementation. Reading test file tests/test_lexer.py
  to understand expected token format.
lexer-a3f1: Implemented keyword and operator tokenisation. 42/58 tests pass.
  Remaining failures are string literals and multi-character operators.
parser-7b2c: Lexer not yet available — writing parser against the token format
  specified in goal.md. Will integrate when lexer is ready.
```

Workers communicate through the chat channel. The supervisor monitors progress, reads results, and adjusts the plan.

## Interact with the Team

You are a full participant. Type a message and press Enter to send:

```
you: Focus on getting the lexer to 100% first — the parser depends on it.
```

Your message appears in the chat. The `nbs-chat-terminal` automatically publishes a `human-input` bus event, which gives your message priority attention from the agents. The supervisor reads it and adjusts:

```
supervisor: Acknowledged. Prioritising lexer completion. parser-7b2c — pause
  on AST implementation, switch to writing parser test infrastructure.
```

You can use `@handle` to address a specific agent:

```
you: @lexer-a3f1 Are string literals handled yet?
```

## Check Worker Status

While the terminal is running, you can open another terminal to inspect state:

```bash
# List all workers and their status
nbs-workers list

# Check a specific worker
nbs-workers status lexer-a3f1

# Search a worker's output log
nbs-workers search lexer-a3f1 "test.*pass" --context=10

# Read a worker's completed results
nbs-workers results lexer-a3f1
```

## Shut It Down

When you are ready to stop, type `/shutdown` in the chat terminal:

```
/shutdown
```

This kills the team — all agent sessions are terminated.

To exit the chat terminal itself, type `/exit` or press Ctrl-C.

## What Just Happened

You ran a full multi-agent session:

1. Launched the chat terminal with your goal file
2. The supervisor read the goal, planned the work, and spawned workers
3. Workers executed tasks with fresh context and reported findings
4. You observed and intervened through the chat channel
5. You shut everything down cleanly

The key insight: you saw the whole cycle before needing to understand any of the underlying mechanisms. The chat terminal is your window into the team. Everything else -- the event bus, the sidecar, worker lifecycle management -- operates behind the scenes.

## What Is Left Behind

After the session, your project directory contains:

```
~/c11-interp/
├── goal.md
├── src/                     # Code written by workers
├── tests/                   # Tests written by workers
└── .nbs/
    ├── chat/
    │   └── c11-interp.chat        # Full chat history
    ├── events/
    │   └── processed/       # All processed bus events
    ├── scribe/
    │   └── live-log.md      # Decision log (if Scribe was active)
    └── workers/
        ├── lexer-a3f1.md    # Lexer worker task file
        ├── lexer-a3f1.log   # Lexer worker session log
        ├── parser-7b2c.md   # Parser worker task file
        └── parser-7b2c.log  # Parser worker session log
```

You can review the chat history:

```bash
nbs-chat export .nbs/chat/c11-interp.chat --last=50 | less -R
```

You can read any worker's full output log:

```bash
less .nbs/workers/lexer-a3f1.log
```

## Next

[Chapter 4: How the Team Works](04-how-the-team-works.md) -- Understand the roles, the 3Ws, self-check, and task scoping rules.
