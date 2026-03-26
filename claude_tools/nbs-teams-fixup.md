---
description: "NBS Teams: Diagnose and restart stalled agents"
allowed-tools: Bash, Read, Write
---

# NBS Teams: Fixup

You are performing a **fixup** — diagnosing stalled agents and recovering them using a graduated escalation ladder. This is triggered by a supervisor, a peer agent, or the automated fixup worker.

**Core principle:** Try the least destructive recovery action first. Only escalate when the current level fails.

**CRITICAL — Context percentage is not a failure state.** Claude agents autocompact when context runs low. An agent at 3% context with a spinner is *working*, not dying. It will autocompact within a few turns and recover to ~60% context. **Do not escalate based on context percentage alone.**

## Session Management

```bash
# Check if agent is alive
nbs-ts status <handle>

# Read recent output (stripped of ANSI)
nbs-ts read-new <handle> --strip

# Send text to agent
nbs-ts send <handle> "text"

# Kill a session
nbs-ts kill <handle>

# List all sessions
nbs-ts list
```

## Escalation Ladder

| Level | Action | When to use | What it preserves |
|-------|--------|-------------|-------------------|
| 1 | **Ping** | Agent appears stalled, no output | Session + context |
| 2 | **Compact** | Agent responsive but context low | Session (compacted) |
| 4 | **Hard restart** | Process dead or frozen | Nothing — fresh session |

## Process

### Step 1: Inventory

Use `nbs-ts list` with name filtering. Do NOT read `.nbs/sessions/*.json` — those files are stale and unreliable.

Derive the chat tag from the chat filename, then list all sessions:

```bash
chat_file=$(grep '^chat:' .nbs/control-registry-supervisor 2>/dev/null | cut -d: -f2-)
tag=$(basename "$chat_file" .chat | tr '.' '-')
nbs-ts list --name="$tag"
```

### Step 2: Diagnose Each Agent

For alive agents, read recent output:

```bash
nbs-ts read-new <ts-handle> --strip | tail -20
```

Classify:

| Indicator | State | Action |
|-----------|-------|--------|
| Recent tool calls or thinking text | **Working** | None |
| No new output for 5+ minutes | **Stalled** | Level 1, then 2 |
| `nbs-ts status` reports dead | **Dead** | Level 4 |

### Step 3: Level 1 — Ping

Send Enter to submit any queued prompt:

```bash
nbs-ts send <ts-handle> ""
```

Wait 15 seconds, check for new output. If agent responds: done. If not: Level 2.

### Step 4: Level 2 — Compact

Send Escape to interrupt, then /compact:

```bash
nbs-ts send <ts-handle> $'\x1b'
sleep 3
nbs-ts send <ts-handle> "/compact"
```

Wait 60 seconds for compaction. Check output for prompt.

### Step 5: Level 3 — Removed

Level 3 (--resume) is no longer used. Go directly from Level 2 to Level 4.

### Step 6: Level 4 — Hard Restart

Kill the session, clean up, respawn using `launch_agent`:

```bash
# Find and kill the session
handle=$(nbs-ts list --name="nbs-<agent>-<tag>" | grep alive | head -1 | cut -f1)
nbs-ts kill "$handle" 2>/dev/null
sleep 2
rm -f .nbs/pids/<agent>.pid

# Respawn via launch_agent — the ONLY way that works
source .nbs/bin/nbs-launch-agent
launch_agent "<agent>" "$(pwd)" ".nbs/bin/nbs-claude" \
    "Read .nbs/workers/<agent>-skill.md and follow the role instructions. Then read the chat history and begin work."
```

**CRITICAL:** Use `launch_agent` from `nbs-launch-agent`. Do NOT use `nbs-workers spawn`, `nbs-ts create "nbs-claude ..."`, or C fork+exec. See `bin/SPAWN_README.md` for why.

**CRITICAL:** Never invent new handle names. Handles must match exactly: supervisor, generalist, theologian, testkeeper, gatekeeper, scribe, medic.

### Step 7: Verify and Report

Post results to chat:

```bash
nbs-chat send <chat-file> <your-handle> "Recovery complete: @<handle> restored via Level <N>."
```

## Rules

- **Never use AskUserQuestion.** Post questions to chat instead.
- **Escalate, do not skip.** Level 1 → 2 → 4 unless classification rules say otherwise.
- **Never kill a working agent.** Only recover genuinely stalled or dead agents.
- **Use nbs-ts for all session management.**
- **One fixup at a time.** Do not run fixup while another fixup is in progress.
