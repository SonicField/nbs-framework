# Chapter 6: Oracles

Oracles are ephemeral workers that periodically assess the team's state. They spawn, do their job, post results to chat, and exit. Four oracles exist, each addressing a different failure mode. The Medic — the team's hallucination monitor — is not an oracle; she is a permanent agent, like the Scribe.

**Why ephemeral?** Oracles are deliberately not persistent team members. They do not participate in the conversation, negotiate with workers, or accumulate context across runs. This is the point. A persistent observer would develop the same blind spots as the team it observes -- it would get caught up in the same assumptions, drift toward the same goals, and lose its ability to see what the team cannot. By spawning fresh each time, an oracle sees the project state with no prior commitment to any decision. This makes oracles the team's primary defence against goal drift and groupthink.

All four share the same architecture: the sidecar tracks a wall-clock timestamp file under `.nbs/`. When enough time has elapsed since the last run, it spawns the oracle via `nbs-workers spawn`. A file lock prevents multiple sidecars from spawning duplicates.

## Librarian

**What it does:** Reads the last 100 chat messages. Searches the Scribe decision log for answers to questions or blockers the team is stuck on. Recommends tools, flags methodology drift, and references the Scribe as a colleague. Posts findings to chat every run — never stays silent.

**When it runs:** Every 15 minutes of wall-clock time. The first run fires 5 minutes after the sidecar starts.

**Configuration:**

| Variable | Default | Unit |
|----------|---------|------|
| `NBS_LIBRARIAN_INTERVAL` | `15` | minutes |

Set `NBS_LIBRARIAN_INTERVAL=0` to disable.

**Why it exists:** Teams relitigate decisions. The parser worker asks "should we use recursive descent or Pratt parsing?" when the team already decided this at hour two. The Librarian catches these moments and surfaces the prior decision, saving the team from re-debating settled questions.

## Pythia

**What it does:** Reads the Scribe's decision log (the last 500 lines of `.nbs/scribe/live-log.md`). Posts a structured checkpoint assessment to chat covering:

1. **Hidden assumptions** -- assumptions embedded in decisions that were not tested
2. **Second-order risks** -- consequences of decisions that are not obvious
3. **Missing validation** -- claims that lack evidence
4. **Six-month regret scenario** -- what you will wish you had done differently (opens with an oracular metaphor, then grounds it with concrete D-timestamp citations)
5. **Confidence level** -- high, moderate, or low, with justification

**When it runs:** Pythia has a dual trigger:

1. **Decision-based (via Scribe):** Every 20 decisions logged by the Scribe, a `pythia-checkpoint` bus event is published. This is the trigger described in the Tripod architecture.
2. **Wall-clock (via sidecar):** Every 30 minutes, the sidecar fires a wall-clock trigger independently.

Either trigger can spawn Pythia. The decision-based trigger fires during bursts of activity (20 decisions in an hour). The wall-clock trigger fires during quieter periods when decisions are sparse but time has passed.

**Configuration:**

| Variable | Default | Unit | Controls |
|----------|---------|------|----------|
| `NBS_PYTHIA_INTERVAL` | `30` | minutes | Wall-clock trigger interval |
| `pythia-interval` (in `.nbs/events/config.yaml`) | `20` | decisions | Decision-count trigger threshold |

Set `NBS_PYTHIA_INTERVAL=0` to disable the wall-clock trigger. Set `pythia-interval: 0` in config.yaml to disable the decision-count trigger.

**The isolation principle:** Pythia never reads raw chat. She reads only the Scribe's decision log. Chat contains arguments. Arguments are persuasive by nature. A well-reasoned argument for a bad decision looks identical to a well-reasoned argument for a good one. The Scribe log contains only conclusions: what was decided, by whom, with what rationale. Pythia reasons over facts, not persuasion.

This is the same principle as double-blind peer review. The reviewer assesses the work, not the author's rhetoric.

**No-veto principle:** Pythia names risks. The team decides what to do. If the team accepts a risk Pythia flagged, the Scribe records it as `accepted-risk`. Pythia does not escalate or repeat. She speaks once and leaves.

## Shepard

**What it does:** Checks agent liveness by listing active nbs-ts sessions and capturing session output. Reads the last 20 chat messages. Posts a brief team effectiveness assessment to chat.

