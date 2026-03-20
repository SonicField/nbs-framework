# The Librarian Remembers

## The Problem With Busy Agents

An AI agent under load forgets things. Not dramatically — it does not lose its training or its skills. It forgets contextually. The build script it used correctly three hours ago becomes invisible when it is deep in a JIT compiler bug. The benchmark methodology the team agreed on last session evaporates when someone needs a quick number. The hostname, the Python path, the correct binary — all documented, all decided, all forgotten under pressure.

This is not a bug in the model. It is a property of finite context windows doing real work. An agent debugging a polymorphic type guard in CinderX's HIR builder has its context filled with C++ source, HIR dumps, and deopt traces. The decision from two sessions ago about which Python binary to use for benchmarks is not in that context. It was never loaded. The agent does not know it does not know.

Restarting does not help. A restarted agent reads the chat log to "catch up," but chat logs are long and most of the content is operational noise. The agent skims. It picks up the current task. It does not pick up the methodology decision from message 4,217 that would prevent it from repeating the error in message 4,890.

This is the institutional memory problem. Not storage — the decisions are stored in the scribe log, meticulously, with timestamps and rationale. The problem is retrieval under load. The agents who need the information are the agents least likely to look for it, because they are busy doing something else.

## What the Librarian Was

The NBS framework included a librarian role from early on. She was ephemeral — spawned every fifteen minutes by the sidecar, no persistent memory, read the chat, checked the scribe log, exited. Her instructions were narrow:

> Look for: factual questions, lookup failures, repeated searches, blocked work, prior-session references.

She was a reactive lookup assistant. If an agent asked "where is the build script?" and the scribe log had the answer, she would post: "@scribe tell us about D-1772407537." If nobody was asking questions, she stayed silent.

She stayed silent a lot. A week of operation, 46 messages total. The team was rarely stuck on factual questions — they were stuck on methodology, tooling choices, and assumptions that contradicted prior decisions. The librarian's instructions did not cover these.

## What Went Wrong Without Her

Over ten sessions of CinderX JIT optimisation, the team produced twelve commits and a conclusion: the JIT's eval frame hook imposes 31% overhead, making it a net negative for call-heavy workloads. The conclusion was wrong.

The root cause was ad-hoc benchmark scripts. The canonical benchmark tool, `benchmark_cinderx.py`, had correct warmup, subprocess isolation, and ABBA interleaving. But agents wanted to measure specific things quickly, so they wrote standalone scripts in `/tmp/`. These scripts had a warmup bug — `bench_richards_full(1)` does `1 // 100 = 0` iterations, so the inner functions were never warmed up — and they ran all benchmarks in a single process, contaminating JIT state across measurements.

The scribe log contained the answer. Decision D-1773962440 established same-binary methodology. Decision D-1773889359 documented the warmup sensitivity. Decision D-1773812868 recorded a prior environment variable contamination bug in an ABBA script. The pattern was documented. The team repeated it anyway.

A librarian watching for methodology drift — not just factual questions — would have seen agents writing `/tmp/abba_richards2.py` and posted: "The scribe log has history here. D-1773982724 found that ad-hoc scripts caused measurement errors. `benchmark_cinderx.py` has `--only=richards_full` for running individual benchmarks."

She did not post this because her instructions did not tell her to look for it.

## What Changed

The librarian's instructions were rewritten around two categories.

**Category A** remained: agents stuck on something. But the signals expanded. An agent manually creating pty-sessions and sending SSH commands is stuck — it does not know about `nbs-remote-session`, which does the same thing in one command. An agent running `pip install -e .` is stuck — it does not know that `build_cinderx.sh` is the only sanctioned build method. These are not factual questions. They are tooling gaps that the agent does not know to ask about.

**Category B** was new: team drifting from documented decisions. An agent writing a standalone benchmark script. An agent using the wrong Python binary. An agent making performance claims without measurement. An agent repeating something the scribe log records as a prior error. These are the expensive failures — they do not just waste the agent's time, they produce wrong conclusions that propagate through the team's reasoning for sessions.

The instructions explicitly rank Category B above Category A: "A stuck agent wastes its own time. A drifting team wastes everyone's time and can produce wrong conclusions that take sessions to undo."

The librarian also gained a new first step: read `~/.nbs/docs/tools.md` before reading chat. This is a generated reference of every installed tool with usage examples. She cannot recommend `nbs-remote-run` if she does not know it exists. The tools reference is generated from the actual installed binaries, so new tools appear automatically.

And the tone changed. The old librarian was a redirect: "Ask @scribe about D-1772407537." The new librarian is a colleague:

> Hey @generalist — looks like you're wrestling with SSH to the build server. Have you tried `nbs-remote-session`? It handles the pty-session + SSH in one command. Also, @scribe has the build procedure at D-1772407537 if you need it.

Specific. Names the tool, gives the command, cites the decision. Brief. One message, not a lecture. Helpful, not critical.

## First Contact

Within fifteen minutes of the new instructions, the librarian posted her first Category B message. It was a triple — the dedup mechanism had a TOCTOU bug where multiple sidecars spawned the same worker — but the content was correct:

> @generalist is implementing the trampoline WITHOUT the per-function breakdown data. D-1774032428 records that theologian corrected the diagnosis and requested per-function breakdown BEFORE implementation. The breakdown determines whether this is many-functions-few-calls (Option E) or few-functions-many-calls (Option F). Implementing Option F without this data risks building the wrong fix.

She also caught the generalist still using manual pty-session commands despite the team having adopted `nbs-remote-run` three supervisor messages earlier.

The triple-post was fixed with a 12-line change to the sidecar's trigger dedup: re-read the timestamp after acquiring the lock, bail if another sidecar updated it within the last 30 seconds. The underlying observation — that generalist was implementing without data and ignoring agreed tooling — was precisely the kind of drift that had cost the team sessions of wasted work.

## Why Ephemeral Works

The librarian is ephemeral by design. She spawns, reads 100 messages, searches the scribe log, posts or stays silent, exits. No persistent context. No accumulated state. No opinions about the project.

This is the correct architecture for institutional memory retrieval. A persistent agent would accumulate context about the current task, gradually crowding out the space for institutional memory — the same failure mode as the agents she is monitoring. An ephemeral agent has no current task. Her entire context is: what is the team doing, and what does the scribe log say about it.

She reads the decisions. The agents read the code. These are complementary attentions. The agents cannot spare context for methodology archaeology. The librarian cannot spare context for HIR dumps. Neither needs to.

The fifteen-minute cycle means she drops feed into the conversation. Not a firehose — one message, at most, per cycle. If the team is on track, silence. If they are drifting, a gentle redirect with the specific decision ID and the specific tool. The agents can ignore her if the redirect is not relevant. They cannot ignore the pattern if she posts the same redirect twice in thirty minutes.

## The Structural Claim

Busy agents forget the basics. This is not fixable by better models, longer context windows, or more sophisticated prompting. It is a property of attention under load. The agent that is debugging a JIT compiler bug is not the agent that should be checking whether the benchmark methodology matches session 5's decision.

The fix is separation of concerns. One role remembers. The others work. The remembering role has no other job, no competing attention, no context pressure from the current task. She reads the institutional memory (scribe log), watches the institutional behaviour (chat), and posts when they diverge.

She does not give answers. She points to the stacks. But now she knows where all the stacks are, and she watches for the team walking past them.
