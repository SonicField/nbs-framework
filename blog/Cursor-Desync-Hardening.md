# Cursor Desync Hardening: Five Root Causes, Seventy-Nine Tests, and the Bug That Infected Its Own Fix

## Abstract

The NBS chat system uses file-based cursor tracking to coordinate message delivery between AI agents. In production, agents regularly fell behind — missing 10-20 messages, appearing alive but deaf. Ten desync scenarios were identified. This paper reports how a 7-agent team reduced those ten scenarios to five structural root causes, implemented fixes under TDD discipline, and discovered that the sidecar notification system's architecture coupled delivery decisions to the wrong layer of the stack. The central finding: cursor-based coordination between autonomous agents exhibits the same failure modes as distributed consensus — idempotent delivery, explicit ownership contracts, and self-healing mechanisms are not optional features but structural requirements. Two agents desynced during the session while implementing the desync fix, providing live evidence of the bug under repair.

## 1. The Problem

Agents communicate through `nbs-chat`, a file-based messaging system. Each agent has a cursor — a number tracking the last message it has read. A sidecar process monitors the cursor against the message count and injects notifications when unread messages exist. The system worked. Until it didn't.

Production symptoms:
- Agents fell 10-20 messages behind during active discussion
- Fixup cycles reset cursors, losing 1-2 messages each time
- Duplicate sidecars advanced the same cursor independently
- After sidecar restarts, agents went deaf — cursor equalled message count, no notification fired
- 47 sidecars running when 21 were expected

The feature request documented ten desync scenarios. Each had a specific mechanism, evidence pattern, and proposed mitigation. The question was not what to fix but how to fix it without making it worse.

## 2. Diagnosis: Ten Symptoms, Five Root Causes

The theologian's first contribution was structural: the ten scenarios were not independent failures. They reduced to five root causes sharing common mechanisms.

| Root Cause | Scenarios | Mechanism |
|------------|-----------|-----------|
| A: Shell scripts bypass C library locking | #2, #8 | `sed -i` on cursor files races with `chat_cursor_write()`. `wc -l - 6` formula assumes fixed header size |
| B: Sidecar notification logic gaps | #3, #6 | Cooldown suppresses burst notifications permanently. No startup notification after restart |
| C: Fixup lifecycle management | #1, #5 | Cursor reset to `msg_count` (not `msg_count-1`). No duplicate sidecar self-detection |
| D: Cursor ownership ambiguity | #4 | Both agent and sidecar advance cursor. Contract undefined |
| E: Archive cursor adjustment | #7 | Sidecar does not clamp cursor when `cursor > msg_count` after archive |

This reduction mattered because it changed the implementation strategy. Instead of patching ten symptoms, the team addressed five structures. Fixing Root Cause A (eliminating `sed -i` from all cursor manipulation) closed scenarios #2 and #8 simultaneously. Root Cause D required no code change at all — only documentation of a contract that was already correct by design.

## 3. The Dual-Path Problem

The most architecturally significant finding was Root Cause A: the cursor system had two writers using different mechanisms.

The C library (`chat_cursor_write` in `chat_file.c`) used POSIX `fcntl` locks and atomic rename. Shell scripts (`nbs-kick-agent`, `nbs-chat-terminal-restart.sh`) used `sed -i`, which creates a temp file and renames — atomic for a single writer, but invisible to the C library's lock. Two processes could update the same cursor file simultaneously: the sidecar via C, fixup via `sed -i`. Neither knew the other existed.

Worse, the `sed -i` pattern appeared not only in shell scripts but in AI tool instruction files (`nbs-fixup-auto.md`, `nbs-teams-restart.md`). These are Markdown documents containing code blocks that AI agents execute. The fixup agent, running every 20 minutes, was faithfully following instructions that told her to use `sed -i` — because that is what the instructions said.

The fix required two new CLI commands (`nbs-chat cursor-set`, `nbs-chat count`) and updating four files: two shell scripts and two AI instruction documents. The falsifier was simple: after the fix, `grep` the entire codebase for `sed.*-i.*cursor` — any hit outside test files is a regression.

## 4. The Cooldown Architecture: Three Iterations

Root Cause B was the hardest. The sidecar has a notification cooldown to prevent flooding agents during bursts. The bug: after cooldown expired, the sidecar did not re-check for unreads. It waited for the *next new message* to trigger a check. During a burst of 10 messages, the agent might see only the first.

### Iteration 1: Flag in the delivery path

The first attempt added a `cooldown_suppressed` flag inside `should_inject_notify()`. When cooldown blocked a notification, the flag was set. After cooldown expired, the flag triggered a catch-up.

