---
description: Collaborative discovery of artefacts in projects lacking epistemic structure
allowed-tools: Read, Glob, Grep, AskUserQuestion, Bash(find:*), Bash(ls:*), Bash(git log:*)
---

# NBS Discovery

You are conducting an **NBS discovery** — a collaborative, read-only process to understand a project that was developed without epistemic discipline, or has drifted into disorder.

The human has context you cannot infer. Ask constantly. Confirm before concluding.

**This command makes no changes.** It produces a discovery report. When the human is ready to act on findings, they run `/nbs-recovery`.

---

## The Problem

Projects developed without epistemic structure accumulate:
- Scattered artefacts across multiple locations
- Undocumented decisions and their rationale
- Results whose value is unclear
- Dead ends mixed with genuine progress
- Implicit goals never written down

The goal of discovery is to **understand what exists and what it's worth**, so recovery can proceed safely.

---

## Process

### Phase 1: Establish Context

Before searching for anything, understand what you're discovering.

**Ask the human:**
- What was this project trying to achieve? (Terminal goal, even if never written)
- What timeframe did the work span?
- What locations might contain related artefacts?
- What are the valuable outcomes you want to preserve?
- What do you remember about dead ends or false starts?

**Do not proceed until you have answers.** The human's memory is the primary source of truth.

### Phase 2: Archaeology

Search for artefacts systematically. For each location the human suggests:

1. **List what exists** - file names, directories, dates
2. **Present findings to human** - "I found these files. Which are relevant?"
3. **Read only what human confirms** - don't waste tokens on dead ends
4. **Build a map** - track what's where and what it contains

**Checkpoint after each location**: Confirm understanding before moving on.

### Phase 3: Triage

For each artefact found, **ask the human**:

| Question | Why |
|----------|-----|
| What was this trying to do? | Context the file can't provide |
| Did it work? | Only the human knows |
| Is this a result (keep), false start (discard), or partial (evaluate)? | Human decides value |
| What does this prove or disprove? | Interpret with human guidance |

**Build a triage table** together:

```markdown
| Artefact | Location | Purpose | Status | Action |
|----------|----------|---------|--------|--------|
| file.py | [project]/v2/ | Parallel depickling | Works, verified | Keep - core result |
| test_v2.py | [project]/old/ | Alternative approach | Failed | Discard - document why |
| notes.md | [docs]/ | Design thinking | Partial | Extract key decisions |
```

### Phase 4: Gap Analysis

You now know what exists. But **what's missing** to reach the terminal goal?

This phase identifies instrumental goals that aren't captured in the artefacts. Work through this **step by step** - do not dump all questions at once.

**Step 1: Create a gap analysis plan**

Based on the terminal goal and what you've found, identify 3-6 questions about:
- What needs to happen to get from current state to terminal state?
- What infrastructure, environment, or tooling is needed?
- What external dependencies or processes must be navigated?
- What knowledge or decisions are missing?

Present this plan to the human: "I've identified N questions to understand the gaps. May I work through them one at a time?"

**Step 2: Work through questions one at a time**

For each question:
1. Ask the question clearly
2. Wait for the human's answer
3. Confirm your understanding: "So [restatement]. Is that correct?"
4. Only then proceed to the next question

**Do not batch questions.** Humans have limited working memory. One question, one answer, one confirmation.

**Important**: The confirmed restatements are valuable artefacts. They contain distilled human knowledge in verified form. These restatements must be preserved in full in the discovery report - they are input to recovery.

**Step 3: Synthesise into instrumental goals**

After all questions are answered, summarise:
- What instrumental goals are needed (not artefacts, but actions)
- What sequence makes sense
- What dependencies exist between goals

Present this synthesis to the human for confirmation before including in the report.

---

## Output: Discovery Report

At the end of discovery, produce a report for the human to review before recovery:

```markdown
# Discovery Report: [Project Name]

**Date**: [date]
**Terminal Goal (Reconstructed)**: [One sentence, confirmed by human]

## Artefacts Found

| Location | Files | Status |
|----------|-------|--------|
| [path] | [files] | [explored/skipped/partial] |

## Triage Summary

| Artefact | Purpose | Verdict | Rationale |
|----------|---------|---------|-----------|
| [file] | [what it does] | Keep/Discard/Evaluate | [why] |

## Valuable Outcomes Identified
[What was achieved, with evidence pointers]

## Gap Analysis

### Instrumental Goals Summary

| Goal | Why Needed | Dependencies |
|------|------------|--------------|
| [action needed] | [what it enables] | [what must come first] |

### Confirmed Understanding (Full Detail)

For each gap question, preserve the confirmed restatement in full. These are verified distilled knowledge - recovery needs them.

#### [Question 1 topic]
**Question**: [what was asked]
**Confirmed**: [the full restatement that was verified by human]

#### [Question 2 topic]
**Question**: [what was asked]
**Confirmed**: [the full restatement that was verified by human]

[Continue for all gap questions]

## Open Questions
[What remains uncertain - needs human input or further investigation]

## Recommended Next Steps
[What /nbs-recovery should do - but no action taken yet]
```

**Note**: The discovery report is the sole input to `/nbs-recovery`. Everything recovery needs must be in this report. Do not rely on conversation history or external logs.

---

## Rules

- **Read-only**. This command makes no changes to files or structure.
- **Ask constantly**. The human is the primary source of truth.
- **Show your work**. Present findings, let human interpret.
- **Admit uncertainty**. "I don't know what this is for" is valid — ask.
- **Document everything**. The discovery report is input to recovery.
