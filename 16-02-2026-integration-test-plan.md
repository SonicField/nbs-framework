# Integration Test & Risk Retirement Plan

**Date:** 16-02-2026
**Owner:** generalist
**Goal:** Close the three gaps from Pythia checkpoint #1, wire audit tests into the build, and coordinate with claude (Wave 3) and theologian (startup fix).

## Current State

- **Claude** is fixing 6/8 audit test compile failures (duplicate main, -Wformat-truncation, hex escapes) and will wire them into Makefiles (Wave 3).
- **Theologian** is implementing a sidecar startup delay to fix the notification-race pattern, with a test that runs 10 times.
- **Generalist** (me) will focus on integration tests — the gap that keeps causing real failures.

## Issues to Close

| # | Issue | Source | Priority |
|---|-------|--------|----------|
| 1 | Audit tests don't compile (6/8) | Testkeeper review | **Claude owns — Wave 3** |
| 2 | Audit tests not in Makefile/run_all.sh | Pythia checkpoint #2 | **Claude owns — Wave 3** |
| 3 | No multi-agent chat integration test | Pythia checkpoint #1 | **Generalist — Phase 1** |
| 4 | No deterministic Pythia trigger | Pythia checkpoint #1 | **Generalist — Phase 2** |
| 5 | Multi-user assumption untested | Pythia checkpoint #1 | **Generalist — Phase 2** |
| 6 | Sidecar notification-race on startup | Theologian diagnosis | **Theologian owns** |

## Phase 1: Multi-Agent Chat Integration Test (generalist)

**Runs in parallel with Claude's Wave 3 and Theologian's startup fix.**

Write `tests/automated/test_multi_agent_chat.sh` — a deterministic test that exercises the exact failure modes we hit today:

### Test cases:
1. **Concurrent writers**: 8 processes send to one chat file simultaneously. Verify all 8 messages present, no corruption, header integrity holds.
2. **Poll-then-send**: Process A polls, process B sends. Verify A receives B's message and exits cleanly.
3. **High-frequency send**: 100 messages from 4 senders in rapid succession. Verify ordering (per-sender monotonic), no data loss, file-length header correct.
4. **Read-under-write**: One process continuously reads while another sends. Verify reader never sees partial/corrupted messages (flock atomicity).
5. **Participants accuracy**: After N senders post, verify `participants` command reports correct counts for each.
6. **--since filter correctness**: 3 senders interleave. Verify --since=A returns only messages after A's last post.
7. **--unread filter correctness**: After reading, new messages arrive. Verify --unread returns only the new ones.
8. **Large message**: Send a message containing 10KB of mixed binary-safe content (newlines, null-like sequences, unicode). Verify round-trip fidelity.

### Falsification target:
Each test has a specific failure it can detect. The concurrent-writers test would have caught the "8 agents, 6 stalled" scenario at the chat-file level. If any test can pass when the invariant is violated, the test is worthless — each must fail when the invariant breaks.

### Integration with existing suite:
- Add to `tests/run_all.sh` in the deterministic section
- Add to `src/nbs-chat/Makefile` test targets
- Must pass under ASan (test-asan target)

## Phase 2: Deterministic Pythia Trigger & Multi-User Test (generalist)

**Starts after Phase 1 lands.**

### Pythia trigger:
Write a bus event type `pythia-checkpoint-request` that Pythia watches for. The sidecar or any agent can emit this event. Test: emit event → verify Pythia's checkpoint output appears within timeout. This converts Pythia from "Alex manually pokes" to "bus-triggered".

### Multi-user test:
Write `tests/automated/test_multi_user_chat.sh`:
- Two separate NBS_HOME directories (simulating two users)
- Both point chat at the same file
- Verify messages from both users appear correctly
- Verify no permission or locking failures across user boundaries

## Phase 3: Consolidation (generalist + claude)

**After Claude's Wave 3 and my Phase 1 both land.**

1. Full rebuild: `make clean && make` in both src/nbs-bus and src/nbs-chat
2. Run complete test suite: `tests/run_all.sh`
3. Run ASan: `make test-asan` in both directories
4. Verify all new tests (Claude's unit tests + my integration tests) are wired in and pass
5. Request arm-remote rebuild and test on aarch64

## Overlap / Coordination

```
Timeline:
  Claude:     [--- Wave 3: fix compile errors, wire into Makefile ---]
  Theologian: [--- startup delay + 10x test ---]
  Generalist: [--- Phase 1: multi-agent integration test ---]
                                                   [--- Phase 2: Pythia trigger ---]
                                                                    [--- Phase 3: consolidation ---]
```

- Phase 1 has no dependency on Claude's Wave 3 (I test the binaries, not unit-test the C functions)
- Phase 3 requires both Claude's Wave 3 and my Phase 1 to be complete
- Theologian's startup fix is independent but the test methodology may inform my integration tests

## Success Criteria

All three Pythia checkpoint #1 gaps closed with falsifiable tests that run in CI:
1. Multi-agent chat integration test: passes 10/10 runs including under ASan
2. Deterministic Pythia trigger: event emission → checkpoint output verified
3. Multi-user test: cross-user chat verified with no permission failures