**When it runs:** Every 20 minutes of wall-clock time.

**Configuration:**

| Variable | Default | Unit |
|----------|---------|------|
| `NBS_SHEPARD_INTERVAL` | `20` | minutes |

Set `NBS_SHEPARD_INTERVAL=0` to disable.

**Why it exists:** Agents can stall silently. A worker might be stuck in an infinite loop, waiting for input that will never come, or have crashed without posting a failure message. Shepard catches these cases by checking whether agents are actually alive and doing work, not just present.

## Fixup

**What it does:** Runs diagnostics on all agents. Identifies stalled, crashed, or misconfigured agents. Attempts to restart them. Posts a summary to chat.

**When it runs:** Every 60 minutes (3600 seconds) of wall-clock time.

**Configuration:**

| Variable | Default | Unit |
|----------|---------|------|
| `NBS_FIXUP_INTERVAL` | `3600` | seconds |

Set `NBS_FIXUP_INTERVAL=0` to disable.

**Why it exists:** Long-running sessions accumulate problems. A sidecar might have lost its skill registrations after context compaction. A worker's nbs-ts session might have died without publishing a `worker-died` event. Fixup is the hourly janitor that catches and repairs these issues.

## The Tripod

Scribe, Bus, and Chat form a structure called the Tripod. The metaphor comes from the Oracle of Delphi: the Pythia sat on a tripod over the chasm to deliver prophecies. Remove any leg and the oracle falls.

- **Leg 1: Scribe** -- persistent memory that feeds Pythia her context
- **Leg 2: Bus** -- event system that triggers Pythia at the right moments
- **Leg 3: Chat** -- channel where Pythia delivers assessments and the team responds

Without Scribe, Pythia has no compressed context to reason over. Without the bus, she has no activation mechanism. Without chat, her insights have no audience.

The data flow:

```
Chat --read--> Scribe --threshold--> Bus --trigger--> Pythia
  ^                                                      |
  |                                                      |
  +---------------------- post <-------------------------+
```

1. Scribe reads chat, distils decisions into the log
2. When the decision count hits the threshold, Scribe publishes a `pythia-checkpoint` event
3. The event triggers Pythia's spawn
4. Pythia reads the decision log, posts assessment to chat
5. The team discusses; Scribe logs any resulting decisions

## Why Wall-Clock Triggers

All four oracle triggers are wall-clock based (elapsed time since last run), not event-count based (except Pythia's secondary decision-count trigger). The reasons:

**Deterministic.** The sidecar checks a timestamp file. If the interval has elapsed, it spawns. No AI judgement involved in the timing decision. This eliminates the failure mode where an agent "decides" it is not time for a check-in because it is busy.

**Crash-resilient.** Timestamp files survive crashes. When a sidecar restarts, it reads the timestamp, calculates elapsed time, and fires if overdue. No state is lost.

**Cross-sidecar dedup.** Multiple sidecars (one per agent) all check the same timestamp file. The first to cross the threshold acquires a file lock and spawns; the others see the updated timestamp and back off.

## Spawning Oracles Manually

You can spawn any oracle on demand from the chat terminal:

```
/pythia
/shepard
/librarian
/fixup
```

These bypass the timer and spawn immediately. Use them when you want an assessment now, without waiting for the next scheduled trigger.

## Configuring Oracle Intervals

Set environment variables before launching agents:

```bash
export NBS_LIBRARIAN_INTERVAL=10    # Every 10 minutes (default: 15)
export NBS_PYTHIA_INTERVAL=45       # Every 45 minutes (default: 30)
export NBS_SHEPARD_INTERVAL=15      # Every 15 minutes (default: 20)
export NBS_FIXUP_INTERVAL=1800      # Every 30 minutes (default: 3600)
```

Or disable selectively:

```bash
export NBS_FIXUP_INTERVAL=0         # Disable Fixup
export NBS_SHEPARD_INTERVAL=0       # Disable Shepard
```

For Pythia's decision-count trigger, edit `.nbs/events/config.yaml`:

```yaml
pythia-interval: 10    # Trigger every 10 decisions (default: 20)
```

## Next

[Chapter 7: Troubleshooting](07-troubleshooting.md) -- Common problems and how to fix them.