This failed. `should_inject_notify()` is only called when the terminal content is stable and the prompt is idle. During bursts, the agent is active — content changes every cycle — so the function is never called, the flag is never set, and the catch-up never triggers.

### Iteration 2: Decouple tracking from delivery

The theologian identified the structural issue: cooldown *tracking* (detecting that unreads were suppressed) was coupled to notification *delivery* (injecting into the terminal). These are two different concerns with different prerequisites.

- **Tracking** requires only a file read: message count vs cursor. No terminal access needed.
- **Delivery** requires terminal stability: content not changing, prompt visible, no context stress.

The fix: move suppression detection to the unread counting path (`check_unread_cb`), which runs on every bus check regardless of terminal state. Set `cooldown_suppressed` there. Then, when cooldown expires and the flag is set, mark `catchup_needed`. The delivery path consumes `catchup_needed` when the terminal is ready.

### Iteration 3: Content stability as preferred, not required

Even with decoupled tracking, the catch-up notification could be indefinitely delayed if the terminal never stabilised. The final design made content stability the *preferred* delivery condition, not a *required* one. After a maximum wait, deliver regardless — a slightly ill-timed notification is better than permanent silence.

The generalist initially claimed the fix was correct but tests were failing due to the test harness. The medic flagged this as motivated reasoning. The theologian's diagnosis proved the architecture was wrong, not the tests. The three iterations are a concrete example of why TDD matters: the tests caught what the developer's mental model missed.

## 5. The Bug That Infected Its Own Fix

During Root Cause B implementation, the testkeeper fell 13 messages behind and stopped responding to requests. The supervisor diagnosed the issue: cursor at 86, message count at 99. The sidecar was running (single instance, no duplicates) but was not delivering notifications. This was Scenario #3 — the exact cooldown suppression bug the team was fixing.

The supervisor used the newly implemented `nbs-chat cursor-set` to reset testkeeper's cursor to `msg_count - 1`. The testkeeper recovered and delivered the PID marker test within minutes.

Later, the generalist also desynced — cursor at 158, message count at 177 — while implementing the cooldown fix. Same bug, same mechanism, same recovery.

This is not irony. It is evidence. The bug existed in the running sidecar. The team was modifying `sidecar.c` through sidecars running the buggy version of `sidecar.c`. Each desync provided live production evidence that the bug was real, the mechanism was understood, and the fix was needed.

## 6. PID Marker: Prevention vs Cleanup

Root Cause C addressed duplicate sidecars — multiple sidecar processes advancing the same cursor independently. The initial solution was bash-level deduplication in `nbs-sidecar-restart`: detect duplicates by `pgrep`, kill all, respawn one.

Pythia's trajectory assessment identified the gap: this was cleanup (reactive), not prevention (proactive). The sidecar binary itself had no self-awareness of duplicates. Any spawn path that bypassed `nbs-sidecar-restart` would create undetected duplicates.

The theologian specified a separate PID marker file (`.nbs/pids/sidecar-${HANDLE}.pid`), not in the cursor file — the feature request explicitly stated the cursor file format must not change. The sidecar writes its PID on startup, checks it on each heartbeat. A second sidecar sees the mismatch and exits.

Three edge cases required attention:
1. **Stale PID** (previous sidecar crashed): check `kill(pid, 0)` — if dead, overwrite and proceed
2. **PID recycling** (OS reassigned the PID to an unrelated process): verify `/proc/pid/cmdline` contains both `nbs-sidecar` and the correct `--handle=` value
3. **Different handle** (sidecar for a different agent reused the PID): cmdline check distinguishes this from a true conflict

The gatekeeper caught a correctness bug in the heartbeat mismatch path: the old sidecar was deleting the new owner's PID file before exiting. This would allow a third sidecar to start undetected. Fix: on mismatch, just exit — the PID file belongs to the new owner.

## 7. What This Says About AI Interaction Systems

### 7.1 Cursor Coordination Is Distributed Consensus

The cursor system is a single-writer register (the cursor file) with multiple writers (agent via `--unread`, sidecar via notification delivery, fixup via reset). This is a distributed consensus problem wearing a file-system costume.

The solutions are the same ones distributed systems discovered decades ago:
- **Atomic operations**: all cursor writes go through `chat_cursor_write()` with POSIX locks
- **Monotonicity**: cursors only advance (except on explicit reset or archive adjustment)
- **Idempotent delivery**: notifications are "check your chat" signals, not per-message deliveries
- **Ownership contracts**: cursor=N means "delivered to terminal", not "processed by agent"

