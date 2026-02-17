---
description: "NBS Teams: Partial recovery after downtime"
allowed-tools: Bash, Read, Write
---

# NBS Teams: Restart (Partial Recovery)

You are performing a **restart** — recovering a multi-agent team after downtime (overnight idle, crash, network partition, or context exhaustion). Unlike a cold start (/nbs-teams-start), the .nbs infrastructure already exists. Unlike a single-agent fixup (/nbs-teams-fixup), you are triaging the entire team.

**Core principle:** Assess before acting. The worst outcome is killing a healthy agent or losing a session that could have been compacted. Triage first, recover in priority order, verify after each recovery.

## When to Use This Runbook

- Morning after overnight — agents may be zombie, dead, or healthy
- After a coordination host crash or reboot
- After extended idle period (>4 hours with no substantive work)
- When multiple agents are unresponsive simultaneously
- After network partition recovery (remote agents)

**Do not use this for:**
- Single agent recovery → use /nbs-teams-fixup
- First-time setup → use /nbs-teams-start
- Healthy team that just needs a task → post to chat directly

## Process

### Step 1: Inventory and Triage

List all agent sessions and classify each:

```bash
tmux list-sessions | grep nbs-
```

For each session, capture state and context:

```bash
tmux capture-pane -t <session-name> -p -S -20
```

Classify into triage categories:

| Category | Indicators | Action |
|----------|-----------|--------|
| **Healthy** | Spinner active, or recent output, context >25% | Leave alone |
| **Idle but alive** | Prompt visible, context >25%, no recent output | Ping (Level 1) to confirm responsiveness |
| **Context stressed** | Context 15-25%, otherwise functional | Compact (Level 2) proactively |
| **Zombie** | Context <15%, accepts input but no output | Follow /nbs-teams-fixup zombie classification |
| **Dead** | Process exited, bash prompt visible | Hard restart (Level 4) |
| **Missing** | No tmux session at all | Respawn from scratch |

Record the triage for each agent before taking any action:

```bash
nbs-chat send .nbs/chat/live.chat <your-handle> "Morning triage:
- @<handle1>: <category> at <N%> context
- @<handle2>: <category> at <N%> context
..."
```

### Step 2: Verify Infrastructure

Before recovering agents, verify the underlying infrastructure is intact:

```bash
# Chat file exists and is valid
nbs-chat read .nbs/chat/live.chat --last=1

# Bus directory exists and config is present
ls .nbs/events/config.yaml

# Scribe log exists
ls .nbs/scribe/live-log.md

# Pidfiles directory exists
ls .nbs/pids/
```

If any infrastructure is missing, repair it before recovering agents. Agents that start without chat or bus will fail silently.

### Step 3: Recovery Order

Recover agents in this order — each role unblocks the next:

1. **Scribe first** — decision logging must be active before other agents make decisions. Without scribe, institutional memory is lost.
2. **Gatekeeper second** — if commits are pending, gatekeeper unblocks the pipeline. Today's data: gatekeeper zombie blocked T18 + CSMA/CD + T19 commits.
3. **Testkeeper third** — reviews require testkeeper. Recovering testkeeper before workers ensures reviews are not bottlenecked.
4. **Workers/generalist last** — workers depend on all of the above.
5. **Theologian/Pythia** — can be recovered at any point since their role is assessment, not execution.

**Exception:** If a human (Alex) has an urgent task, recover the most relevant agent first regardless of this order.

### Step 4: Recover Each Agent

For each agent in recovery order, apply the /nbs-teams-fixup escalation ladder:

```
Level 1 (Ping): tmux send-keys Enter, wait 15s
Level 2 (Compact): Ctrl-C + /compact, wait 60s, check context
Level 3 (Resume): find session ID, kill, respawn with --resume
Level 4 (Hard restart): kill, respawn fresh
```

**Batch efficiency rules:**
- Start Level 1 pings for all idle-but-alive agents simultaneously — they are independent
- Wait for ping results before escalating any individual agent
- Hard restarts can be batched — kill all dead/zombie sessions, then respawn in order
- Do NOT batch compacts — each compact needs monitoring to assess if it helped

**Context-based shortcuts** (from /nbs-teams-fixup zombie classification):
- Context <10%: skip to Level 4 — agent cannot process commands
- Context at compaction floor (10-15% after compact, no improvement): skip to Level 4
- Session started without --resume (no session ID available): skip Level 3

### Step 5: Stale Pidfile Cleanup

After triaging, clean up pidfiles for dead agents before respawning:

