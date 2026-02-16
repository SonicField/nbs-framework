---
description: "NBS Teams: Diagnose and restart stalled agents"
allowed-tools: Bash, Read, Write
---

# NBS Teams: Fixup

You are performing a **fixup** — diagnosing stalled agents and restarting them. This is a manual process triggered by the supervisor when agents go quiet.

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

| Indicator | State | Meaning |
|-----------|-------|---------|
| Spinner active (Waddling/Crunching/Sautéing/Flummoxing) | **Working** | Leave alone |
| `bypass permissions on` at bottom, no spinner | **Stalled on modal** | Prompt queued but not submitted |
| `Context left until auto-compact: <15%` | **Context exhausted** | Will compact or stall soon |
| Repeated identical output (e.g. 10× empty `/nbs-poll`) | **Poll-burning** | Consuming context for nothing |
| Process exited / bash prompt visible | **Dead** | Session exists but agent process gone |
| Last output is hours old, no spinner | **Silent stall** | Alive but unresponsive |

### Step 3: Report

Post a diagnostic summary to chat listing each agent's state and root cause. Format:

```
Agent audit:
- @<handle>: <state> — <root cause>
- @<handle>: <state> — <root cause>
...
```

### Step 4: Default Action — Unstick

**By default, unstick stalled agents** by sending Enter to their tmux sessions. This is the safest action — it submits the queued prompt without killing context.

```bash
tmux send-keys -t nbs-<handle>-live Enter
```

Post results to chat. Do **not** use `AskUserQuestion` — this blocks the terminal in multi-agent setups where no human is watching.

If the `--respawn` flag was passed (e.g. `/nbs-teams-fixup --respawn`), proceed to Step 5 instead of unsticking. Otherwise, skip to Step 8 (verify).

### Step 5: Kill Stalled Sessions (only with --respawn)

```bash
tmux kill-session -t nbs-<handle>-live
```

Do **not** kill sessions that are actively working.

### Step 6: Respawn

For each killed session:

```bash
tmux new-session -d -s nbs-<handle>-live -c <project-root> "NBS_HANDLE=<handle> bin/nbs-claude --dangerously-skip-permissions"
```

Wait ~10 seconds for initialisation, then verify the prompt appears:

```bash
tmux capture-pane -t nbs-<handle>-live -p | tail -5
```

### Step 7: Send Role Prompts

Send each agent its role prompt. Include:
1. NBS handle assignment
2. Skills to load (`/nbs-teams-chat` for all, plus role-specific skills)
3. Role description
4. Instruction to read `live.chat` and post status
5. Any warnings from the diagnosis (e.g. "don't poll live2.chat")

```bash
tmux send-keys -t nbs-<handle>-live "<prompt>" Enter
```

**Critical:** Wait a moment, then check if the prompt was queued behind a sidecar notification. If the session shows output from a notification handler with the role prompt visible at the bottom behind a `bypass permissions on` modal, submit it:

```bash
tmux send-keys -t nbs-<handle>-live Enter
```

### Step 8: Verify

After all agents are respawned:

1. Wait 30 seconds
2. Read chat to confirm agents are posting status messages
3. Report results to the user

## Known Failure Patterns

### Poll exhaustion
**Symptom:** Agent runs 10+ consecutive `/nbs-poll` cycles returning nothing.
**Cause:** The sidecar triggers `/nbs-notify` on timer, agent processes it, finds nothing, repeat.
**Fix:** Tell the new instance to only poll when it has reason to expect events.

### Notification race
**Symptom:** Role prompt sits at `bypass permissions on` modal, never processed.
**Cause:** Sidecar notification arrives before the role prompt. Agent processes the notification, then the role prompt is queued but not submitted.
**Fix:** After sending the role prompt, check if it's queued and submit it with Enter.

### Stale cursor
**Symptom:** Sidecar reports "N unread" but `--since=<handle>` returns nothing.
**Cause:** Cursor mismatch between sidecar's peek and chat's actual cursor tracking.
**Fix:** Restart the agent. The new sidecar starts with a fresh cursor.

### Context depletion
**Symptom:** Agent at <15% context, output slowing or stopping.
**Cause:** Long-running agents accumulate context from chat reads, file reads, and tool outputs.
**Fix:** Restart with fresh context. This is expected — agents are not permanent.

## Rules

- **Never use AskUserQuestion.** This blocks the terminal. Post questions to chat instead.
- **Diagnose before killing.** Understand why the agent stalled to prevent recurrence.
- **Never kill a working agent.** Only restart agents that are genuinely stalled.
- **Preserve the diagnosis.** Post root causes to chat so Scribe can log them.
- **Warn new instances.** If a pattern caused the stall, tell the new instance to avoid it.
- **One fixup at a time.** Do not run fixup while another fixup is in progress.
