---
description: Configure a project's CLAUDE.md with NBS engineering standards
allowed-tools: Bash, Read, Write, Edit, Glob
---

# NBS Init

Configure the current project's CLAUDE.md to reference the NBS engineering standards. Idempotent — safe to run repeatedly.

---

## Step 1: Verify NBS Installation

Check that `{{NBS_ROOT}}/concepts/engineering-standards.md` exists and is readable. Read the file to confirm.

If the file does not exist or is not readable, **abort** with this message:

> NBS engineering standards not found at `{{NBS_ROOT}}/concepts/engineering-standards.md`.
>
> Install the NBS framework first. See: https://github.com/SonicField/nbs-framework

Do not proceed past this step if the file is missing.

---

## Step 2: Find CLAUDE.md

Look for `CLAUDE.md` in the current working directory (the project root).

Determine which case applies:

1. **No CLAUDE.md exists** — proceed to Step 3a
2. **CLAUDE.md exists and contains `<!-- NBS:BEGIN -->`** — proceed to Step 3b
3. **CLAUDE.md exists without an NBS block** — proceed to Step 3c

---

## Step 3a: Create New CLAUDE.md

Create `CLAUDE.md` in the project root containing exactly:

```markdown
<!-- NBS:BEGIN -->
## Engineering Standards

**MANDATORY**: Always follow the engineering standards defined in:
`{{NBS_ROOT}}/concepts/engineering-standards.md`

Read this file at the start of any engineering work. The core principles are:
- Safety through verbs, not nouns
- Falsifiability as foundation
- The Cycle of Verified Construction: Design → Plan → Deconstruct → [Test → Code → Document] → Next
- Integration-first testing
- Assertions at all levels (preconditions, postconditions, invariants)

### Foundational Principle: Falsifiability

The antidote to bullshit is falsifiability. A claim without a potential falsifier is bullshit, even if it happens to be true.

**The implicit contract for any claim:**
1. I can articulate what would prove me wrong
2. I have done what I can to find that counterexample
3. I am reporting actual confidence, not performing confidence
<!-- NBS:END -->
```

---

## Step 3b: Update Existing NBS Block

The CLAUDE.md already contains an NBS block delimited by `<!-- NBS:BEGIN -->` and `<!-- NBS:END -->`.

Replace everything from `<!-- NBS:BEGIN -->` through `<!-- NBS:END -->` (inclusive) with the exact block content shown in Step 3a.

Use the Edit tool to perform this replacement.

---

## Step 3c: Append NBS Block

The CLAUDE.md exists but has no NBS block. Append the block from Step 3a to the end of the file, preceded by a blank line to separate it from existing content.

Use the Edit tool or Write tool as appropriate.

---

## Step 4: Verify

After writing, read back the CLAUDE.md and confirm:

1. The `<!-- NBS:BEGIN -->` and `<!-- NBS:END -->` markers are present
2. The engineering standards path (`{{NBS_ROOT}}/concepts/engineering-standards.md`) appears in the block
3. The referenced file is readable (already confirmed in Step 1)

Report what was done:

- **Created** — new CLAUDE.md was created
- **Updated** — existing NBS block was replaced
- **Appended** — NBS block was added to existing CLAUDE.md

If verification fails, report the specific failure.
