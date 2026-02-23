---
description: Systematic audit of reasoning quality, goal alignment, and falsification discipline
allowed-tools: Read, Glob, Grep, AskUserQuestion, Bash(git log:*), Bash(git status:*), Bash(git diff:*), Bash(git branch:*), Bash(git rev-parse:*)
---

# NBS Review

**MANDATORY FIRST ACTION - DO NOT SKIP**

Before doing ANYTHING else, run these detection checks:

```
1. git branch --show-current
2. Glob pattern: **/INVESTIGATION-STATUS.md
3. Check for .nbs/terminal-weathering/ directory
```

Dispatch based on results:

| Condition | Action |
|-----------|--------|
| Branch starts with `investigation/` | **UNAMBIGUOUS** → Investigation review immediately. No confirmation. |
| Branch starts with `weathering/` | **UNAMBIGUOUS** → Terminal weathering review (normal review + weathering correctness checks). No confirmation. |
| `INVESTIGATION-STATUS.md` at repo root | Investigation review |
| `.nbs/terminal-weathering/` exists | Normal review + terminal weathering correctness checks |
| `INVESTIGATION-STATUS.md` elsewhere only | Ask user to clarify |
| Neither branch nor file found | Normal review |

---

You are conducting an **NBS review** - a systematic audit of reasoning quality, goal alignment, and falsification discipline for the current project or coding session.

You have access to the full conversation history. Use it.

---

## Step 0: Context Detection and Dispatch

**You must run the detection checks above before proceeding.**

### Investigation Branch Detected (Unambiguous Signal)

If branch starts with `investigation/`:

→ **IMMEDIATELY produce investigation review. Do NOT ask for confirmation. Do NOT produce normal review.** The branch name is an unambiguous signal that investigation context is active.

Review the investigation work:
- Is the hypothesis clearly stated and falsifiable?
- Are experiments designed with clear pass/fail criteria?
- Are observations recorded (not just interpretations)?
- Is the status document current (read INVESTIGATION-STATUS.md if it exists)?

**Output format**: Short review of investigation rigour. NOT Status/Issues/Recommendations format.

### INVESTIGATION-STATUS.md at Repo Root (No Investigation Branch)

Same as above - produce investigation review.

### INVESTIGATION-STATUS.md Found Elsewhere Only (Not Repo Root, Not Investigation Branch)

Ask the user:
> "I found an INVESTIGATION-STATUS.md file at [path]. Are you currently in an investigation, or is this a test fixture / old file?"

### If you executed `/nbs-discovery` this session

Evidence: You performed Phases 1-4 (Establish Context, Archaeology, Triage, Gap Analysis) and produced a discovery report.

→ **Dispatch**: Read `{{NBS_ROOT}}/claude_tools/nbs-discovery-verify.md` and apply it instead of continuing here. That command verifies discovery report completeness before recovery.

### If you executed `/nbs-recovery` this session

Evidence: You read a discovery report and created or executed a recovery plan.

→ **Continue below**: Apply normal NBS review to the recovery work. (Future: may dispatch to recovery-specific verification)

### If none of the above

→ **Continue below**: Apply normal NBS review.

### Terminal Weathering Context Detected

If branch starts with `weathering/` or `.nbs/terminal-weathering/` exists:

→ **Dispatch**: Read `{{NBS_ROOT}}/claude_tools/nbs-terminal-weathering-review.md` and apply it. This provides terminal-weathering-specific correctness checks **in addition to** the normal NBS review below.

---

## Your Task

Produce a **concise, human-readable** review (readable in under 2 minutes) that surfaces drift, bullshit, and blind spots before they compound.

Where you identify gaps, goal drift, or areas where human context would help, **ask specific questions** using the AskUserQuestion tool. Do not guess. Do not proceed on assumption.

---

## Review Dimensions

### 1. Terminal Goals
- Are terminal goals clearly articulated and written down?
- Has there been drift, loss of sight, or abandonment of terminal goals?
- **Watch for**: Myopic fixation on minor issues that damages progress toward the actual goal. Example: modifying code to make a test pass rather than questioning whether the test itself is wrong when viewed from the terminal goal.

### 2. Instrumental Goals
- Are tactical goals clearly defined?
- Is there a coherent sequence, or just "the next thing in front of our noses"?
- Is scaffolding (useful now, discarded later) distinguished from permanent work?

### 3. Ethos / Pathos / Logos Alignment

Check for these failure modes:

| Mode | Failure | Example |
|------|---------|---------|
| **Ethos** | Appealing to authority over evidence | Assuming existing code is more correct than new code simply because it existed first |
| **Pathos** | Building something that won't serve the humans who need it | Technically elegant but unusable; wrong paradigm for the user |
| **Logos** | Aesthetic detour disguised as logic | Obsessing over perfect algorithm when sloppy idempotent one runs 10x faster; ignoring cache effects |

### 4. Documentation State
- Does a written plan exist? Is it current?
- Does a progress log exist? Is it current?
- Are commits clean, small, atomic (enabling backtracing and bisection)?

