---
description: Bootstrap a project for NBS teams in one command
allowed-tools: Write, Bash, Read
---

# NBS Teams: Start (Cold Start)

You are performing a **cold start** — bootstrapping a project from nothing to a running multi-agent team. This creates the `.nbs/` structure, spawns agents with handles, verifies sidecars, and registers chat and bus.

**This is a one-time setup command.** If `.nbs/` already exists, confirm before overwriting.

**Core principle:** Verify after every step. A cold start that skips verification produces silent failures that compound overnight.

## Process

### Step 1: Check for Existing Structure

Before anything, check if `.nbs/` exists:

```bash
ls -la .nbs/ 2>/dev/null
```

**If it exists:**
- Ask: "An `.nbs/` directory already exists. Would you like to add teams structure to it, or is this a fresh start?"
- If fresh start: proceed. If adding: skip directory creation for existing directories.
- **Warning:** Existing `.nbs/` directories contain valuable state (chat history, decision logs, cursor positions). Do not delete without explicit confirmation.

**If it does not exist:** Proceed to Step 2.

### Step 2: Establish Terminal Goal

Ask the user:

> "What is the terminal goal for this project? (One sentence describing what you're trying to achieve)"

Wait for their answer. This grounds all subsequent work.

**Do not proceed without a terminal goal.** If the answer is vague, ask for clarification:
- "Can you be more specific? What would success look like?"
- "What problem are you solving?"

### Step 3: Plan Team Composition

Ask the user how many agents they want to run and what roles are needed. Present the standard roles:

| Role | Handle convention | Purpose |
|------|-------------------|---------|
| Supervisor | `supervisor` or user's handle | Goal-keeper, task delegation, 3Ws |
| Worker | `worker-1`, `worker-2`, or task-specific names | Tactical work on delegated tasks |
| Scribe | `scribe` | Decision logging, institutional memory |
| Gatekeeper | `gatekeeper` | Code review, commit/push via pty-session |
| Testkeeper | `testkeeper` | Test suite ownership, verification |
| Theologian | `theologian` | Architecture, invariant enforcement |

