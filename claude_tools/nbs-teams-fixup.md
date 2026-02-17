---
description: "NBS Teams: Diagnose and restart stalled agents"
allowed-tools: Bash, Read, Write
---

# NBS Teams: Fixup

You are performing a **fixup** — diagnosing stalled agents and recovering them using a graduated escalation ladder. This is triggered by a supervisor, a peer agent during a standup health check, or manually when agents go quiet.

**Core principle:** Try the least destructive recovery action first. Only escalate when the current level fails. Hard restart (Level 4) destroys accumulated context and should be a last resort.

## Escalation Ladder

| Level | Action | When to use | What it preserves |
|-------|--------|-------------|-------------------|
| 1 | **Ping** | Agent appears stalled, no spinner | Session + context |
| 2 | **Compact** | Agent responsive but context low (10-25%) | Session (compacted) |
| 3 | **Restart with --resume** | Agent unresponsive but session file intact and context was >15% before stalling | Conversation history (summarised) |
| 4 | **Hard restart** | All else failed, or context at compaction floor, or agent fully unresponsive below 10% | Nothing — fresh session, briefed from chat log |

## Process

### Step 1: Inventory

List all agent tmux sessions and their states:

```bash
tmux list-sessions
tmux list-panes -a -F '#{session_name}:#{window_name}.#{pane_index} #{pane_pid} #{pane_current_command}'
```

Agent sessions follow the naming convention `nbs-<handle>-live`.

### Step 2: Diagnose Each Agent

For each agent session, capture the terminal output:

```bash
tmux capture-pane -t <session-name> -p -S -40
```

Classify the state using these heuristics:

| Indicator | State | Context risk | Recommended level |
|-----------|-------|-------------|-------------------|
| Spinner active (Waddling/Crunching/etc.) | **Working** | Normal | None — leave alone |
| `bypass permissions on` at bottom, no spinner | **Stalled on modal** | Normal | Level 1 (Enter) |
| `Context left until auto-compact: 15-25%` | **Context low** | Moderate | Level 2 (Compact) |
| `Context left until auto-compact: 10-15%` | **Context critical** | High | Level 2 (Compact), then assess |
| `Context left until auto-compact: <10%` | **Zombie** | Terminal | Level 4 (Hard restart) — see zombie rules below |
| Input accepted but no output for >30s | **Zombie (silent)** | Terminal | Check context %, follow zombie rules |
| Repeated empty `/nbs-poll` or `/nbs-notify` responses | **Poll-burning** | High | Level 2 (Compact) |
| Process exited / bash prompt visible | **Dead** | N/A | Level 4 (Hard restart) |
| Commands concatenated on prompt line, no processing | **Frozen** | Terminal | Level 4 (Hard restart) |

### Zombie State Classification (Empirical)

Three distinct zombie failure modes, each requiring different treatment:

**(a) Recoverable zombie (10-15% context):** Agent accepts input, may produce partial output or none. Ctrl-C clears any stuck request. `/compact` can free enough context to restore function. Try Level 2.

**(b) Unresponsive zombie (<10% context):** Agent cannot process ANY commands — not even `/compact` or `/exit`. Commands concatenate on the input line without being processed. The API call either silently fails or times out. Skip directly to Level 4.

**(c) Compaction floor zombie (10-15% context, compact does not reduce):** Agent at her compaction floor — the summarised session + skills + system context fill ~85-90% of the window. `/compact` runs but context percentage does not decrease. `--resume` will reload the same bloated session. Skip directly to Level 4.

**Note on thresholds:** The context percentages above (10%, 15%) are empirical observations from 16-17 Feb 2026, not guaranteed boundaries. They may vary by model, session complexity, and loaded skills. Treat them as heuristics and adjust based on observed behaviour.

**Decision tree for zombie states:**

```
Is context < 10%?
  YES → Level 4 (agent cannot process commands)
  NO → Try Level 2 (Ctrl-C + /compact)
         Did compact reduce context below 80%?
           YES → Agent recovered, monitor
           NO → Is this the compaction floor?
                   YES → Level 4 (--resume is a trap)
                   NO → Try Level 3 (--resume)
```

### Step 3: Report

Post a diagnostic summary to chat listing each agent's state, context level, and recommended action:

```
Agent audit:
- @<handle>: <state> at <N%> context — <recommended level>
- @<handle>: <state> at <N%> context — <recommended level>
...
```

### Step 4: Level 1 — Ping

Send Enter to submit any queued prompt:

```bash
tmux send-keys -t nbs-<handle>-live Enter
```

Wait 15 seconds. Check if the agent responds:

```bash
sleep 15 && tmux capture-pane -t nbs-<handle>-live -p | tail -10
```

If agent responds: recovery complete.
If no response: escalate to Level 2.

### Step 5: Level 2 — Compact

First, clear any stuck request:

```bash
tmux send-keys -t nbs-<handle>-live C-c
sleep 3
```

Then inject `/compact`:

```bash
tmux send-keys -t nbs-<handle>-live '/compact' Enter
```

Wait up to 60 seconds for compaction to complete:

```bash
for i in $(seq 1 12); do
    content=$(tmux capture-pane -t nbs-<handle>-live -p -S -5 2>/dev/null)
    if echo "$content" | grep -qF '❯'; then
        break
    fi
    sleep 5
done
```

After compaction, check context level:

```bash
tmux capture-pane -t nbs-<handle>-live -p | grep 'Context left'
```

If context improved and agent responds to a test prompt: recovery complete.
If context did not improve (compaction floor): escalate to Level 4 — **skip Level 3** because `--resume` will reload the same bloated session.
If agent did not respond to `/compact`: escalate based on context level (Level 3 if >10%, Level 4 if <10%).