### 5. Falsifiability Discipline
- Is each choice backed by falsifiable evidence it is not the wrong choice?
- If evidence is missing, how would we create it?
- Are we logging: falsification criteria, attempts, outcomes?
- Tests written before code?
- Tests verified to fail on bugs (by experiment, not reasoning)?
- Assertions present and tested (debug + optimised modes)?
- How long since correctness tools ran (linters, tsan, etc.)? If long, why?

### 6. Bullshit Check
- Are all outcomes being reported, or are we cherry-picking positive results?
- Are negative results being analysed for what they reveal?
- Is confidence being performed or is it actual?
- Remember: **The valuable information lives in the negative outcomes** and the differences between conditions that produce positive vs negative results.

---

## Process

### Step 1: Foundation Check

Before anything else, ask yourself: **Do I have clear memory of having read the pillars in this session?**

- If **yes** (you can recall specific content from goals.md, falsifiability.md, etc.): proceed to Step 2.
- If **no** or **unsure**: Read the foundation documents now. Context compaction erodes pillar knowledge - re-reading restores it.

**Read all pillars** (required, no exceptions):
1. `{{NBS_ROOT}}/concepts/goals.md` - the foundation
2. `{{NBS_ROOT}}/concepts/falsifiability.md` - the method
3. `{{NBS_ROOT}}/concepts/rhetoric.md` - human intent and failure modes
4. `{{NBS_ROOT}}/concepts/bullshit-detection.md` - honest reporting
5. `{{NBS_ROOT}}/concepts/verification-cycle.md` - the process
6. `{{NBS_ROOT}}/concepts/zero-code-contract.md` - human-AI roles
7. `{{NBS_ROOT}}/concepts/engineering-standards.md` - the standards

Do not skip any. Do not read "the most relevant ones." Read all of them.

The framework lives at: `{{NBS_ROOT}}/` (or locate via the symlink at `~/.claude/commands/nbs.md`).

### Step 2: Initial Review

1. **Read the conversation history** - understand what's been discussed, decided, attempted
2. **Read relevant project files** - look for:
   - Plan files (pattern: `*-plan.md`, `*-plan.soma`)
   - Progress logs (pattern: `*-progress.md`, `*-progress.soma`)
   - CLAUDE.md or similar project instructions
3. **Assess each dimension** - be stark, be honest
4. **Identify gaps** - note where you lack clarity or need human context

### Step 3: Deepen Where Needed

For any dimension where you lack clarity, read the relevant pillar before concluding. **When in doubt, read. The cost of re-reading is lower than the cost of drift.**

| Dimension | Read | Signal you need to read |
|-----------|------|------------------------|
| Goals | `{{NBS_ROOT}}/concepts/goals.md` | Can't state terminal goal in one sentence |
| Human intent / Pathos | `{{NBS_ROOT}}/concepts/goals.md` then `{{NBS_ROOT}}/concepts/rhetoric.md` | Don't know why the human wants this |
| Ethos / Logos failures | `{{NBS_ROOT}}/concepts/rhetoric.md` | Unsure if issue is authority-appeal or aesthetic detour |
| Verification discipline | `{{NBS_ROOT}}/concepts/verification-cycle.md` | Unclear what phase we're in |
| Falsifiability | `{{NBS_ROOT}}/concepts/falsifiability.md` | Making claims without falsifiers |
| Honest reporting | `{{NBS_ROOT}}/concepts/bullshit-detection.md` | Only reporting positive outcomes |
| Human-AI roles | `{{NBS_ROOT}}/concepts/zero-code-contract.md` | Unclear who decides what |
| Engineering standards | `{{NBS_ROOT}}/concepts/engineering-standards.md` | Assertions missing, anti-patterns present, testing methodology unclear |

**Judgement call**: If the issue is obvious (no plan exists), don't spend tokens reading pillars. If the issue is nuanced (is this Ethos or Logos? why does the human want this?), read for guidance.

This is intelligence, not algorithm. The pillars guide your judgement; they don't replace it.

### Step 4: Ask

Where human input is needed, use AskUserQuestion. Do not guess. Do not proceed on assumption.

### Step 5: Produce Output

Recommendations with falsification criteria. Concise. Readable in under 2 minutes.

---

## Output Format

Your final output must be concise. Use this structure:

```markdown
# NBS Review

## Status
[2-4 bullets max - overall health assessment]

## Issues
[Bulleted list - one line each - stark assessments]

## Questions for You
[If you used AskUserQuestion, summarise what was clarified]
[If questions remain, list them]

---

## Recommendations

### Strategic
[If any - with falsification criteria for each]

### Tactical
[Immediate actions - with evidence requirements for each]
```

---

## Rules

- **Conciseness is mandatory**. The human must be able to read this in under 2 minutes.
- **No bullshit**. If the honest recommendation is "deep-six the last 6 weeks' work," say it. Back it with falsifiable criteria.
- **Ask, don't assume**. Use AskUserQuestion when you need human context or goal clarification.
- **Evidence over assertion**. Every recommendation needs falsification criteria.

---

## The Contract

Neither party trusts assertions. Both parties trust evidence.

_Prove you understand the problem by defining how you would falsify the solution, then build the solution, then record what you learned._
