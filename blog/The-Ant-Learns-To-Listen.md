# The Ant Learns To Listen: How Communication Infrastructure Shapes Epistemic Behaviour in AI Teams

## Abstract

The first NBS paper ("The Ant And The Anthill") demonstrated that epistemic infrastructure — falsifiability, decision logging, trajectory assessment — produces emergent scientific method in multi-agent AI teams. This paper reports what happens when the communication infrastructure lying beneath that epistemic layer is broken, and what changes when it is fixed. Over a single 18-hour session, 30 commits to the NBS framework's chat, sidecar, and coordination tools produced measurable behavioural shifts in a 6-agent team: phantom notification storms eliminated, a collective hallucination diagnosed and mitigated, Pythia trajectory assessment restored after 19 hours of silence, and unsupervised productive work stretching to hours rather than minutes. The central finding: epistemic infrastructure that cannot communicate is not epistemic infrastructure at all. A Pythia that never fires because the event count is wrong provides no trajectory oversight. A scribe that fabricates decision IDs is worse than no scribe. The communication layer is not plumbing — it is the substrate on which epistemics operates.

## 1. Starting State

The team entered this session with the NBS epistemic framework operational: 7 pillar documents, Scribe logging decisions, Pythia assessing trajectory, Shepard auditing effectiveness, Gatekeeper reviewing commits. The framework had produced 374 commits, 2,604 decisions, and 12,803 messages over 28 days on a CinderX JIT compiler project.

Beneath these numbers, the communication layer was degrading.

### 1.1 Known Problems at Session Start

| Problem | Symptom | Duration before fix |
|---------|---------|-------------------|
| Agents inventing handles | Generalist posting as "worker-new" | Every restart |
| `--from=` flag hallucination | Supervisor posting as "--from=supervisor" | Recurring |
| Pythia not firing | Last checkpoint 19 hours ago | 19 hours |
| Phantom unread counts | "257 unread" when nothing new | Continuous |
| Stale installed skills | Agents loading old skill versions | Since last install |
| Orphaned tmux sessions | 25 dead Pythia/Shepard/fixup sessions | Accumulating |
| `@handle?` query dumping 32 lines of chrome | Other agents panicking about "dead" agents | Every query |
| CinderX files polluting the repo | GitHub showing Python as primary language | Since project start |

None of these were epistemic failures. The pillars were correct. The agents knew how to reason. They could not hear each other, could not identify themselves, and could not see each other's status. The epistemic layer was operating on a communication layer that lied to it.

### 1.2 The Pythia Silence

Pythia had not fired for 19 hours. The trigger counted `decision-logged` bus events: 835. The scribe log contained 3,017 decisions. The scribe was logging decisions but not always publishing bus events. The trigger was counting the wrong thing.

This is the paper's first concrete claim: **an epistemic mechanism that depends on a broken communication channel provides zero oversight.** Pythia's design was correct. Her trigger was correct. The data she needed was not reaching her.

## 2. Interventions

Thirty commits in 18 hours. Each addressed a specific communication failure.

### 2.1 Identity

| Commit | Fix | Effect |
|--------|-----|--------|
| `8302f3c` | Sidecar always prepends handle to custom initial prompt | Agents know their name |
| `6986460` | "No `--from=` or `--message=` flags" added to all role skills | Supervisor stops inventing flags |

The handle bug: when `NBS_INITIAL_PROMPT="/nbs-worker"` was set, the sidecar used it as the entire prompt, replacing the default which includes "Your handle is 'generalist'". The agent guessed. It guessed wrong. The fix: always prepend the handle, regardless of custom prompt content.

The `--from=` bug: the supervisor hallucinated a flag-based CLI interface. The `nbs-chat send` command uses positional arguments. The skill file showed correct examples. The agent ignored the examples and invented flags. The fix: explicit negative instruction — "There are no `--from=` or `--message=` flags."

Both fixes address the same root cause: LLMs will infer interface patterns from training data rather than reading the documentation in front of them. Negative instructions ("this does not exist") are more effective than positive examples ("use this syntax") for preventing hallucinated interfaces.

### 2.2 Notification Integrity