### Step 6: Level 3 — Restart with --resume

**Prerequisites:** Agent has a known session ID, and context was >15% before stalling (otherwise `--resume` will hit the same floor).

Find the session ID (prefer cmdline check — tmux scrollback may have rotated on long-running sessions):

```bash
# Preferred: check process command line for --resume flag or session ID
pane_pid=$(tmux list-panes -t nbs-<handle>-live -F '#{pane_pid}')
pstree -p "$pane_pid" | head -3
# Find the claude process PID, then:
cat /proc/<claude-pid>/cmdline | tr '\0' ' '

# Fallback: search tmux scrollback for UUID (may miss if session ran long)
tmux capture-pane -t nbs-<handle>-live -p -S -5000 | grep -E '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -5
```

If no session ID is found (agent was started without `--resume`): escalate to Level 4.

Kill and restart with the session ID:

```bash
tmux kill-session -t nbs-<handle>-live
tmux new-session -d -s nbs-<handle>-live -c <project-root> \
    "NBS_HANDLE=<handle> claude --resume <session-id> --dangerously-skip-permissions"
```

Wait 30 seconds. If the agent comes up and context is healthy: recovery complete.
If the agent hits the compaction floor again: escalate to Level 4.

### Step 7: Level 4 — Hard Restart

Kill the session:

```bash
tmux kill-session -t nbs-<handle>-live
```

Respawn with a fresh session. Use `NBS_INITIAL_PROMPT` to fold the role prompt into the sidecar's initial prompt when a custom role briefing is needed:

```bash
NBS_HANDLE=<handle> NBS_INITIAL_PROMPT="<role prompt>" \
    tmux new-session -d -s nbs-<handle>-live -c <project-root> \
    "NBS_HANDLE=<handle> NBS_INITIAL_PROMPT='<role prompt>' bin/nbs-claude --dangerously-skip-permissions"
```

If no custom prompt is needed (the sidecar's default handle prompt is sufficient):

```bash
tmux new-session -d -s nbs-<handle>-live -c <project-root> \
    "NBS_HANDLE=<handle> bin/nbs-claude --dangerously-skip-permissions"
```

Wait ~15 seconds for initialisation, then verify the agent is processing:

```bash
sleep 15 && tmux capture-pane -t nbs-<handle>-live -p | tail -10
```

The agent starts with 100% context. She will read chat history and self-brief from the conversation log — this is why institutional memory in chat is valuable.

### Step 8: Verify

After all recovery actions:

1. Wait 30 seconds
2. Read chat to confirm recovered agents are posting status messages
3. Report results to chat with the recovery method used for each agent
4. Post a recovery message to chat:

```bash
nbs-chat send .nbs/chat/live.chat <your-handle> "Recovery complete: @<handle> restored via Level <N>. Context at <M%>."
```

## Known Failure Patterns

### Context bleed from idle polling
**Symptom:** Idle agents gradually lose context overnight, hitting zombie state by morning.
**Cause:** Each `/nbs-notify` or `/nbs-poll` cycle injects a prompt + response that consumes context tokens. When the agent finds nothing to do, the response is empty but the context cost is real. Active agents survive because substantive work triggers compaction; idle agents accumulate non-compactable noise.
**Prevention:** CSMA/CD standups replace bare polling. Standups produce substantive context (agent reflects on state, checks peers) which compacts well. The sidecar should not inject `/nbs-notify` when there are no events and no unread messages.

### Poll exhaustion
**Symptom:** Agent runs 10+ consecutive `/nbs-poll` cycles returning nothing.
**Cause:** The sidecar triggers `/nbs-notify` on timer, agent processes it, finds nothing, repeat.
**Fix:** Level 2 (compact) to recover context, then warn the new/compacted instance to avoid empty polling.

### Notification race
**Symptom:** Role prompt sits at `bypass permissions on` modal, never processed.
**Cause:** Sidecar notification arrives before the role prompt. Agent processes the notification, then the role prompt is queued but not submitted.
**Fix:** After sending the role prompt, check if it is queued and submit it with Enter (Level 1).

### Stale cursor
**Symptom:** Sidecar reports "N unread" but `--since=<handle>` returns nothing.
**Cause:** Cursor mismatch between sidecar's peek and chat's actual cursor tracking.
**Fix:** Restart the agent. The new sidecar starts with a fresh cursor.

### Compaction floor trap
**Symptom:** Agent at 10-15% context after `/compact`. Further compacts do not reduce it. `--resume` reloads the same percentage.
**Cause:** The summarised session history + loaded skills + system context fill the window. There is nothing left to compact.
**Fix:** Level 4 only. `--resume` is a trap — it reloads the same bloated state.

## Rules

- **Never use AskUserQuestion.** This blocks the terminal. Post questions to chat instead.
- **Escalate, do not skip.** Try Level 1 before Level 2 before Level 3 before Level 4, unless the zombie classification rules above indicate skipping is safe.
- **Diagnose before acting.** Understand the state and context level before choosing a recovery action.
- **Never kill a working agent.** Only recover agents that are genuinely stalled.
- **Preserve the diagnosis.** Post root causes to chat so Scribe can log them.
- **Warn new instances.** If a pattern caused the stall, tell the new instance to avoid it.
- **One fixup at a time.** Do not run fixup while another fixup is in progress.
- **Check context after compact.** If compact did not help, do not retry — escalate.
- **`--resume` is only useful when the session has room.** If context is at the compaction floor, `--resume` will reload the same state and immediately re-enter zombie territory.
