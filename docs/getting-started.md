# Getting Started

## Installation

```bash
git clone https://github.com/SonicField/nbs-framework.git
cd nbs-framework

# Build C binaries (nbs-chat, nbs-chat-terminal, nbs-chat-remote)
cd src/nbs-chat && make && cd ../..

# Install (symlinks bin/ to ~/.nbs/bin/, installs Claude commands)
./bin/install.sh
```

This:
1. Compiles the C tools (`nbs-chat`, `nbs-chat-terminal`, etc.)
2. Processes command templates with your install path
3. Creates `~/.nbs/` with commands and symlinks to concepts/docs/bin
4. Creates symlinks in `~/.claude/commands/` for Claude Code

Restart Claude Code to pick up the commands.

### Custom Install Location

For testing or alternative setups:

```bash
./bin/install.sh --prefix=/path/to/custom/location
```

## First Use

### For NBS teams (supervisor/worker patterns)

When you want to set up a project for NBS teams:

```
/nbs-teams-start
```

The command asks for your terminal goal and creates:
- `.nbs/chat/live.chat` - Coordination channel
- `.nbs/events/` - Bus for event-driven coordination
- `.nbs/scribe/` - Scribe writes decision logs here
- `.nbs/workers/` - Worker task files go here

You become the supervisor. Decompose work into tasks and spawn workers:

```bash
nbs-worker spawn <slug> /path/to/project "Task description"
```

Coordinate via chat and capture learnings.

Need help?

```
/nbs-teams-help
```

Interactive guidance on spawning workers, writing tasks, task scope, monitoring, and chat-based coordination.

```
/nbs-help
```

Interactive guidance on the NBS framework itself - goals, falsifiability, investigations.

### During a work session

When you want a check on your current work:

```
/nbs
```

The command reviews the conversation history and any project files, then produces a short report. Read it in under two minutes. Act on it or not.

### When facing a messy project

When you have scattered artefacts, unclear goals, and no documentation:

**Step 1: Discovery**

```
/nbs-discovery
```

The AI asks you questions. Answer them. You know things the files cannot tell.

The process has four phases:
1. **Establish Context** - terminal goal, timeframe, locations
2. **Archaeology** - find artefacts, present to you, build a map
3. **Triage** - assess each artefact with your input
4. **Gap Analysis** - identify what's missing to reach the goal (one question at a time)

At the end, you get a discovery report.

**Step 2: Verify**

```
/nbs
```

After discovery, run `/nbs`. It detects you just did discovery and automatically verifies the report is complete - all sections present, confirmed restatements captured in full, nothing lost from the conversation.

**Step 3: Recovery**

```
/nbs-recovery
```

The AI creates a plan. Each step is described, justified, reversible. You confirm each step before it executes. Nothing changes without your approval.

**Step 4: Review**

```
/nbs
```

After recovery, run `/nbs` again. It reviews the recovery work for drift, bullshit, and blind spots.

---

## The Documents

Read these when you want to understand the principles:

| Document | What It Contains |
|----------|-----------------|
| `concepts/goals.md` | Foundation - terminal vs instrumental goals |
| `concepts/falsifiability.md` | What makes a claim testable |
| `concepts/rhetoric.md` | Ethos, Pathos, Logos applied to technical work |
| `concepts/verification-cycle.md` | Design → Plan → Deconstruct → [Test → Code → Document] |
| `concepts/zero-code-contract.md` | Engineer specifies, Machinist implements |
| `concepts/bullshit-detection.md` | Honest reporting and negative outcome analysis |

The AI reads these when it needs them. You read them when you want to.

---

## Testing

Run the test suite:

```bash
./tests/run_all.sh
```

For quick tests (skips slow AI evaluation tests):

```bash
./tests/run_all.sh --quick
```

Tests use AI to evaluate AI. The verdict files contain deterministic pass/fail judgements.

---

## What to Expect

The framework does not guarantee good outcomes. It prompts good questions.

You will be asked:
- What is the terminal goal?
- What would falsify this approach?
- Is this result being reported honestly?
- What does this failure reveal?

The discomfort is the point. Comfortable collaboration produces comfortable bullshit.