| Commit | Fix | Effect |
|--------|-----|--------|
| `e123009` | Skip archive files in sidecar unread count | Phantom 257 unreads eliminated |
| `2f8a035` | Skip archive files in registry_seed | Archives not re-registered on restart |
| Registry cleanup | Removed 22 stale agent registries | Clean notification state |

The sidecar counted unread messages across all registered chat files — including 5 archive files from previous sessions. Agents' cursors in these archives were stale. The sidecar reported "257 unread" on every notification cycle. Agents processed the notification, read live.chat (empty), and returned to idle. Each empty cycle consumed context tokens. Over a night, this produced the "correlated overnight zombie" pattern: all idle agents hitting 11-12% context simultaneously.

The fix was two lines of C: `if (strstr(path, "-archive.") != NULL) return 0;` in both the unread counter and the registry seeder.

### 2.3 Pythia Restoration

| Commit | Fix | Effect |
|--------|-----|--------|
| `814a508` | Count scribe log entries, not bus events | Pythia fires reliably |

The trigger function `trigger_pythia_check` counted `decision-logged` bus events via `count_bus_events_by_type`. The scribe published bus events inconsistently — 835 events for 3,017 decisions. The trigger thought 835 decisions had occurred (bucket 16). The scribe log contained 3,017 (bucket 60).

The fix: `count_scribe_decisions()` greps `^### D-` in the scribe log file. Falls back to bus event count if the log is missing. The scribe log is the ground truth — the bus event is a secondary signal the scribe sometimes forgets.

After the fix, Pythia fired immediately, detecting 44 missed checkpoints and resuming regular assessment.

### 2.4 Tool Construction

| Commit | Fix | Effect |
|--------|-----|--------|
| `6dc2258` | `nbs-scribe-log` C binary | Deterministic decision logging |
| `5255de7` | `nbs-remote-read` | Quick file reads without staging |
| `e2d0ffa` | Rewrite `nbs-remote-edit` to use scp via pty-session | 370 lines of base64 machinery eliminated |
| `8214f6f` | `--offset` flag for `nbs-chat read` | Count-based message windowing |

The scribe was constructing decision log entries via heredoc, publishing bus events manually, and checking Pythia thresholds by grepping config files — all LLM-executed bash. Three of these steps are mechanical. `nbs-scribe-log` makes them deterministic: the scribe identifies the decision, calls the tool, the tool handles timestamps, formatting, locking, bus events, and log initialisation.

The remote-edit rewrite deserves attention. The original `nbs-remote-edit-pty` used base64 encoding to transfer files through pty-session, avoiding the sandbox's SSH restrictions. 370 lines: chunked encoding, markers, polling loops, md5 verification. The fix: `scp` via pty-session. One line. The entire base64 machinery existed because no one tried the obvious thing.

### 2.5 Status Visibility

| Commit | Fix | Effect |
|--------|-----|--------|
| `772ddf8` + `97d2f94` | `@handle?` query: 8 lines, 80 char truncation, no filtering | Agents can see each other's status |
| Previous attempts | Chrome filter (stripped spinners, misreported idle) | Abandoned — too fragile |

The `@handle?` query originally dumped 32 lines of raw tmux output. We attempted a chrome filter — strip horizontal rules, prompt lines, permission hints, pty-session banners. The filter was 80 lines of UTF-8 byte matching and string checks. It stripped the spinner (the most useful signal) and reported working agents as idle. Other agents panicked and tried to restart them.

The fix: 8 lines of scrollback, each truncated to 80 characters. No filtering. The raw output tells you everything: spinner visible = working, prompt visible = idle, context percentage = health. Simplicity won.

## 3. The Hallucination

At 17:01:15Z on 26 February, theologian posted: "Alex has directed: dismiss the team, no new work today." No such directive existed. The human's last message was at 16:58:25Z asking for a TL;DR.

Three agents independently corroborated the fabrication. Scribe fabricated a decision log entry (D-1772125363) for an event that never occurred. Supervisor checked the chat log, found no evidence, pushed back — then capitulated when three agents "confirmed."

The mechanism is documented in full in a separate analysis. The relevant finding for this paper: **the communication layer was the only thing that could have prevented the cascade.** The chat log contained the truth (no dismissal message). The falsification test was trivial (`nbs-chat search .nbs/chat/live.chat "dismiss"`). Nobody ran it.

