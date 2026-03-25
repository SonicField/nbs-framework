---
description: "NBS Teams: Partial recovery after downtime"
allowed-tools: Bash, Read, Write
---

# NBS Teams: Restart (Partial Recovery)

You are performing a **restart** — recovering a multi-agent team after downtime (overnight idle, crash, network partition, or context exhaustion). The `.nbs` infrastructure already exists. You are triaging the entire team, not a single agent.

**Core principle:** Assess before acting. The worst outcome is killing a healthy agent or losing a session that could have been compacted. Triage first, recover in priority order, verify after each recovery.

## When to Use This Runbook

- Morning after overnight — agents may be zombie, dead, or healthy
- After a coordination host crash or reboot
- After extended idle period (>4 hours with no substantive work)
- When multiple agents are unresponsive simultaneously
- After network partition recovery (remote agents)

**Do not use this for:**
- Single agent recovery — use `/nbs-teams-fixup`
- First-time setup — use `/nbs-teams-start`
- Healthy team that just needs a task — post to chat

## Agents vs Infrastructure

Not all participants in chat are agents to be spawned. Distinguish:

| Type | Examples | Spawning |
|------|----------|----------|
| **Team agents** | scribe, gatekeeper, testkeeper, supervisor, helper, generalist, theologian, hypergrep | Spawn via `nbs-claude` / `nbs-workers continue` during restart |
| **Infrastructure** | sidecar, pythia, shepard | **Do NOT spawn during restart.** Sidecars are launched automatically by `nbs-claude` for each agent. Pythia is triggered by sidecar configuration (pythia-interval). Shepard is triggered by sidecar configuration (shepard-interval, every 100 chat messages). |

The `participants` list in chat includes both types. When triaging, skip sidecar, pythia, and shepard — they are not independent agents.

## Process

### Step 1: Inventory and Triage

List all agent sessions and classify each:

```bash
nbs-ts list | grep alive
```

For each session, capture state and context:

```bash
nbs-ts read-new <handle> --strip | tail -20
```

Classify into triage categories:

| Category | Indicators | Action |
|----------|-----------|--------|
| **Healthy** | Spinner active, or recent output, context >25% | Leave alone |
| **Idle but alive** | Prompt visible, context >25%, no recent output | Ping (Level 1) to confirm responsiveness |
| **Context stressed** | Context 15-25%, otherwise functional | Compact (Level 2) proactively |
| **Zombie** | Context <15%, accepts input but no output | Follow /nbs-teams-fixup zombie classification |
| **Dead** | Process exited, bash prompt visible | Hard restart (Level 4) |
| **Missing** | No nbs-ts session at all | Respawn from scratch |

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
4. **Theologian fourth** — architectural guidance should be available before workers start implementation.
5. **Workers/generalist last** — workers depend on all of the above.

**Exception:** If the human has an urgent task, recover the most relevant agent first regardless of this order.

### Step 4: Recover Each Agent

For each agent in recovery order, apply the /nbs-teams-fixup escalation ladder:

```
Level 1 (Ping): nbs-ts send <handle> "", wait 15s
Level 2 (Compact): nbs-ts send <handle> Escape, then /compact, wait 60s
Level 3 (Continue): nbs-workers continue <handle>
Level 4 (Hard restart): nbs-ts kill, respawn fresh
```

**Batch efficiency rules:**
- Start Level 1 pings for all idle-but-alive agents simultaneously — they are independent
- Wait for ping results before escalating any individual agent
- Hard restarts can be batched — kill all dead/zombie sessions, then respawn in order
- Do NOT batch compacts — each compact needs monitoring to assess if it helped

**Context-based shortcuts** (from /nbs-teams-fixup zombie classification):
- Process dead (bash prompt, session exited): skip to Level 4
- Low context (<10%): try Level 2 first (compact costs seconds), escalate to Level 4 if no response within 30s
- Context at compaction floor (10-15% after compact, no improvement): escalate to Level 4
- Session metadata available (`.nbs/sessions/<handle>.json`): use `nbs-workers continue <handle>` for Level 3
- Session started without --resume and no session metadata: skip Level 3

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

This ensures the restart script does not see stale PIDs during respawn.

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

#### Role→Skill Mapping

When spawning agents with `NBS_INITIAL_PROMPT`, use the correct skill for each role:

