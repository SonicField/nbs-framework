# Why This Exists

AI systems generate plausible text. Plausibility is not truth.

The gap between "sounds right" and "is right" costs energy - human attention, compute cycles, wasted iterations. When a collaborator (human or AI) bullshits, the debt compounds. Every unchallenged assumption propagates. Every vague goal invites scope creep. Every untested claim becomes technical debt.

This framework externalises the epistemic discipline required to close that gap.

---

## The Problem

Most AI collaboration fails not from lack of capability but from lack of structure:

| Failure Mode | What Happens |
|--------------|--------------|
| **Goal drift** | We optimise for the wrong thing |
| **Confirmation bias** | We accept what sounds right without verification |
| **Authority worship** | We assume existing code/docs are correct |
| **Cherry-picking** | We report successes, bury failures |
| **Vague confidence** | "This should work" without defining what would falsify it |

These are not AI problems. They are epistemic problems. Humans have wrestled with them for millennia. AI research largely ignores this.

---

## The Solution

Three components:

### 1. Foundation and Pillars

Documented principles that can be referenced, not just invoked:

- **Goals** (foundation) - Terminal vs instrumental; what we actually want
- **Falsifiability** - Claims require potential falsifiers
- **Rhetoric** - Ethos, Pathos, Logos applied to technical work
- **Verification Cycle** - Design → Plan → Deconstruct → [Test → Code → Document]
- **Zero-Code Contract** - Clear separation of specification and implementation
- **Bullshit Detection** - Honest reporting; negative outcomes are data

### 2. Commands

Claude Code slash commands that apply these principles:

| Command | Purpose |
|---------|---------|
| `/nbs` | Review and dispatch - detects context, routes to appropriate verification |
| `/nbs-teams-start` | Bootstrap project for NBS teams (creates `.nbs/` structure) |
| `/nbs-teams-help` | Interactive guidance for NBS teams usage |
| `/nbs-help` | Interactive guidance for the NBS framework |
| `/nbs-discovery` | Read-only archaeology of a messy project |
| `/nbs-discovery-verify` | Verify discovery report is complete (auto-dispatched) |
| `/nbs-recovery` | Step-wise restructuring with confirmation |
| `/nbs-investigation` | Hypothesis testing through isolated experiments |
| `/nbs-audit` | Audit codebase against engineering standards with parallel sub-agents |
| `/nbs-poll` | Periodic check of chats and workers (heartbeat) |
| `/nbs-chat-digest` | Summarise chat channel history |
| `/nbs-pte` | Precise Technical English mode for unambiguous specifications |
| `/nbs-natural` | Exit Precise Technical English mode |

In addition to slash commands, the framework includes C binaries:

| Binary | Purpose |
|--------|---------|
| `nbs-hub` | Deterministic process enforcement for teams — audit gates, phase gates, stall detection |
| `nbs-chat` | File-based messaging (non-interactive commands) |
| `nbs-chat-terminal` | Interactive terminal client for human participation |
| `nbs-chat-remote` | SSH proxy for remote chat access |
| `nbs-worker` | Worker lifecycle management (spawn, monitor, dismiss) |
| `nbs-claude` | Sidecar for Claude Code integration (poll injection, plan mode auto-select) |

The dispatch design keeps one entry point (`/nbs`) while allowing context-specific verification. Run `/nbs` after discovery and it verifies the report. Run it after recovery and it reviews the work. Run it mid-session and it audits for drift.

### 3. Testing

Tests that evaluate AI output using AI, with human-verifiable verdicts. Non-deterministic output, deterministic judgement.

---

## How It Works

The framework does not enforce rules. It prompts questions.

When you run `/nbs`, the AI reads the foundation document. If ambiguity arises, it reads the relevant pillar. It then produces a short report surfacing:

- Terminal goal status (clear? drifted? abandoned?)
- Instrumental goal coherence (sequenced or scattered?)
- Documentation state (current or stale?)
- Falsifiability discipline (evidence or assertion?)
- Bullshit detection (all outcomes reported?)

The human reads the report and decides what to do. The AI proposes; the human disposes.

For messy projects, `/nbs-discovery` maps what exists before any changes. The human provides context the files cannot. The output is a discovery report - a triage table of artefacts with verdicts.

Then `/nbs-recovery` creates a step-wise plan. Each step is atomic, reversible, described. The human confirms each step before execution. Nothing is deleted without explicit approval.

---

## The Underlying Claim

Better epistemics produce better outcomes.

This is falsifiable:
- If projects using this framework show no improvement in goal alignment, it fails
- If the framework is unused, it fails
- If the pillars don't match the author's actual standards, they're wrong

The framework is a hypothesis. Use it, measure it, revise or discard it.

---

## Who This Is For

Anyone collaborating with AI on technical work who wants:
- Explicit goals instead of assumed understanding
- Verifiable claims instead of plausible assertions
- Honest reporting instead of optimistic summaries
- Structured recovery instead of chaotic archaeology

If you trust vibes, this is not for you. If you want evidence, read on.