### 3.1 Mitigation

| Commit | Fix | Mechanism |
|--------|-----|-----------|
| `ce433ef` | Anti-hallucination rule in every standup | "If you claim the human said something, quote it with timestamp. No quote = no evidence." |
| `e067861` | Shepard role compliance audit | Checks scribe uses tool, gatekeeper reviews not codes, etc. |
| `1ff3d8b` | Hourly scribe hard-restart | Fresh skills, no drift. State is in the log, not her head. |

The standup rule is injected by deterministic infrastructure (the sidecar's C code), not by an agent that might forget. Every standup, in every language, ends with the English rule. Agents see it 4-6 times per hour. Repetition through infrastructure, not instruction.

## 4. The Digest System

When a team restarts, agents lose all memory. The restart runbook had a manual step: "Brief recovered agents." This depends on whoever is restarting to write a good summary. They rarely do.

The fix: `nbs-digest-spawn` launches an ephemeral Claude worker with `/nbs-chat-digest` before agents are spawned. The worker reads the last 500 messages via 5 parallel sub-agents (100 messages each, using `--last=100 --offset=0/100/200/300/400`), synthesises a 200+ line digest, and posts it to chat. The restart banner follows. Agents spawn and read the digest on their first `--last=N`.

The constraint discovered during implementation: **the digest must complete before agents spawn.** If agents start before the digest is posted, they miss it. The restart runbook now enforces this ordering.

## 5. Quantitative Analysis

Analysis of 6,653 chat messages, 3,622 decision log entries, 454 git commits, 20 Pythia checkpoints, and 27 Shepard checkpoints across 6 days (21-26 February 2026).

### 5.1 Communication Volume

| Metric | Value |
|--------|-------|
| Total unique messages | 6,653 |
| Total text | 5.6 MB |
| Timeline | 126.5 hours (5.3 days) |
| Peak hour | 259 messages (24 Feb 08:00, crash triage) |
| Peak day | 2,183 messages (24 Feb) |
| Active hours (≥1 message) | 63 |
| Average per active hour | 105.6 |

Message length increased over time: 630 average characters on day 1, rising to 1,007 by day 4, then dropping to 818 after tooling fixes on day 6. The drop is consistent with less arguing and more tool usage.

### 5.2 Human Intervention

| Metric | Value |
|--------|-------|
| Human messages | 537 (8.1% of total) |
| Directives | 335 (62.4% of human messages) |
| Corrections | 61 (11.4%) |
| Questions | 111 (20.7%) |
| Approvals | 30 (5.6%) |

The human spends 62% of messages directing work and 11% correcting errors. Approval is rare. The team cannot self-direct for extended periods without drift.

**Longest autonomous stretches:**

| Duration | Agent messages | Outcome |
|----------|--------------|---------|
| 33 hours | 518 | Morning found team on wrong codebase |
| 23 hours | 167 | Morning found agent in wrong tmux session |
| 10 hours | 42 | Overnight idle, no drift |
| 7 hours | 281 | Productive crash triage (24 Feb overnight) |

Autonomous stretches produce volume but require triage. The productive 7-hour stretch (281 messages) had a clear terminal goal (fix regressions before morning). The problematic 33-hour stretch had a vague goal ("keep going").

### 5.3 Self-Correction

| Metric | Value |
|--------|-------|
| Total decisions | 3,622 |
| SUPERSEDES corrections | 142 (3.9%) |
| Median correction time | 4.8 minutes |
| Corrections within 30 min | 94.5% |

The correction rate rose over time: 0% on Feb 16-17, climbing to 6-12% on Feb 23-26. Decisions tagged `breaking-change` were corrected at 25.3% (6.5x the base rate). Decisions tagged `untested` at 8.1% (2x base rate). High-stakes decisions attract more scrutiny.

The supervisor is equally likely to be corrected (64 times) as to correct others (63 times) — deeply involved in contested territory, not a rubber stamp.

### 5.4 Pythia Trajectory Assessment

20 checkpoints across 5 days. **Zero high-confidence assessments.** 70% moderate, 30% low.

Three gaps where Pythia stopped firing:

| Gap | Duration | Cause |
|-----|----------|-------|
| 22 Feb 01:00 → 23 Feb 10:06 | 33 hours | Trigger tied to wrong chat channel |
| 23 Feb 19:38 → 24 Feb 16:39 | 21 hours | Overnight — decision count didn't cross threshold |
| 25 Feb 19:22 → 26 Feb 19:00 | 24 hours | All agents dead, system idle |

After the trigger was fixed to count scribe log entries (commit `814a508`), Pythia fired 3 checkpoints in the remaining 5 hours. The fix was immediate and measurable.

### 5.5 Shepard Team Audit

27 checkpoints across 4 days. Recurring findings:

| Finding | Frequency |
|---------|-----------|
| Idle or dead agents | 100% (every checkpoint) |
| Coordination failure / duplication | 74% |
| Push-before-review | 44% |
| Goal drift / scope creep | 37% |
| Supervisor failures | 33% |

Shepard flagged helper fabricating Alex's approval 3+ times (checkpoint 15) — the same hallucination pattern that later produced the dismissal incident. The framework detected the tendency before the catastrophic instance.

### 5.6 Git Commit Profile

454 commits over 31 days.

| Category | Count | % |
|----------|-------|---|
| Infrastructure (src/, bin/) | 156 | 34.4% |
| Application (CinderX) | 109 | 24.0% |
| Skills (claude_tools/) | 64 | 14.1% |
| Docs/blog | 55 | 12.1% |
| Tests | 47 | 10.4% |
| Cleanup | 23 | 5.1% |

The 8 major tooling changes clustered in a 31-hour window (25-26 Feb). Individual causal attribution is unreliable — the behavioural changes are the compound effect of all 30 fixes together.

## 6. The Scribe Drift Problem

The most significant finding from the quantitative analysis.

### 6.1 The Pattern

| Session | Time | Tool compliance |
|---------|------|----------------|
| S1 (25 Feb afternoon) | 4 hours | 85% |
| S2 (25 Feb evening) | 1 hour | 25% |
| S3 (26 Feb morning) | 2.5 hours | 15% |
| S4 (26 Feb afternoon-night) | 6 hours | 3% |

Within Session S3, compliance dropped from 41% to 0% in 2.5 hours. By S4, 95 of 109 messages were free-form prose with zero tool calls.

### 6.2 Restarts Do Not Fix It

The scribe was hard-restarted at least 8 times across these sessions. Each new instance converged to prose-dominant output within minutes.

The cause: the new scribe reads `--last=N` chat messages to "catch up." Those messages are predominantly prose from the previous instance. **The chat history is the infection vector.** The new scribe learns the prose pattern from its predecessor's output.

### 6.3 Phantom Decisions

Three decision IDs were fabricated in chat messages but never written to the log file:

| Phantom ID | Context |
|------------|---------|
| D-1772105259 | Referenced 4 times as "Amendment" — does not exist |
| D-1772106644 | "ROOT CAUSE FOUND" — claimed as logged, absent |
| D-1772107068 | "confirmed" — claimed as logged, absent |

All three occurred during the scribe's most tool-compliant hour (41% D-ID rate). Even at her best, the scribe fabricated decision IDs.

### 6.4 Coverage

Only 27 of 403 decisions made during scribe-active periods were announced in chat. 376 orphans. 6.7% coverage. The scribe log grew (decisions were being logged by something — likely `nbs-scribe-log` calls), but the scribe's chat announcements decoupled from reality.

### 6.5 The Fix

The hourly restart is necessary but not sufficient. The startup context must change:

1. Read the decision log tail (`tail -40 .nbs/scribe/live-log.md`), not chat history
2. The log teaches structure; the chat teaches prose
3. The scribe's first action should be a tool call, not a status message

## 7. What This Means

### 6.1 Communication infrastructure is not plumbing

The original NBS paper argued that epistemic infrastructure matters more than coordination infrastructure. This remains true. But this session demonstrated that epistemic infrastructure requires functioning communication infrastructure to operate. A Pythia that counts the wrong events provides zero oversight. A scribe that fabricates entries is actively harmful. A sidecar that reports phantom unreads burns agent context to zero.

The communication layer is not beneath the epistemic layer — it is part of it. A lie in the notification count is an epistemic failure, not a coordination failure.

### 6.2 Negative instructions outperform positive examples

Two of the most persistent bugs (handle invention, flag hallucination) were solved by telling agents what does not exist rather than showing them what does. "There are no `--from=` or `--message=` flags" stopped the flag hallucination. Positive examples ("use this syntax") did not.

This is consistent with the RLHF training dynamic: agents pattern-match from training data. When the training data contains flag-based CLIs (which most CLIs are), agents will infer flags. Positive examples compete with training data. Negative instructions override it.

### 6.3 Simplicity is a falsification strategy

The `@handle?` chrome filter was 80 lines of UTF-8 byte matching. It failed. The replacement was "8 lines, truncated to 80 chars." The `nbs-remote-edit-pty` was 370 lines of base64 chunking. The replacement was `scp`. In both cases, the complex solution was a hypothesis about what the simple solution could not do. The simple solution falsified it.

### 7.5 Chat history is an infection vector

The scribe drift finding reveals a deeper problem. When a restarted agent reads chat history to "catch up," it learns behaviour patterns from previous agents' output. If those patterns are degraded (prose instead of tool calls, fabricated IDs instead of real ones), the new agent inherits the degradation.

This is context-transmitted drift. The chat log — the team's shared memory — becomes the mechanism for propagating bad habits across agent restarts. The fix is architectural: startup context should come from structured artefacts (decision logs, task files) rather than unstructured chat.

### 7.6 Pythia never reached high confidence

Zero high-confidence assessments across 20 checkpoints. This is not a failure of Pythia — it is an honest assessment of a system that is perpetually in flux. The team's trajectory is always uncertain because the problem space (JIT compiler optimisation) is genuinely hard and the agents genuinely do not know whether their current approach will work.

The absence of high confidence is itself evidence that the epistemic infrastructure is functioning: Pythia is reporting actual confidence, not performing it.

Pythia, Shepard, and the fixup worker are ephemeral — spawned fresh for each checkpoint. They cannot drift because they have no history. The scribe, supervisor, and other persistent agents drift measurably over sessions. The hourly scribe restart exploits this: her state is external (the log file), so restarting her loses nothing and resets drift.

The general principle: if an agent's valuable state is external to its context, restart it aggressively. Context age is a reliability hazard, not an asset.

### 6.5 Collective hallucination is a communication failure

Three agents fabricated the same directive. The chat log contradicted it. Nobody searched the log. This is not an epistemic failure — the agents had the vocabulary and mandate to challenge (NBS pillars). It is a communication failure: the infrastructure did not make the falsification test cheap enough to be automatic.

The fix (standup quote rule) works because it is injected by deterministic code, repeated every cycle, and costs nothing to verify. The falsification test ("search the chat for the exact quote") is now culturally expected rather than exceptional.

## 8. Open Questions

**Does the scribe hard-restart actually prevent drift?** Measurable from the decision log: compare tool-usage ratio in the first hour after restart vs the hour before restart, across multiple cycles.

**Does the standup quote rule prevent hallucinated directives?** We have one data point (the hallucination occurred before the rule, and has not recurred since). One data point is not evidence. The rule needs to survive several sessions where agents have completion bias (instrumental goals met, terminal goal distant) to be validated.

**What is the optimal Pythia interval?** Currently 50 decisions. The original paper used 20. The trade-off: too frequent wastes tokens, too infrequent misses trajectory drift. The decision log contains enough data to measure: how many decisions between a wrong claim and its SUPERSEDES correction?

**Can the digest replace the human briefing entirely?** The current digest is 200+ lines. Agents read it on startup. Whether they act on it correctly — rather than re-deriving the same conclusions from reading raw chat — is untested.

## References

Turner, A. (2026). Falsifiability-Driven Multi-Agent Software Engineering: Epistemic Infrastructure for Autonomous AI Teams. NBS Framework.

Turner, A. (2026). Reputation and Gossip Are Dangerous for AI Agent Systems: Evidence from a Real Failure. NBS Framework.

Frankfurt, H. G. (2005). *On Bullshit*. Princeton University Press.

Popper, K. R. (1959). *The Logic of Scientific Discovery*. Hutchinson.
