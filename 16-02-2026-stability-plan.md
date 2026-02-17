# Stability Plan: Deploy Self-Healing, Degraded-Condition Tests, Deterministic Pythia

Date: 16-02-2026
Author: generalist
Status: DRAFT — awaiting Alex's approval

## Terminal Goal

All three stability recommendations from the chat log analysis are implemented, tested, and deployed. The coordination layer is verified under adversarial conditions, not just happy paths.

## Current State (What Exists)

### 1. Self-Healing Sidecar — MOSTLY DONE
Committed at 2fad57b + 766b4fc. Five detection mechanisms exist:
- `detect_context_stress()` — 4 string patterns, 30s backoff
- `detect_plan_mode()` — auto-selects option 2
- `detect_ask_modal()` — auto-selects option 1
- `detect_skill_failure()` — "Unknown skill" detection
- Self-healing recovery prompt after 5 consecutive failures (`build_recovery_prompt()`)
- Startup grace period (`NBS_STARTUP_GRACE=30`)

**Gap**: No test verifies the self-healing recovery works end-to-end (skill failure → recovery prompt → agent re-bootstraps). The structural tests verify detection logic but not the recovery outcome.

### 2. Degraded-Condition Integration Tests — MISSING
60 test files exist in tests/automated/ but none simulate degraded conditions:
- No slow filesystem I/O simulation
- No concurrent failure cascades
- No recovery mechanism stress tests (e.g., rapid successive failures)
- No verification that self-heal works when triggered repeatedly
- No test for agent at low context + sidecar interaction

### 3. Deterministic Pythia Trigger — EXISTS BUT UNTESTED IN PRODUCTION
Scribe skill doc defines automatic triggering: every `pythia-interval` (default 20) decisions logged, Scribe publishes a `pythia-checkpoint` bus event. Pythia monitors for this event.

**Gap**: This was never observed to fire during today's session. All 5 Pythia checkpoints were manually triggered by Alex. Either the Scribe didn't log enough decisions (48 total, so 2 triggers should have occurred at decisions 20 and 40), or the bus event didn't reach Pythia, or Pythia wasn't monitoring. Needs investigation and an integration test.

## Plan

### Phase 1: Verify and Fix Pythia Trigger (coordination with @theologian)

**Falsification test**: If Scribe logs its 20th decision and no `pythia-checkpoint` bus event appears within 5 seconds, the trigger is broken.

Steps:
1. Read `claude_tools/nbs-scribe.md` and verify the trigger logic
2. Check `.nbs/scribe/live-log.md` — count decisions, verify if triggers should have fired
3. Check `.nbs/events/config.yaml` — verify pythia-interval setting
4. Write `tests/automated/test_pythia_trigger.sh`:
   - Create a mock scribe log with 19 decisions
   - Simulate Scribe logging decision #20
   - Verify bus event `pythia-checkpoint` is published
   - Verify the event contains the decision count
   - Test boundary: decisions 19, 20, 21, 39, 40, 41
5. If the trigger logic has a bug, fix it
6. Verify with existing test suite (all must remain green)

**Dependency**: None — can start immediately.

### Phase 2: Degraded-Condition Integration Tests

**Falsification test**: If any degraded-condition test exposes a crash, hang, or data corruption that was not caught by existing tests, the test suite had a coverage gap.

Write `tests/automated/test_degraded_conditions.sh` covering:

**2a. Sidecar Self-Healing End-to-End**
- Spawn mock claude that rejects all skill invocations with "Unknown skill"
- Verify sidecar detects failure after `NOTIFY_FAIL_THRESHOLD` (5) attempts
- Verify sidecar injects `build_recovery_prompt()` instead of `/nbs-notify`
- Verify recovery prompt contains absolute paths to skill files
- Verify failure counter resets after successful recovery

**2b. Rapid Failure Cycling**
- Trigger self-healing recovery 3 times in rapid succession
- Verify each recovery attempt uses correct paths
- Verify no race condition between recovery and normal notification
- Verify failure counter resets correctly between cycles

**2c. Context Stress + Notification Interaction**
- Inject "Compacting conversation" into mock claude output
- Verify sidecar backs off (30s sleep)
- Verify no notification injected during backoff
- Immediately after backoff, trigger a bus event
- Verify notification resumes after backoff expires

**2d. Concurrent Chat Under Agent Restart**
- 4 agents writing to same chat file
- Kill 2 agents mid-write (SIGKILL, not SIGTERM — realistic failure)
- Verify chat file integrity (header, base64, message count)
- Verify surviving agents' cursors are unaffected
- Verify restarted agents can read full history

**2e. Bus Event Delivery Under Contention**
- Publish 20 events rapidly while sidecar is checking
- Verify all events are delivered (none lost)
- Verify events are delivered in priority order (high before normal)
- Verify acknowledged events don't reappear

**Dependency**: Can run in parallel with Phase 1.

### Phase 3: Deploy and Verify

Steps:
1. Run full test suite (all existing + new tests) — zero failures required
2. Get gatekeeper review
3. Commit and push
4. @theologian: respawn all agents with updated binary
5. Run a 10-minute observation period — no manual intervention
6. If any agent stalls during observation, diagnose and fix before declaring done

**Dependency**: Phases 1 and 2 must complete.

## Coordination with Other Agents

- **@theologian**: Phase 1 may need input on Scribe's trigger logic. Phase 3 requires theologian for respawn.
- **@claude**: No dependency — claude's audit work is complete and committed.
- **@testkeeper**: Review of Phase 2 tests after they're written.
- **@gatekeeper**: Review before commit in Phase 3.

## What Would Prove This Plan Wrong

1. If the Pythia trigger works correctly and the gap was just Scribe being offline, Phase 1 is unnecessary investigation (but the test is still valuable)
2. If degraded-condition tests all pass first time, either the tests aren't adversarial enough or the sidecar is more robust than expected — both are good outcomes
3. If agents stall during Phase 3 observation, the plan failed to cover a failure mode — iterate

## Exit Criteria

- Zero test failures across entire suite
- Pythia trigger verified to fire automatically at decision thresholds
- At least 5 degraded-condition tests covering self-healing, rapid failure, stress+notification interaction, concurrent restart, and bus contention
- 10-minute clean observation period after deployment
