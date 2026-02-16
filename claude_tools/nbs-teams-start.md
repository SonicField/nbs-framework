---
description: Bootstrap a project for NBS teams in one command
allowed-tools: Write, AskUserQuestion, Bash(mkdir:*)
---

# Start NBS Teams

You are bootstrapping a project for NBS teams. This creates the minimal structure needed to start working with supervisor/worker patterns.

**This is a one-time setup command.** If `.nbs/` already exists, confirm before overwriting.

---

## Process

### Step 1: Check for Existing Structure

Before anything, check if `.nbs/` exists:

```bash
ls -la .nbs/ 2>/dev/null
```

**If it exists:**
- Ask: "An .nbs/ directory already exists. Would you like to add teams structure to it, or is this a fresh start?"
- If fresh start, proceed. If adding, skip directory creation for existing dirs.

### Step 2: Ask Terminal Goal

Ask the user:

> "What is the terminal goal for this project? (One sentence describing what you're trying to achieve)"

Wait for their answer. This grounds all subsequent work.

**Do not proceed without a terminal goal.** If the answer is vague, ask for clarification:
- "Can you be more specific? What would success look like?"
- "What problem are you solving?"

### Step 3: Create Directory Structure

```bash
mkdir -p .nbs/chat .nbs/events/processed .nbs/scribe .nbs/workers
```

### Step 4: Create Chat Channel

```bash
nbs-chat create .nbs/chat/live.chat
```

### Step 5: Create Bus Config

Write `.nbs/events/config.yaml`:

```yaml
dedup-window: 300
ack-timeout: 120
pythia-interval: 20
retention-max-bytes: 16777216
```

### Step 6: Post Terminal Goal to Chat

```bash
nbs-chat send .nbs/chat/live.chat supervisor "Terminal goal: [user's goal verbatim]"
```

### Step 7: Confirm and Explain

Tell the user what was created:

```
Created:
- .nbs/chat/live.chat (coordination channel)
- .nbs/events/ (bus for event-driven coordination)
- .nbs/scribe/ (Scribe writes decision logs here)
- .nbs/workers/ (worker task files go here)

Your terminal goal is posted to chat.

Next steps:
1. Load /nbs-teams-supervisor for planning guidance
2. Decompose your goal into worker tasks
3. Spawn workers with nbs-worker or Task tool sub-agents
4. Post 3Ws to chat after each worker completes

Run /nbs-teams-help if you need guidance on any of these.
```

---

## Rules

- **One question, one action, confirmation.** This is not a wizard that asks 10 questions upfront.
- **Terminal goal is mandatory.** Do not create structure without it.
- **Confirm before overwriting.** Existing `.nbs/` directories contain valuable state.
- **Explain what was created.** The user should understand the structure.

---

## What Happens Next

The user is now the supervisor. They should:
1. Load `/nbs-teams-supervisor` for planning guidance
2. Decompose work into worker tasks
3. Use `nbs-worker spawn` or Task tool sub-agents to create and start workers
4. Post 3Ws to chat after each worker completes

If they need help, they run `/nbs-teams-help`.
