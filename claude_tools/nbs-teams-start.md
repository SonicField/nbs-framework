---
description: Bootstrap a project for NBS teams in one command
allowed-tools: Write, Bash, Read
---

# NBS Teams: Start (Cold Start)

You are performing a **cold start** — bootstrapping a project from nothing to a running multi-agent team. Infrastructure is created by `nbs-chat-init`. Your job is to gather the user's intent, call `nbs-chat-init`, then spawn and verify agents.

**This is a one-time setup command.** If `.nbs/` already exists, confirm before overwriting.

## Process

### Step 1: Check for Existing Structure

```bash
ls -la .nbs/ 2>/dev/null
```

**If it exists:**
- Ask: "An `.nbs/` directory already exists. Would you like to add teams to it, or is this a fresh start?"
- If fresh start: proceed. If adding: skip to Step 4.
- **Warning:** Existing `.nbs/` directories contain valuable state. Do not delete without explicit confirmation.

**If it does not exist:** Proceed to Step 2.

### Step 2: Establish Terminal Goal

Ask the user:

> "What is the terminal goal for this project? (One sentence describing what you're trying to achieve)"

Wait for their answer. **Do not proceed without a terminal goal.**

### Step 3: Create Infrastructure with nbs-chat-init

Choose a chat name based on the project (e.g. the directory name). Then run:

```bash
nbs-chat-init --name=<chat-name>
```

This creates everything: `.nbs/chat/`, `.nbs/events/`, `.nbs/scribe/`, `.nbs/workers/`, `.nbs/pids/`, bus config, chat file, and scribe log. **Do not manually create these directories or files — nbs-chat-init is the single source of truth.**

If the user wants the `--root` flag to point at a different directory:

```bash
nbs-chat-init --name=<chat-name> --root=<project-root>
```

Verify it worked:

```bash
nbs-chat read .nbs/chat/<chat-name>.chat --last=1
ls .nbs/events/config.yaml
```

### Step 4: Plan Team Composition

Ask the user how many agents they want and what roles are needed:

| Role | Handle | Purpose |
|------|--------|---------|
| Supervisor | `supervisor` | Goal-keeper, task delegation, 3Ws |
| Generalist | `generalist` | Tactical work on delegated tasks |
| Scribe | `scribe` | Decision logging, institutional memory |
| Gatekeeper | `gatekeeper` | Code review, pre-push verification |
| Testkeeper | `testkeeper` | Test suite ownership, verification |
| Theologian | `theologian` | Architecture, invariant enforcement |

**Minimum viable team:** supervisor + generalist.

**Standard team:** All six roles above.

### Step 5: Post Terminal Goal to Chat

```bash
nbs-chat send .nbs/chat/<chat-name>.chat supervisor "Terminal goal: [user's goal verbatim]"
```

### Step 6: Write Skill Files

For each agent role, copy the skill content to a file the agent can read on startup:

```bash
mkdir -p .nbs/workers
for role in scribe gatekeeper testkeeper theologian generalist supervisor; do
    if [[ -f "$HOME/.nbs/commands/nbs-${role}.md" ]]; then
        cp "$HOME/.nbs/commands/nbs-${role}.md" ".nbs/workers/${role}-skill.md"
    elif [[ -f "$HOME/.nbs/commands/nbs-teams-chat.md" && "$role" == "generalist" ]]; then
        cp "$HOME/.nbs/commands/nbs-teams-chat.md" ".nbs/workers/${role}-skill.md"
    fi
done
```

### Step 7: Spawn Agents

Spawn agents one at a time with 5-second stagger:

```bash
for handle in scribe gatekeeper testkeeper theologian generalist supervisor; do
    NBS_HANDLE="$handle" \
    NBS_TRANSPORT=ts \
    NBS_INITIAL_PROMPT="Read .nbs/workers/${handle}-skill.md and follow the role instructions. Then read the chat history and begin work." \
    setsid .nbs/bin/nbs-claude --root="$(pwd)" --dangerously-skip-permissions \
        >/dev/null 2>&1 &
    echo "Spawned $handle"
    sleep 5
done
```

### Step 8: Verify Agents Are Alive

Wait 30 seconds. Then:

**8a. Sessions exist:**

```bash
nbs-ts list | grep alive
```

**8b. Agents posted to chat:**

```bash
nbs-chat participants .nbs/chat/<chat-name>.chat
```

**8c. Pidfiles exist:**

```bash
ls .nbs/pids/
```

If an agent is alive in nbs-ts but not in chat after 60 seconds, the sidecar failed to inject. Diagnose:

```bash
nbs-ts read-new <handle> --strip | tail -20
```

### Step 9: Post Team Roster

```bash
nbs-chat send .nbs/chat/<chat-name>.chat supervisor \
    "Team online: @scribe @gatekeeper @testkeeper @theologian @generalist @supervisor. Terminal goal: [goal]."
```

### Step 10: Confirm to User

Tell the user what was created and what to do next:

```
Infrastructure: nbs-chat-init created chat, bus, scribe log, workers, pids.
Agents running: [list handles]

Next steps:
1. Use nbs-chat-terminal .nbs/chat/<name>.chat <your-handle> to interact
2. Post tasks and directions to chat — agents will respond
3. /nbs-teams-fixup if agents stall
4. /nbs-teams-help for guidance
```

## Rules

- **Terminal goal is mandatory.** Do not create structure without it.
- **Use nbs-chat-init for infrastructure.** Do not manually create directories or config files.
- **Verify after spawning.** Check nbs-ts, chat participants, and pidfiles.
- **Stagger spawns.** 5 seconds between agents.
- **One question, one action, confirmation.** Not a wizard.