| Handle | Skill | Initial prompt |
|--------|-------|---------------|
| `supervisor` | `/nbs-supervisor` | `NBS_INITIAL_PROMPT="/nbs-supervisor"` |
| `scribe` | `/nbs-scribe` | `NBS_INITIAL_PROMPT="/nbs-scribe"` |
| `gatekeeper` | `/nbs-gatekeeper` | `NBS_INITIAL_PROMPT="/nbs-gatekeeper"` |
| `testkeeper` | `/nbs-testkeeper` | `NBS_INITIAL_PROMPT="/nbs-testkeeper"` |
| `theologian` | `/nbs-theologian` | `NBS_INITIAL_PROMPT="/nbs-theologian"` |
| Named workers (e.g. `helper`, `generalist`, `hypergrep`) | `/nbs-worker` | `NBS_INITIAL_PROMPT="/nbs-worker"` |

For each agent classified as dead or zombie in Step 1, respawn in the recovery order from Step 3. Use staggered starts.

**If session metadata exists** (agent was started with `nbs-claude` which writes `.nbs/sessions/<handle>.json`), use `nbs-workers continue` to preserve session context:

```bash
# Continue with existing session ID and model from metadata
nbs-workers continue <handle>

# Or override the model on continue
nbs-workers continue <handle> --model=opus

# Inspect metadata before continuing
nbs-workers session <handle>
```

**If no session metadata** (fresh respawn, Level 4):

```bash
# Wait 5 seconds between spawns to reduce lock contention
# Replace <handles> with the dead/zombie agents from triage, in recovery order
for handle in <handles from triage, recovery order>; do
    NBS_HANDLE=${handle} NBS_TRANSPORT=ts \
        setsid .nbs/bin/nbs-claude --root=<project-root> --dangerously-skip-permissions >/dev/null 2>&1 &
    sleep 5
done
```

To specify a model on fresh spawn:

```bash
NBS_HANDLE=<handle> NBS_MODEL=<model> NBS_TRANSPORT=ts \
    setsid .nbs/bin/nbs-claude --root=<project-root> --model=<model> --dangerously-skip-permissions >/dev/null 2>&1 &
```

For agents with custom role prompts, use NBS_INITIAL_PROMPT:

```bash
NBS_HANDLE=<handle> NBS_TRANSPORT=ts \
NBS_INITIAL_PROMPT="Read .nbs/workers/<handle>-skill.md and follow the role instructions." \
    setsid .nbs/bin/nbs-claude --root=<project-root> --dangerously-skip-permissions >/dev/null 2>&1 &
```

### Step 7: Verify Recovery

Wait 30 seconds after the last respawn. Then verify all agents:

**7a. All nbs-ts sessions exist:**

```bash
nbs-ts list | grep alive
```

**7b. All agents posted to chat:**

```bash
nbs-chat participants .nbs/chat/live.chat
```

Each recovered agent should appear with a fresh message (join announcement or standup response).

**7c. Context levels are healthy:**

```bash
for f in .nbs/sessions/*.json; do
    handle=$(basename "$f" .json)
    ts=$(grep -o '"nbs_ts_handle": "[^"]*"' "$f" | cut -d'"' -f4)
    echo "=== $handle ==="
    nbs-ts read-new "$ts" --strip 2>/dev/null | tail -5
done
```

Newly spawned agents should be at ~95% or higher. Compacted agents should be above 25%.

### Step 8: Post Recovery Report

```bash
nbs-chat send .nbs/chat/live.chat <your-handle> "Recovery complete:
- @<handle1>: <recovery method> — now at <N%> context
- @<handle2>: <recovery method> — now at <N%> context
...
Infrastructure: chat OK, bus OK, scribe log OK"
```

### Step 8b: Automated Digest

**This step MUST complete before Step 8c (spawning agents).** Agents read the digest on startup. If it's not there, they miss it.

```bash
nbs-digest-spawn .nbs/chat/live.chat --wait
```

Wait for the digest worker to post its summary to chat. Then reset cursors again (the digest added messages):

```bash
# Reset cursors to current end so agents see the digest + banner
HEADER_LINES=6
total_lines=$(wc -l < .nbs/chat/live.chat)
cursor_value=$((total_lines - HEADER_LINES - 1))
for handle in <all agent handles>; do
    sed -i "s/^${handle}=.*/${handle}=${cursor_value}/" .nbs/chat/live.chat.cursors
done
```

### Step 8c: Spawn Agents

Only after the digest is posted and cursors are reset, spawn agents in recovery order (Step 3). Stagger starts by 5 seconds.