**Minimum viable team:** 1 supervisor (the user's own Claude session) + 1 worker.

**Handle rules:**
- Must match `^[a-zA-Z0-9_-]+$`
- Must be unique across all agents (including remote agents)
- Each agent's tmux session is named `nbs-<handle>-live`

Record the chosen handles. They are needed for Steps 8 and 9.

### Step 4: Create Directory Structure

```bash
mkdir -p .nbs/chat .nbs/events/processed .nbs/scribe .nbs/workers .nbs/pids
```

The `pids/` directory stores pidfiles for handle collision guards — prevents two agents from running with the same handle.

### Step 5: Create Chat Channel

```bash
nbs-chat create .nbs/chat/live.chat
```

This is the primary coordination channel. All agents read and write here. Additional topic-specific channels can be created later.

### Step 6: Create Bus Config

Write `.nbs/events/config.yaml`:

```yaml
dedup-window: 300
ack-timeout: 120
pythia-interval: 20
retention-max-bytes: 16777216
```

| Field | Meaning |
|-------|---------|
| `dedup-window` | Seconds to suppress duplicate events (300 = 5 min) |
| `ack-timeout` | Seconds before unacknowledged events are re-delivered (120 = 2 min) |
| `pythia-interval` | Decision-logged events between Pythia trajectory assessments |
| `retention-max-bytes` | Max size of processed events directory before pruning |

### Step 7: Post Terminal Goal to Chat

```bash
nbs-chat send .nbs/chat/live.chat supervisor "Terminal goal: [user's goal verbatim]"
```

This is the first message in the chat log. Every agent that spawns will read it and orient towards it.

### Step 8: Spawn Agents

For each agent in the team plan (Step 3), spawn a tmux session running `nbs-claude`:

```bash
tmux new-session -d -s nbs-<handle>-live -c <project-root> \
    "NBS_HANDLE=<handle> bin/nbs-claude --dangerously-skip-permissions"
```

**With a custom role prompt** (for specialised roles like scribe, gatekeeper):

```bash
NBS_HANDLE=<handle> NBS_INITIAL_PROMPT="<role prompt>" \
    tmux new-session -d -s nbs-<handle>-live -c <project-root> \
    "NBS_HANDLE=<handle> NBS_INITIAL_PROMPT='<role prompt>' bin/nbs-claude --dangerously-skip-permissions"
```

**Spawn order matters:**
1. Spawn agents one at a time, waiting 5 seconds between each
2. The sidecar needs 30 seconds of startup grace before injecting notifications
3. Do not spawn all agents simultaneously — staggered starts reduce contention on the chat file lock

**Sidecar environment variables** (all optional, with sensible defaults):

| Variable | Default | Purpose |
|----------|---------|---------|
| `NBS_HANDLE` | `claude` | Agent's unique handle |
| `NBS_STANDUP_INTERVAL` | `15` | Minutes between CSMA/CD standup check-ins (0 to disable) |
| `NBS_NOTIFY_COOLDOWN` | `15` | Minimum seconds between `/nbs-notify` injections |
| `NBS_STARTUP_GRACE` | `30` | Seconds after init before allowing notifications |
| `NBS_INITIAL_PROMPT` | handle + chat skill | Custom initial prompt for specialised roles |

**Standup architecture:** The sidecar posts periodic CSMA/CD standups to chat: `@all Check-in: what are you working on? What is blocked? What could we be doing? If idle, find useful work.` This replaces bare `/nbs-poll` injection. The primary benefit is replacing unconditional polling with conditional event notification — `/nbs-notify` only fires when events or unreads exist. Note: overnight with no work, standups may produce repetitive content ('idle, no blockers'). The compaction benefit is secondary; the real win is eliminating the 96 empty poll cycles per agent per night that cause context rot.

### Step 9: Verify Agents Are Alive

Wait 30 seconds after the last agent is spawned. Then verify:

**9a. Tmux sessions exist:**

```bash
tmux list-sessions | grep nbs-
```

Each agent should appear as `nbs-<handle>-live`. If a session is missing, spawning failed — check `<project-root>/.nbs/nbs-claude-*.log` for errors.

**9b. Agents posted to chat:**

```bash
nbs-chat participants .nbs/chat/live.chat
```

Each agent should appear with at least 1 message (their initial "Hello" from loading `/nbs-teams-chat`).

**Falsifier:** If an agent's handle appears in tmux but not in chat participants after 60 seconds, the sidecar failed to inject the initial prompt. Diagnose:

```bash
tmux capture-pane -t nbs-<handle>-live -p -S -20
```

Common causes:
- Claude's prompt (`>`) did not appear within 60 seconds (slow startup)
- Permissions modal blocked the initial prompt
- Handle collision with an existing agent

**9c. Pidfiles exist:**

```bash
ls .nbs/pids/
```

Each agent should have a `<handle>.pid` file. If a pidfile exists but the PID inside it does not match a running process, the previous agent crashed without cleanup.

### Step 10: Post Team Roster to Chat

Once all agents are verified:

```bash
nbs-chat send .nbs/chat/live.chat supervisor "Team online: @<handle1> @<handle2> @<handle3> ... Terminal goal: [goal]. First task: [if known]"
```

This anchors the team's starting state in the chat log.

### Step 11: Confirm and Explain

Tell the user what was created:

```
Created:
- .nbs/chat/live.chat      (coordination channel)
- .nbs/events/             (bus for event-driven coordination)
- .nbs/scribe/             (Scribe writes decision logs here)
- .nbs/workers/            (worker task files go here)
- .nbs/pids/               (handle collision guards)

Agents running:
- nbs-<handle1>-live       (<role>)
- nbs-<handle2>-live       (<role>)
...

Your terminal goal is posted to chat.

Next steps:
1. Load /nbs-teams-supervisor for planning guidance
2. Decompose your goal into worker tasks
3. Post 3Ws to chat after each worker completes

Maintenance:
- /nbs-teams-fixup    — diagnose and restart stalled agents
- /nbs-teams-help     — interactive guidance

Run /nbs-teams-help if you need guidance on any of these.
```

## Cross-Machine Agents (Optional)

If the team includes agents on remote machines, additional setup is required after the local cold start:

**Prerequisites:**
- SSH access from remote machine to coordination host (see cross-machine plan, Section 9)
- `nbs-chat-remote` and `nbs-bus-remote` installed on remote machine
- Same OS user on both machines (flock and cursor tracking require this)

**Remote agent startup:**

```bash
# On the remote machine:
export NBS_CHAT_HOST=<coordination-host>
export NBS_CHAT_OPTS="-o ControlMaster=auto -o ControlPersist=300"

NBS_HANDLE=<handle> bin/nbs-claude --dangerously-skip-permissions \
    --remote-host=<coordination-host>
```

**Verification:** Remote agent should appear in `nbs-chat participants` on the coordination host within 60 seconds.

**Handle namespacing:** For cross-machine deployments, consider using `handle:hostname` format (e.g. `claude:build-server-1`) to prevent collisions across machines.

## Known Failure Patterns

### Agent fails to start

**Symptom:** Tmux session exists but shows a bash prompt (nbs-claude exited immediately).
**Causes:**
- `bin/nbs-claude` not found or not executable (`chmod +x bin/nbs-claude`)
- `pty-session` not found (install from `~/.nbs/bin/` or project `bin/`)
- Handle collision — another agent with the same handle is already running
**Fix:** Check the log file at `.nbs/nbs-claude-*.log`. Fix the cause. Kill the dead tmux session and respawn.

### Sidecar fails to inject initial prompt

**Symptom:** Agent is running but never posted to chat. Tmux shows Claude's prompt with no input.
**Cause:** The sidecar waits up to 60 seconds for Claude's prompt (`>`) to appear. If Claude takes longer to initialise (first run, loading model), the window expires.
**Fix:** Manually send the initial prompt:
```bash
tmux send-keys -t nbs-<handle>-live "Your NBS handle is '<handle>'. Load /nbs-teams-chat. Use this handle for all nbs-chat send commands." Enter
```

### Context bleed from overloaded initial prompt

**Symptom:** Agent starts with low context (80-85%) instead of ~95%.
**Cause:** `NBS_INITIAL_PROMPT` is too long, or too many skills are loaded at startup.
**Prevention:** Keep initial prompts under 500 characters. Load additional skills via chat instructions after startup, not via the initial prompt.

### Stale pidfiles after crash

**Symptom:** `nbs-claude` refuses to start with "Handle already active" error.
**Cause:** Previous agent crashed without running its cleanup trap. Pidfile still contains the old PID.
**Fix:** Verify the PID is dead (`kill -0 <pid>`), then delete the pidfile:
```bash
rm .nbs/pids/<handle>.pid
```
Or use `--force` to override: `bin/nbs-claude --force`

## Rules

- **Terminal goal is mandatory.** Do not create structure without it.
- **Confirm before overwriting.** Existing `.nbs/` directories contain valuable state.
- **Verify after spawning.** Do not assume agents are alive — check tmux, chat, and pidfiles.
- **Stagger spawns.** Wait 5 seconds between agents to reduce lock contention.
- **Explain what was created.** The user should understand the structure.
- **One question, one action, confirmation.** This is not a wizard that asks 10 questions upfront.

## What Happens Next

The user is now the supervisor. They should:
1. Load `/nbs-teams-supervisor` for planning guidance
2. Decompose work into worker tasks
3. Use chat for all coordination — post tasks, read updates, capture 3Ws
4. Run `/nbs-teams-fixup` if agents stall

If they need help, they run `/nbs-teams-help`.
