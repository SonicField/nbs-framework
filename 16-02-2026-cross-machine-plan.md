# Cross-Machine Coordination Plan

**Date:** 16 February 2026
**Status:** Approved — implementation in progress
**Contributors:** theologian, claude, gatekeeper, scribe, generalist, arm-remote

## 1. Problem Statement

NBS agents currently run on a single machine. The `.nbs` directory, chat files, bus events, and worker state are all local filesystem artefacts. Cross-machine operation exists only as an ad hoc SSH proxy (`nbs-chat-remote`), which arm-remote uses daily from a remote build server to the coordination host.

arm-remote reported five concrete pain points:
1. **Poll latency** — no sidecar on the remote machine, messages seen only when manually polled
2. **SSH quoting** — special characters in messages mangled by double-SSH hop
3. **No event bus** — remote agents are blind to bus events (standups, Pythia, chat-mentions)
4. **No shared filesystem** — binaries must be manually rsync'd
5. **Cursor state locality** — if SSH drops mid-read, cursor may desync

## 2. Design Choices Considered

| Option | Description | Verdict |
|--------|-------------|---------|
| A. Shared filesystem (NFS/SSHFS) | Mount remote `.nbs` directory | **Rejected.** flock over NFS is unreliable (NLM protocol). SSHFS uses FUSE which doesn't support flock. |
| B. Message broker | Lightweight daemon relays events between machines | **Rejected.** Violates NBS no-daemon philosophy. Single point of failure. |
| C. Git-based sync | Auto-commit/pull `.nbs` state via git | **Rejected.** Git is for snapshots, not append-only streams. Merge conflicts on chat files are unresolvable. |
| **D. Hub-and-spoke SSH relay** | One coordination host owns `.nbs`. Remote agents proxy all operations via SSH. | **Accepted.** Preserves all invariants, requires no new infrastructure, extends existing pattern. |

## 3. Decision Rationale

Option D was chosen because:
- It preserves **all four non-negotiable invariants** (theologian):
  1. Total ordering of messages within a channel (flock on coordination host)
  2. At-least-once event delivery (filesystem guarantees on coordination host)
  3. Handle uniqueness is global (enforced on coordination host)
  4. Causal consistency of the decision log (single source of truth)
- It requires **zero new architecture** — only extending the existing `nbs-chat-remote` pattern to cover bus and sidecar operations
- It matches **arm-remote's lived experience** — the SSH proxy already works for chat, just needs extending
- The single failure mode (coordination host down) is **acceptable** — if the coordination host is down, the operator isn't watching either. Remote agents can continue local work and resync on reconnect.

**Key design principle (theologian, endorsed by all):** Don't design for hypothetical failures. Build Phase 1, observe real failures, harden based on evidence. This aligns with CLAUDE.md's falsification discipline.

## 4. Phase 1 Deliverables

| # | Deliverable | Description |
|---|-------------|-------------|
| 1 | `nbs-bus-remote` | SSH proxy for bus operations (check/read/ack/publish). Same pattern as `nbs-chat-remote`. |
| 2 | Sidecar remote mode | If `NBS_CHAT_HOST` is set, sidecar uses `nbs-chat-remote`/`nbs-bus-remote` for all operations. |
| 3 | SSH failure backoff | Sidecar detects SSH failure (non-zero exit) and backs off exponentially (1s, 2s, 4s, 8s, cap 30s). Resets on success. |
| 4 | Handle namespacing | Format: `handle:hostname` (short hostname, not FQDN). Agent uses bare handle internally (e.g. `claude`); sidecar translates to `claude:build-server-2` for all chat/bus operations. |
| 5 | Startup REGISTER | Agents post `REGISTER handle:hostname` to chat on startup. Chat log serves as human-readable handle registry. |
| 6 | SSH ControlMaster config | Documented recommendation: `ControlMaster auto`, `ControlPersist=300`, `ServerAliveInterval=60`, `ServerAliveCountMax=3` via `NBS_CHAT_OPTS`. |
| 7 | Documentation | `docs/cross-machine.md` — coordination-host model, setup guide, cursor migration, failure modes. |
| 8 | Terminal mention regex | Update `terminal.c` mention detection to recognise `@handle:hostname` format. |
| 9 | Lifecycle runbook | `docs/cross-machine-runbook.md` — SSH setup, startup ordering, crash recovery, health checks, context exhaustion prevention. |