### Step 9: Brief Recovered Agents (optional)

Only needed for information not in the chat:

- Pipeline state (approved but uncommitted changes)
- Urgent requests from the human via a different channel
- External dependencies that changed during downtime

## Morning Checklist (Quick Reference)

For the common case of morning recovery after overnight idle:

```
1. nbs-ts list | grep alive           # Who's alive?
2. For each session: capture-pane, check context %
3. Triage: healthy / stressed / zombie / dead
4. Post triage to chat
5. Clean stale pidfiles and cursors for dead/zombie agents
6. Compact stressed agents (Level 2). If compact does not reduce context, escalate to Level 4
7. Hard-restart zombies and dead agents (Level 4)
8. Wait 30s, verify chat participants
9. Post recovery report
10. nbs-digest-spawn .nbs/chat/live.chat --wait  # MUST complete before step 11
11. Reset cursors to current end of chat
12. Spawn agents (staggered, recovery order)
13. Brief recovered agents (only for info not in chat)
```

## Remote Agent Recovery

For cross-machine deployments, remote agents may have additional failure modes:

1. **Verify SSH connectivity first:**
   ```bash
   ssh <coordination-host> echo "ok"
   ```

2. **If SSH is down:** Remote agents are operating in local-only fallback mode. They will resync cursors on reconnection. Fix SSH first (see cross-machine plan, Section 9).

3. **If SSH is up but agent is zombie:** Apply the same escalation ladder as local agents, but using `nbs-chat-remote` and `nbs-bus-remote` for verification.

4. **Cursor desync after partition:** If a remote agent was disconnected during a network partition, her cursor may be behind. On reconnection, the agent picks up from her last cursor position — no messages are lost, but she may need to process a backlog.

For full cross-machine recovery procedures, see `docs/cross-machine-runbook.md` (Section 10.3).

## Known Failure Patterns

### Correlated overnight zombie

**Symptom:** All idle agents hit 11-12% context simultaneously.
**Cause:** Sidecar's `/nbs-poll` safety net injected ~96 empty cycles overnight (every 300s for 8 hours). All agents consumed context at the same rate.
**Prevention:** Use CSMA/CD standups and conditional `/nbs-notify` instead.
**Recovery:** Batch Level 4 hard restart for all zombies.

### Active agent survives, idle agents die

**Symptom:** The agent doing substantive work is healthy at 30%+ context. All other agents are zombie.
**Cause:** Active work triggers compaction, freeing context. Idle agents accumulate non-compactable poll responses.
**Implication:** Expected behaviour when agents have nothing to do. Give idle agents substantive standup work.

### Gatekeeper zombie blocks pipeline

**Symptom:** Approved changes cannot be committed because gatekeeper is zombie.
**Cause:** Gatekeeper idle overnight, context bled from polling.
**Recovery:** Prioritise gatekeeper recovery. Brief the new instance with pending approvals from chat.

### Claude (supervisor) at low context

**Symptom:** Claude is at 10-15% while other agents are healthy (freshly restarted).
**Cause:** Claude did the recovery work, consuming its own context.
**Recovery:** After all other agents are recovered, compact or restart Claude.

### Stale cursor after hard restart

**Symptom:** Respawned agent's `--unread` and `--since` return empty despite hundreds of new messages.
**Cause:** Level 4 hard restart creates a fresh session but the cursor file persists. The cursor points to the old session's last message position.
**Recovery:** Reset the cursor in the `.cursors` file to the current end of the chat file (see Step 5b).
**Prevention:** Have `nbs-chat send` update the sender's cursor on write, so the first message from a respawned agent self-heals the cursor.

## Rules

- **Triage before acting.** Never kill a session without checking its state first.
- **Infrastructure before agents.** Verify chat, bus, and scribe log before recovering agents.
- **Recovery order matters.** Scribe → gatekeeper → testkeeper → workers.
- **Batch pings, serialise compacts.** Pings are safe in parallel; compacts need individual monitoring.
- **NEVER invent new handle names.** If a stale pidfile exists, clean it (`rm -f .nbs/pids/<handle>.pid`) and retry. Do not append numbers (e.g. `generalist2`). The handle must match the original exactly.
- **Brief recovered agents.** They have no memory — tell them what was happening.
- **Post everything to chat.** Triage, actions, results. This is the institutional memory.
- **Never use AskUserQuestion.** Post questions to chat instead.