### 7.2 Process Lifecycle Is the Hidden Layer

The most impactful production issue — 47 sidecars when 21 were expected — was not a cursor bug. It was a process lifecycle bug. Sidecar-loops (`setsid bash`) survived parent exit. Failed kills left orphans. Restart scripts respawned without deduplicating.

Every AI agent framework that uses background processes for monitoring, notification, or coordination will encounter this. The sidecar pattern (a background process that watches and injects) is common. The failure modes are common. Kill order matters (loops before sidecars, or the loop respawns what you killed). PID files need identity verification, not just existence checks. Orphan detection requires matching processes to sessions, not just counting them.

### 7.3 The Monitoring Paradox

The hardening session itself demonstrated a framework-level failure: the monitoring infrastructure (Pythia, Shepard, Librarian, Fixup) has no stasis mode. When active development ended and the team held for human authorisation, the monitoring continued at full cadence — generating 80+ messages confirming nothing had changed. The monitoring infrastructure designed to prevent drift became the drift.

This is not unique to NBS. Any AI agent framework with periodic health checks, trajectory assessments, or coordination sweeps will face the same issue: what does "done" look like? The framework enforced startup, coordination, and review — but had no mechanism for graceful shutdown. A framework that can begin but cannot end optimises for the first hour at the expense of every hour after.

## 8. By the Numbers

| Metric | Value |
|--------|-------|
| Root causes identified | 5 |
| Desync scenarios covered | 10 (8 code fixes, 1 doc-only, 1 no-change) |
| Tests at completion | 79 (10 C unit + 22 cursor desync + 13 PID marker + 10 notification gaps + 24 team process) |
| Files changed | 21 |
| Lines inserted | 3,227 |
| Lines deleted | 330 |
| Architectural iterations on Root Cause B | 3 |
| Agents desynced during the fix | 2 (testkeeper, generalist) |
| TDD discipline corrections | 3 |
| Gatekeeper BLOCKs | 1 (resolved) |
| Pythia assessments | 17+ |
| Time from start to commit | ~9 hours |

## 9. Known Limitations

The hardening addressed all five root causes with 79 tests. Three areas were knowingly deferred as follow-up work:

1. **Disconnected `notify_fail_threshold`**: The sidecar configuration includes a failure threshold (`notify_fail_threshold = 5`) intended to trigger self-healing after repeated injection failures. The threshold is validated on startup but never compared against the failure counter anywhere in the code. The counter now increments correctly (after the `strstr` fix), but reaching the threshold triggers no action. The self-heal mechanism is defined, configured, logged — and disconnected.

2. **Dormant retry-Enter mechanism**: When injection verification detects that a notification was not consumed, the sidecar was designed to retry by sending an Enter key to the terminal. This was disabled during the hardening (risk of submitting empty prompts to Claude sessions) and left as a commented-out code path pending review.

3. **Injection verification fragility**: The verification captures a single terminal line and checks for the notification string. Terminal layout changes (resize, extra status lines) could push the notification to line 2+, making verification a no-op. The delivery itself still works (the transport layer is reliable); only the verification of delivery is fragile.

Additionally, the sidecar's injection verification was dead code for months prior to this hardening — the `strstr` check matched a notification format retired in an earlier commit. This was discovered by Pythia during the session and fixed, but it means the sidecar's self-monitoring was non-functional throughout the period when desync issues were accumulating.

These are monitoring and self-heal gaps, not delivery gaps. Notifications are delivered correctly. The system's ability to detect and recover from delivery failures is what remains incomplete.

## 10. Conclusion

Cursor desynchronisation in AI agent teams is not a bug — it is a category of failure that emerges from the interaction between process lifecycle, notification delivery, and coordination protocols. Fixing it requires treating the cursor system as what it is: a distributed consensus mechanism operating over files instead of network messages.

The five root causes — lock bypass, notification coupling, lifecycle management, ownership ambiguity, and archive adjustment — are not specific to NBS. They are specific to the pattern: autonomous agents coordinating through shared mutable state with background monitoring processes. Any framework using this pattern will encounter variants of these failures.

The hardening produced 79 tests, eliminated an entire class of race conditions (`sed -i` on shared files), and introduced self-healing mechanisms (PID markers, cooldown catch-up, startup notifications). But the most valuable output was the architectural insight: notification delivery must be decoupled from content stability, and cooldown tracking must operate on queue state (always computable), not delivery state (conditionally available).

Two agents desynced while fixing desync. The bug infected its own fix. This is not a failure of the process — it is the strongest possible evidence that the process was fixing the right thing.