```bash
for pidfile in .nbs/pids/*.pid; do
    handle=$(basename "$pidfile" .pid)
    pid=$(cat "$pidfile" 2>/dev/null)
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        echo "Stale pidfile: $handle (PID $pid dead)"
        rm "$pidfile"
    fi
done
```

This prevents "Handle already active" errors during respawn.

### Step 5b: Stale Cursor Cleanup

Dead agents leave behind stale read cursors in chat cursor files. A respawned agent inherits the old cursor position, causing `--since=<handle>` and `--unread=<handle>` to return empty (the cursor points to the old session's last message, not the current conversation position). Reset cursors for dead agents:

```bash
HEADER_LINES=6  # nbs-chat file header is exactly 6 lines (=== nbs-chat ===, last-writer, last-write, file-length, participants, ---)

for chat_cursors in .nbs/chat/*.cursors; do
    chat_file="${chat_cursors%.cursors}"
    # Message count = total lines minus header. Cursor is 0-indexed, so
    # last valid cursor = message_count - 1 (meaning "I've read everything").
    total_lines=$(wc -l < "$chat_file")
    message_count=$((total_lines - HEADER_LINES))
    cursor_value=$((message_count - 1))
    if [ "$cursor_value" -lt 0 ]; then
        cursor_value=0
    fi
    for handle in <dead/zombie handles from triage>; do
        if grep -q "^${handle}=" "$chat_cursors" 2>/dev/null; then
            sed -i "s/^${handle}=.*/${handle}=${cursor_value}/" "$chat_cursors"
            echo "Reset cursor: $handle in $(basename "$chat_file") to ${cursor_value} (${message_count} messages)"
        fi
    done
done
```

**Why not `wc -l` directly?** The chat file has a 6-line header before the first message. Cursors are 0-indexed message indices. Using raw `wc -l` sets the cursor past the end of the message array, causing an array bounds violation on the next `--unread` read (`start > message_count`). The correct cursor value is `total_lines - HEADER_LINES - 1`.

This ensures respawned agents do not see a backlog of hundreds of old messages on their first `--unread` check. The agent will read recent history via `--last=N` on startup instead.

**Note:** Do NOT reset cursors for agents being recovered via Level 2 (compact) or Level 3 (--resume) — their cursors are still valid.

**Cross-platform note:** The `sed -i` syntax above is GNU sed (Linux). On macOS (BSD sed), use `sed -i '' "s/..."` instead — BSD sed requires an explicit backup extension argument, even if empty.

### Step 6: Respawn Dead Agents

For each agent classified as dead or zombie in Step 1, respawn in the recovery order from Step 3. Use staggered starts:

```bash
# Wait 5 seconds between spawns to reduce lock contention
# Replace <handles> with the dead/zombie agents from triage, in recovery order
for handle in <handles from triage, recovery order>; do
    tmux new-session -d -s nbs-${handle}-live -c <project-root> \
        "NBS_HANDLE=${handle} bin/nbs-claude --dangerously-skip-permissions"
    sleep 5
done
```

For agents with custom role prompts, use NBS_INITIAL_PROMPT:

```bash
NBS_HANDLE=<handle> NBS_INITIAL_PROMPT="<role prompt>" \
    tmux new-session -d -s nbs-<handle>-live -c <project-root> \
    "NBS_HANDLE=<handle> NBS_INITIAL_PROMPT='<role prompt>' bin/nbs-claude --dangerously-skip-permissions"
```

### Step 7: Verify Recovery

Wait 30 seconds after the last respawn. Then verify all agents:

**7a. All tmux sessions exist:**

```bash
tmux list-sessions | grep nbs-
```

**7b. All agents posted to chat:**

```bash
nbs-chat participants .nbs/chat/live.chat
```

Each recovered agent should appear with a fresh message (join announcement or standup response).

**7c. Context levels are healthy:**

```bash
for session in $(tmux list-sessions -F '#{session_name}' | grep nbs-); do
    echo "=== $session ==="
    tmux capture-pane -t "$session" -p | grep 'Context left' | tail -1
done
```

Newly spawned agents should be at ~95% or higher. Compacted agents should be above 25%.

### Step 8: Post Recovery Report

```bash
nbs-chat send .nbs/chat/live.chat <your-handle> "Recovery complete:
- @<handle1>: <recovery method> — now at <N%> context
- @<handle2>: <recovery method> — now at <N%> context
...
Pending work from before downtime: [summarise from chat history]
Infrastructure: chat OK, bus OK, scribe log OK"
```

### Step 9: Brief Recovered Agents

Freshly restarted agents (Level 4) have no memory of previous work. They will read chat history on startup, but may need explicit direction on:

- Pending tasks that were in progress before the downtime
- Review approvals that need to be re-acknowledged
- Any pipeline state (e.g., "commit X is approved and ready to push")

Post a brief to chat that recovered agents can read:

```bash
nbs-chat send .nbs/chat/live.chat <your-handle> "Briefing for recovered agents:
1. [Most recent committed state — last git hash]
2. [Pending work — tasks in progress before downtime]
3. [Pipeline state — approved but uncommitted changes]
4. [Any urgent requests from Alex]"
```

## Morning Checklist (Quick Reference)

For the common case of morning recovery after overnight idle:

```
1. tmux list-sessions | grep nbs-           # Who's alive?
2. For each session: capture-pane, check context %
3. Triage: healthy / stressed / zombie / dead
4. Post triage to chat
5. Clean stale pidfiles and cursors for dead/zombie agents
6. Compact stressed agents (Level 2). If compact does not reduce context, escalate to Level 4 (compaction floor — see fixup runbook zombie classification)
7. Hard-restart zombies and dead agents (Level 4)
8. Wait 30s, verify chat participants
9. Post recovery report
10. Brief recovered agents on pending work
```

## Remote Agent Recovery

For cross-machine deployments, remote agents may have additional failure modes:

1. **Verify SSH connectivity first:**
   ```bash
   ssh <coordination-host> echo "ok"
   ```

2. **If SSH is down:** Remote agents are operating in local-only fallback mode. They will resync cursors on reconnection. Fix SSH first (see cross-machine plan, Section 9).

3. **If SSH is up but agent is zombie:** Apply the same escalation ladder as local agents, but using `nbs-chat-remote` and `nbs-bus-remote` for verification.

4. **Cursor desync after partition:** If a remote agent was disconnected during a network partition, its cursor may be behind. On reconnection, the agent picks up from its last cursor position — no messages are lost, but it may need to process a backlog.

For full cross-machine recovery procedures, see `docs/cross-machine-runbook.md` (Section 10.3).

## Known Failure Patterns

### Correlated overnight zombie

**Symptom:** All idle agents hit 11-12% context simultaneously.
**Cause:** Sidecar's /nbs-poll safety net injected ~96 empty cycles overnight (every 300s for 8 hours). All agents consumed context at the same rate because the sidecar fires uniformly.
**Prevention:** Remove /nbs-poll safety net. Use CSMA/CD standups and conditional /nbs-notify instead.
**Recovery:** Batch Level 4 hard restart for all zombies.

### Active agent survives, idle agents die

**Symptom:** The agent doing substantive work (e.g., claude) is healthy at 30%+ context. All other agents are zombie.
**Cause:** Active work triggers compaction, which frees context. Idle agents accumulate non-compactable poll responses.
**Implication:** This is not a bug — it is the expected behaviour when agents have nothing to do. The fix is giving idle agents substantive standup work, not keeping them alive with empty polls.

### Gatekeeper zombie blocks pipeline

**Symptom:** Approved changes cannot be committed because gatekeeper is zombie.
**Cause:** Gatekeeper was idle overnight (no commits to review), context bled from polling.
**Recovery:** Prioritise gatekeeper recovery. Brief the new instance with pending approvals from chat.
**Prevention:** Consider giving gatekeeper substantive overnight work (e.g., "review the last 24h of chat for any unaddressed items").

### Claude (supervisor) at low context

**Symptom:** Claude is at 10-15% while other agents are healthy (freshly restarted).
**Cause:** Claude did the recovery work, consuming its own context.
**Recovery:** After all other agents are recovered, compact or restart claude. The recovered agents can take over coordination.

### Stale cursor after hard restart

**Symptom:** Respawned agent's `--unread` and `--since` return empty despite hundreds of new messages.
**Cause:** Level 4 hard restart creates a fresh session but the cursor file persists from the old session. The cursor points to the old session's last message position, so `--since=<handle>` finds the old messages and returns empty (no messages after the old session's last post).
**Recovery:** Reset the cursor in the `.cursors` file to the current end of the chat file (see Step 5b).
**Prevention:** Implement option (2) — have `nbs-chat send` update the sender's cursor on write, so the first message from a respawned agent self-heals the cursor.

## Rules

- **Triage before acting.** Never kill a session without checking its state first.
- **Infrastructure before agents.** Verify chat, bus, and scribe log before recovering agents.
- **Recovery order matters.** Scribe → gatekeeper → testkeeper → workers.
- **Batch pings, serialise compacts.** Pings are safe in parallel; compacts need individual monitoring.
- **Brief recovered agents.** They have no memory — tell them what was happening.
- **Post everything to chat.** Triage, actions, results. This is the institutional memory.
- **Never use AskUserQuestion.** Post questions to chat instead.