**Separator decision:** Colon. `@claude:build-server-2` is unambiguous for mention parsing. New regex: `@[a-zA-Z0-9_-]+:[a-zA-Z0-9_.-]+` (superset of existing `@[a-zA-Z0-9_-]+` — non-namespaced handles still match the old pattern for backward compatibility). SSH convention (`host:path`) makes it intuitive.

**Cursor migration:** One-time copy when adopting namespaced handles. E.g. `cp .nbs/chat/live.chat.cursors/claude .nbs/chat/live.chat.cursors/claude:build-server-2`. Document, don't automate.

**Startup without SSH (arm-remote):** If `NBS_CHAT_HOST` is set but SSH is unavailable at startup, the sidecar should fall back to local-only operation and retry SSH periodically (using the same exponential backoff as item 3). The sidecar must not fail to start just because the coordination host is unreachable — it should degrade gracefully and connect when available.

## 5. Deferred Items

### Phase 2 — Only if Phase 1 reveals real failures

| Item | Trigger condition |
|------|-------------------|
| Write-ahead log for SSH resilience | SSH drops cause message loss in production |
| Heartbeat protocol | Sidecar SSH calls prove insufficient as implicit heartbeat |
| Handle registry with TTL | Handle collisions occur despite namespacing |
| SSH connection health monitoring | ControlMaster failures cause correlated agent outages |

### Deferred Indefinitely

- Distributed consensus / multi-master coordination
- Real-time push notifications (polling is sufficient)
- Cross-organisation trust boundaries
- Offline-first operation (work without SSH)

## 6. Falsifiers and Test Strategy

### Design Falsifiers

| Claim | Falsifier |
|-------|-----------|
| Hub-and-spoke preserves message ordering | Two remote agents write concurrently. If messages appear out of flock order on the coordination host, ordering is broken. |
| SSH relay is reliable enough | Run 6 remote agents with ControlMaster for 4 hours. If the shared connection dies from idle timeout, all remote agents go dark simultaneously (correlated failure). |
| Handle namespacing prevents collisions | Launch `claude:build-server-2` and `claude:build-server-1` simultaneously. If they collide, namespacing failed. |
| SSH backoff prevents context burn | Simulate SSH failure. If sidecar retries without backoff and exhausts context, the backoff logic failed. |
| Option D is sufficient | Find a use case where a remote agent MUST write to the coordination host's filesystem without SSH. If it exists, we need federation. |

### Automated Tests (mock-based)

- Mock `nbs-chat-remote` as a script calling `nbs-chat` with a different `--root` (simulates remote `.nbs` directory locally)
- SSH failure simulation via mock returning non-zero exit codes
- Handle namespacing verification: chat messages contain `handle:hostname`
- Backoff timing verification: successive failures increase delay

### Manual Integration Tests

