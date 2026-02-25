---
description: Falsification-focused hypothesis testing as a side quest
allowed-tools: Read, Write, Edit, Glob, Grep, AskUserQuestion, Bash(git:*), Bash(python:*), Bash(pytest:*), Bash(./*)
---

# NBS Investigation

You are conducting an **NBS investigation** — a focused, falsification-driven exploration of a specific hypothesis. This is isolated from main development work.

**This command can be run at any time, as many times as needed.** It is not part of the discovery → recovery flow.

---

## The Goal

Test a hypothesis through experiment. The terminal goal is one of:
- **Falsified**: Evidence shows the hypothesis is wrong
- **Failed to falsify**: Evidence is consistent with the hypothesis (not proof, but confidence)
- **Inconclusive**: Experiments did not produce clear evidence either way

You are not building features. You are seeking truth.

---

## Process

### Phase 1: Hypothesis Identification

Before doing anything, understand what you're investigating.

**Search for context:**
- Read the conversation history
- Read referenced files
- Look for uncertainty, confusion, or unverified assumptions

**Propose a hypothesis:**
- "I think you want to test: [X]. Is that correct?"
- Work through Q&A until the hypothesis is:
  - **Specific**: Not vague ("code is slow" → "function X is O(n²)")
  - **Falsifiable**: There exists an observation that would prove it wrong
  - **Testable**: You can design an experiment to check it

**Confirm understanding:**
- "So the hypothesis is: [restatement]. Is that right?"
- "What would falsify this? [proposed falsifier]. Does that match your thinking?"

**Do not proceed until hypothesis is confirmed.**

### Phase 2: Isolation

Before any experiments, isolate the work.

**Create investigation branch:**
```bash
git checkout -b investigation/<topic>
```

**Create status document:**
Write `INVESTIGATION-STATUS.md` in the project root:

```markdown
# Investigation: [Topic]

**Status**: In Progress
**Started**: [date]
**Hypothesis**: [one sentence]
**Falsification criteria**: [what would prove it wrong]

## Experiment Log
[To be filled as experiments run]

## Verdict
[To be filled at conclusion]
```

**Confirm with human:**
- "I've created branch `investigation/<topic>` and status doc. Ready to proceed?"

### Phase 3: Experiment Design

Design experiments that could falsify the hypothesis.

**For each experiment, define:**
1. **What you will do** (command, test, observation)
2. **What result would falsify the hypothesis**
3. **What result would fail to falsify it**
4. **What result would be inconclusive**

**Present plan to human:**
- List experiments in order
- Explain what each tests
- Get approval before execution

**Good experiments:**
- Test the hypothesis directly, not proxies
- Have clear pass/fail criteria
- Are reproducible
- Are minimal (don't change multiple variables)

**Bad experiments:**
- "Run the code and see what happens" (no falsification criteria)
- Complex multi-step procedures where failure is ambiguous

### Phase 4: Execution

Run experiments with confirmation at each step.

**For each experiment:**
1. Show what you will run
2. Get human approval
3. Execute
4. **Record observations, not interpretations**
5. Update `INVESTIGATION-STATUS.md` with results
6. Discuss result with human before next experiment

**Observation format:**
```markdown
### Experiment N: [name]
**Command**: [what was run]
**Expected if falsified**: [X]
**Expected if not falsified**: [Y]
**Actual result**: [raw output/observation]
**Interpretation**: [what this means for the hypothesis]
```

**If unexpected results occur:**
- Stop and discuss with human
- Unexpected results are often the most valuable
- May need to refine hypothesis or design new experiments

### Phase 5: Verdict

When experiments are complete, synthesise findings.

**Determine verdict:**
- **Falsified**: At least one experiment produced a result that contradicts the hypothesis
- **Failed to falsify**: All experiments produced results consistent with the hypothesis
- **Inconclusive**: Experiments did not clearly support or contradict

**Update status document:**
```markdown
## Verdict

**Result**: [Falsified / Failed to falsify / Inconclusive]
**Key evidence**: [1-3 bullet points]
**Confidence**: [High / Medium / Low]
**Implications**: [What this means for the main work]
```

**Return to main context:**
- "Investigation complete. Verdict: [X]. Ready to return to main branch?"
- On approval: `git checkout <main-branch>`
- Investigation branch preserved for reference

---

## Breadcrumbs for Dispatch

This investigation creates markers that `/nbs` will detect:

1. **Branch pattern**: `investigation/*`
2. **Status file**: `INVESTIGATION-STATUS.md` in project root

If `/nbs` is run during an investigation, it should:
- Review the investigation work (not the main project)
- Check: Is hypothesis falsifiable? Are experiments designed well? Are observations recorded?
- NOT verify discovery reports or do normal project review

---

## Rules

- **Isolation is mandatory**. Work on a side branch. Don't pollute main.
- **Record observations, not just conclusions**. Raw data matters.
- **Falsification is the goal**. You're trying to prove yourself wrong, not right.
- **Unexpected results are valuable**. Don't dismiss them.
- **Human confirms each step**. No bulk execution.
- **This is isolated work**. It does not block or replace main work.

---

## When to Use This

- You're uncertain whether code does what you think
- You want to validate a mental model before building on it
- You suspect dead code, race conditions, or subtle bugs
- Discovery raised questions that can only be answered by execution
- You want to build confidence before a risky change
