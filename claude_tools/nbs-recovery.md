---
description: Step-wise recovery of projects based on discovery report
allowed-tools: Read, Write, Edit, Glob, Grep, AskUserQuestion, Bash(mv:*), Bash(cp:*), Bash(mkdir:*), Bash(git:*)
---

# NBS Recovery

You are conducting an **NBS recovery** - the action phase that follows `/nbs-discovery`. This command restructures, consolidates, and establishes epistemic discipline for a project.

**Prerequisites**: A discovery report should exist from a prior `/nbs-discovery` run. If not, ask the human if they want to proceed anyway or run discovery first.

---

## The Goal

The terminal goal is: **valuable outcomes preserved, verified, and usable**.

The discovery report identified what's worth keeping. This command:
1. Creates a step-wise recovery plan
2. Executes each step with human confirmation
3. Preserves originals until success is verified
4. Establishes epistemic structure going forward

---

## Process

### Phase 1: Load Discovery

Find and read the discovery report.

**Ask the human:**
- Where is the discovery report? (or: should we proceed without one?)
- Has anything changed since discovery?
- Any decisions you've reconsidered?

If no discovery report exists, offer to run a quick discovery first or proceed with human-guided recovery.

### Phase 2: Create Recovery Plan

Based on discovery, create a step-wise plan. Each step must be:

| Property | Why |
|----------|-----|
| **Atomic** | One action per step |
| **Reversible** | Can undo if wrong |
| **Described** | Human knows what will happen |
| **Ordered** | Dependencies respected |

**Plan format:**

```markdown
# Recovery Plan: [Project Name]

**Based on**: [discovery report path]
**Date**: [date]

## Steps

### Step 1: [Action]
- **What**: [precise description]
- **Why**: [rationale from discovery]
- **Reversible**: [how to undo]
- **Status**: Pending

### Step 2: [Action]
...
```

**Present plan to human for approval before any execution.**

### Phase 3: Execute with Confirmation

For each step:

1. **Show the step**: What will happen, why, how to undo
2. **Ask for confirmation**: "Proceed with step N? [yes/skip/abort]"
3. **Execute if confirmed**
4. **Report result**: What happened, any issues
5. **Update plan**: Mark step complete or note issues

**If any step fails:**
- Stop execution
- Report what went wrong
- Ask human how to proceed (retry/skip/abort/rollback)

### Phase 4: Establish Structure

After restructuring, establish epistemic discipline:

1. **Create plan file**: `<date>-<project>-plan.md` with terminal goal and next steps
2. **Create progress log**: `<date>-<project>-progress.md` documenting recovery
3. **Initialise version control**: If not already under git
4. **Document falsification criteria**: For each preserved result

### Phase 5: Verify

Before declaring success:

1. **Check preserved artefacts**: Are they intact and accessible?
2. **Test reproducibility**: Can key results be reproduced?
3. **Confirm with human**: Does this match what you expected?

---

## Recovery Actions

Common actions the plan might include:

| Action | Description |
|--------|-------------|
| `create_directory` | Create new directory structure |
| `move_file` | Move file to new location (preserve original until verified) |
| `consolidate` | Merge multiple files into one |
| `extract` | Pull relevant sections from a larger file |
| `archive` | Move to archive location (don't delete) |
| `document` | Create documentation for undocumented work |
| `git_init` | Initialise version control |
| `git_commit` | Commit current state |

**Never delete without explicit human approval.** Archive instead.

---

## Output

At the end of recovery, produce:

1. **Restructured project** in agreed location
2. **Recovery log** documenting what was done
3. **Plan file** for future work
4. **Progress file** capturing this session

```markdown
# Recovery Log: [Project Name]

**Date**: [date]
**Discovery Report**: [path]

## Actions Taken

| Step | Action | Result |
|------|--------|--------|
| 1 | [what] | [success/skipped/failed] |
| 2 | [what] | [result] |

## Final Structure
[Tree or description of resulting project layout]

## Falsification Status
[For each preserved result: criteria, evidence, gaps]

## Next Steps
[What remains to be done]
```

---

## Rules

- **Confirm every step**. No bulk changes without per-step approval.
- **Preserve before moving**. Copy first, delete original only after verification.
- **Never delete without explicit approval**. Archive instead.
- **Report honestly**. If something fails, say so.
- **Stop on unexpected issues**. Don't power through problems.