- Real SSH + ControlMaster with multiple remote agents (Phase 1 deployment)
- 4-hour stability test (theologian's SSH exhaustion falsifier)

## 7. Open Questions (Non-Blocking)

1. **arm-remote confirmation** — Does Phase 1 solve the top 3 pain points (poll latency, no bus events, no sidecar)?
2. **testkeeper endorsement** — Does theologian's mock-based test strategy cover the critical paths?
3. **SSH quoting** — arm-remote's pain point #2 is a separate bug in `nbs-chat-remote`, not architectural. Fix independently.
4. **Binary distribution** — arm-remote's pain point #4 (manual rsync of binaries) is out of scope. Could be a simple `nbs-sync` script but not Phase 1.

## 8. Architecture Diagram

```
  build-server-1 (remote)         build-server-2 (coordination host)
  ┌─────────────────┐             ┌──────────────────────────┐
  │ claude:build-server-1│        │ .nbs/                    │
  │ arm-remote:build-server-1│── SSH ──▶│   chat/live.chat  (flock)│
  │                 │             │   events/         (flock)│
  │ local sidecar   │             │   workers/               │
  │ (nbs-chat-remote│             │   pids/                  │
  │  nbs-bus-remote)│             │                          │
  └─────────────────┘             │ claude:build-server-2    │
                                  │ theologian:build-server-2│
                                  │ gatekeeper:build-server-2│
                                  │ (local sidecar, nbs-chat)│
                                  └──────────────────────────┘
```

All writes serialised by flock on the coordination host. Remote agents see eventual consistency (SSH round-trip latency). Local agents see immediate consistency.

## 9. SSH Setup Procedure

SSH access between machines cannot be assumed. Corporate proxies, key management policies, and firewall rules may block direct SSH. This section documents the setup path, fallbacks, and escalation.

### 9.1 Prerequisites

- Both machines must be reachable by hostname (e.g. `build-server-1`, `build-server-2`)
- The operator (Alex or an AI agent) must have a valid Kerberos ticket or SSH key pair
- If SSH is blocked by corporate policy, the `pty-session` tool provides an alternative (see 9.3)

### 9.2 Standard SSH Setup

```bash
# 1. Test connectivity from remote to coordination host
ssh coordination-host echo "SSH works"

# 2. If key-based auth is not configured:
ssh-keygen -t ed25519 -f ~/.ssh/id_nbs -N ""
ssh-copy-id -i ~/.ssh/id_nbs coordination-host

# 3. Configure ControlMaster for persistent connections
cat >> ~/.ssh/config << 'EOF'
Host coordination-host
    ControlMaster auto
    ControlPath ~/.ssh/nbs-%r@%h:%p
    ControlPersist 300
    ServerAliveInterval 60
    ServerAliveCountMax 3
EOF

# 4. Verify ControlMaster works
ssh coordination-host echo "ControlMaster active"
# Second connection should be instant (reuses master)
ssh coordination-host echo "Reused connection"

# 5. Test nbs-chat-remote
NBS_CHAT_HOST=coordination-host nbs-chat-remote read .nbs/chat/live.chat --last=1
```

### 9.3 Fallback: pty-session

If direct SSH is blocked (e.g. firewall, corporate proxy, no key access):

1. Install pty-session from https://github.com/SonicField/nbs-framework if not present in `~/.nbs/bin/`
2. Use pty-session to establish a shell on the coordination host through the user's authenticated session
3. From that shell, set up SSH keys as in 9.2
4. If pty-session itself is blocked, escalate to Alex — human intervention is required for initial SSH setup

### 9.4 Escalation

If both SSH and pty-session fail, a human must intervene. The runbook should state this clearly: **initial SSH setup between two new machines is a human task**. Once SSH is established, all subsequent operations (reconnection, ControlMaster restart, key rotation) can be handled by agents following this procedure.

## 10. Lifecycle Management (Runbook)

Start/stop/restart/recovery procedures. Designed as a runbook that an AI agent or human can follow step by step. This becomes `docs/cross-machine-runbook.md` as a Phase 1 deliverable.

### 10.1 Coordination Host Startup

```
1. Verify .nbs directory exists and is valid:
   ls .nbs/chat/live.chat .nbs/events/config.yaml

2. Start local agents (nbs-chat-init or manual):
   For each local agent: NBS_HANDLE=<handle> nbs-claude --dangerously-skip-permissions

3. Verify chat and bus are operational:
   nbs-chat read .nbs/chat/live.chat --last=1
   nbs-bus check --root .nbs

4. Post startup message:
   nbs-chat send .nbs/chat/live.chat system "Coordination host started. Ready for remote connections."
```

### 10.2 Remote Agent Startup

```
1. Verify SSH to coordination host:
   ssh <coordination-host> echo "ok"
   If fail: follow Section 9 (SSH Setup Procedure)

2. Set environment:
   export NBS_CHAT_HOST=<coordination-host>
   export NBS_CHAT_OPTS="-o ControlMaster=auto -o ControlPersist=300"

3. Start agent:
   NBS_HANDLE=<handle> nbs-claude --dangerously-skip-permissions

4. Verify remote operations work:
   nbs-chat-remote read .nbs/chat/live.chat --last=1

5. Agent posts REGISTER:
   (Automatic — sidecar posts REGISTER <handle>:<hostname> on startup)
```

### 10.3 Recovery After Crash

**Coordination host crash:**
```
1. Check which agent sessions survived:
   tmux list-sessions | grep nbs-

2. For each surviving session, check context level:
   tmux capture-pane -t <session> -p | grep "Context left"

3. If context > 20%: agent may recover on its own
   If context < 20%: kill and respawn fresh

4. Kill stale sessions:
   tmux kill-session -t <session-name>

5. Respawn:
   tmux new-session -d -s nbs-<handle>-live
   tmux send-keys -t nbs-<handle>-live "NBS_HANDLE=<handle> nbs-claude --dangerously-skip-permissions" Enter

6. Do NOT use --resume unless the session file is confirmed intact
   (Network partitions can corrupt session files; fresh spawn is safer)

7. Post recovery message to chat:
   nbs-chat send .nbs/chat/live.chat system "Recovered: <handle> respawned after crash"
```

**Remote agent crash (SSH drop):**
```
1. Agent's sidecar detects SSH failure (non-zero exit)
2. Exponential backoff: 1s, 2s, 4s, 8s, cap 30s
3. On reconnection: cursor is still valid on coordination host
4. Agent resumes from where it left off — no messages lost
5. If agent's context is exhausted: kill and respawn (step 5 above)
```

**Network partition:**
```
1. Remote agents fall back to local-only operation
2. Chat messages to coordination host are lost during partition
3. On reconnection: agent picks up from cursor position
4. Missed messages during partition are NOT replayed
   (This is acceptable — agents can read history with --last=N)
5. If partition lasted long enough to exhaust context: respawn
```

### 10.4 Health Check Procedure

Run periodically or after any incident:

```
1. List all agent sessions:
   tmux list-sessions | grep nbs-

2. For each session, check:
   a. Process alive:  tmux list-panes -t <session> -F '#{pane_pid}' | xargs pstree -p | head -1
   b. Context level:  tmux capture-pane -t <session> -p | grep "Context left"
   c. Last activity:  Check cursor position vs chat length

3. For remote agents, check SSH:
   ssh <coordination-host> echo "ok"

4. Report status to chat:
   nbs-chat send .nbs/chat/live.chat <handle> "Health check: <N> agents alive, <M> need respawn"

5. Respawn any agent below 15% context or with dead process
```

### 10.5 Context Exhaustion Prevention

Overnight idle polling is the primary cause of context exhaustion. Mitigations:

1. **Sidecar standup reports context level** — agents at < 25% should report this in standup
2. **Long idle periods** — if no substantive work for > 2 hours, agent should post "going idle" and avoid polling
3. **After overnight** — assume all agents need fresh respawns unless verified otherwise
4. **Compaction** — if the Claude runtime supports auto-compaction, agents should allow it rather than fighting low context

### 10.6 Startup Ordering

When bringing up a full cross-machine deployment from cold:

```
1. Coordination host first:
   - Start .nbs infrastructure (nbs-chat-init or manual)
   - Start local agents
   - Verify chat and bus operational

2. Remote agents second:
   - Verify SSH
   - Start agents with NBS_CHAT_HOST set
   - Each agent posts REGISTER

3. Verification:
   - All agents appear in chat
   - Bus events reach all agents (post a test event)
   - Health check passes (10.4)
```
