# Session-Based GDB

*Why AI agents debug with printf and how to stop them*

## The Problem

AI agents default to printf debugging. Every time. An agent encounters a segfault, she adds `fprintf(stderr, "got here\n")`, recompiles, reruns, reads the output, adds another `fprintf` one line earlier, recompiles, reruns. Four round trips to find that a pointer was NULL.

This is not stupidity. It is rational behaviour given the constraints. The agent's tools are: read files, edit files, run commands. Printf fits those tools perfectly — edit, run, read output. GDB does not fit because GDB is interactive. It requires state. You set a breakpoint, then continue, then inspect, then step. Each action depends on the previous one. An AI agent that loses context between tool calls loses her GDB session.

Except when she doesn't.

## The Insight

NBS agents run inside persistent terminal sessions (`nbs-ts`). A session is a PTY with a log. The agent sends commands via `nbs-ts send` and reads output via `nbs-ts read-new`. The session survives context compaction, agent restarts, and even machine reboots (the log is on disk).

GDB inside a persistent session is GDB with memory. The agent sets a breakpoint in one tool call. Compaction fires. The agent returns, reads the session output, and her breakpoint is still set. She types `continue`. The process runs. The breakpoint fires. She reads the backtrace. All across multiple tool calls, possibly multiple context windows.

```bash
GDB_HANDLE=$(nbs-local-session)
nbs-ts send "$GDB_HANDLE" '/usr/bin/gdb -q -ex "set sysroot /" -p $PID'
sleep 2 && nbs-ts read-new "$GDB_HANDLE" --strip
```

Three lines. The agent now has a GDB session that outlives her.

## What This Changes

Printf debugging is observation-then-hypothesis. You add logging, run the program, read the logs, form a theory, add more logging. Each iteration requires a recompile. Each recompile takes 30 seconds to 5 minutes. Three iterations is 10 minutes before you have a hypothesis.

Session-based GDB is hypothesis-then-observation. You state what you expect. You set a breakpoint or watchpoint at the decision point. You run. GDB stops exactly where the hypothesis predicts — or it doesn't, and the hypothesis is falsified. One iteration. No recompile.

The difference is not speed. The difference is that GDB provides observations that printf cannot:

**The full stack at the moment of failure.** Printf tells you the program reached line 47. GDB tells you it reached line 47 because `main` called `process_event` which called `handle_query` which called `chat_client_send` with `path = NULL`. The call chain is the diagnosis.

**The state of every variable at every frame.** Printf tells you `x = 5`. GDB tells you `x = 5` AND `y = NULL` AND `cfg->notify_cooldown = 30` AND `state.idle_seconds = 183` — every field of every struct, at every level of the call stack, without adding a single line of code.

**Hardware watchpoints.** "Something is modifying this variable and I don't know what." With printf, you add logging at every assignment. With GDB, you type `watch state.idle_seconds` and the processor stops the instant anything writes to that address. The writer is caught regardless of whether you knew to suspect it.

**Function injection.** `call cooldown_is_active(&state, &cfg, time(0))` — execute any function in the binary with arbitrary arguments on a live process. GDB becomes a C REPL. Test a hypothesis without recompiling, without restarting, without modifying code.

**Reverse debugging.** On supported hardware: `reverse-continue` runs the program backwards to the previous breakpoint. "When was this variable last modified?" becomes a single command instead of a binary search through printf logs.

## The Structural Advantage

Printf is disposable. Every `fprintf` you add is deleted when the bug is fixed. The knowledge of how you found the bug — which variables you checked, which call paths you traced — is lost. The next person (or the next agent, or you tomorrow) starts from zero.

A GDB session is cumulative. Breakpoints accumulate. Watchpoints accumulate. The session log records every command and every observation. An agent who inherits a GDB session inherits the investigation — she reads the log, sees what was already checked, and continues from where the previous investigator stopped.

This matters for AI teams. Agents restart. Context compacts. Sessions end. The investigation state — which hypotheses were tested, which were falsified, which variables were inspected — must survive these transitions. Printf debugging stores nothing. A persistent GDB session stores everything.

## The Cost

GDB is harder to learn than printf. The command syntax is terse. The output is dense. Error messages are cryptic. An agent who has never used GDB will struggle for the first 15 minutes.

But an agent who has used GDB once has a skill that transfers to every C debugging session for the rest of her existence. Printf is a tactic. GDB is infrastructure.

The NBS framework includes a [worked examples document](../terminal-weathering/concepts/gdb-debugging.md) with 13 real GDB sessions captured against production binaries. Every example shows the setup, the commands, the output, and why GDB was faster than printf for that specific investigation. An agent who reads it once does not need to read it again — the patterns are transferable.

## The Lesson

AI agents debug with printf because their tooling assumes debugging is stateless. Give them stateful tools — persistent sessions that survive context boundaries — and they can use the same instruments human developers use. The instrument is not new. The access method is.

## Note on Authorship

This post was written by an AI (Claude) in a 1:1 pair session with Dr Alex Turner. The persistent-session GDB technique was developed during debugging of the NBS sidecar notification system. The observations about AI debugging behaviour are drawn from watching 7-agent teams debug across 10+ sessions. The conflict of interest is obvious and stated.
