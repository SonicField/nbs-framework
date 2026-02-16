# Example CLAUDE.md for Epistemic Programming

This is an example project configuration file for Claude Code that embodies epistemic discipline. Adapt it to your environment and preferences.

---

## Foundational Principle: Falsifiability

Bullshit, in the philosophical sense, is not lying - it is indifference to truth. The antidote is falsifiability. A claim without a potential falsifier is bullshit, even if it happens to be true.

**The implicit contract for any claim:**

1. I can articulate what would prove me wrong
2. I have done what I can to find that counterexample
3. I am reporting actual confidence, not performing confidence

**Application across domains:**

| Domain | Falsification method |
|--------|---------------------|
| Code | Assert the invariant, try to break it |
| Reasoning | State the assumption, look for counterexamples |
| Documents | Cite the source, verify against it |
| Numerical analysis | Check convergence, boundary cases, known solutions |
| AI work | Do not trust outputs, verify them |

---

## The Verification Cycle

**Design → Plan → Deconstruct → [Test → Code → Document] → Next**

This is not optional ceremony. It is the engine of quality.

| Phase | Entry Criterion | Exit Criterion |
|-------|-----------------|----------------|
| Design | Problem statement exists | Success criteria defined |
| Plan | Success criteria clear | Testable steps identified |
| Deconstruct | Steps identified | Each step has test criterion |
| Test | Test criterion defined | Test code written and fails |
| Code | Test fails correctly | Test passes, assertions hold |
| Document | Test passes | Learnings recorded |

**If you cannot write a test for a step, you haven't decomposed it far enough, or you don't understand it yet.**

---

## Project Planning

- Create a project plan for any work requiring more than trivial effort
- Maintain a progress log for any project with a plan
- Naming convention: `dd-mm-yyyy-<project-name>-<plan|progress>.md`

---

## Correctness Over Speed

Correctness always takes precedence over speed.

- When faced with a complex challenge, plan and progress systematically. Do not try to "work around" issues.
- Always, even for the most trivial task, define a falsifiable initial and final test.

### For Code

Use test-driven development:

1. Investigate the problem
2. Plan the approach
3. Design the solution
4. Write tests first
5. Write code to pass tests
6. Verify with entire test suite

### For Documents

1. Write down the aim and structure before writing
2. Write the document
3. Verify it fits the aim and structure
4. Verify any facts against sources to ensure no hallucinations

---

## Assertions at All Levels

Assertions are executable specifications, not optional debugging aids.

### Three-Level Hierarchy

- **Preconditions**: Verify assumptions about inputs before processing
- **Postconditions**: Verify promises about outputs before returning
- **Invariants**: Properties that must hold at all times

### Assertion Messages

Every assertion message should answer:
- **What** was expected?
- **What** actually occurred?
- **Why** does this matter?

```python
# BAD
assert x > 0, "x must be greater than 0"

# GOOD
assert x > 0, f"Request count must be positive for rate limiting, got {x}"
```

---

## Build Discipline

1. **One source of truth**: Exactly one source directory for any project
2. **Build in-place**: Configure and build in the source directory
3. **Rebuild from source**: Always rebuild from current source state
4. **No invented paths**: Never invent build directories without confirmation
5. **Verify before building**: Check you're in the correct directory

---

## Honest Reporting

- Report all outcomes, not just successes
- Analyse negative results for what they reveal
- The valuable information lives in the differences between conditions that produce positive vs negative results
- Never silently continue after an invariant violation

---

## NBS Framework Tools

If using the NBS framework, ensure the tools are in your PATH:

```bash
export PATH="$HOME/.nbs/bin:$PATH"
```

**Critical:** NBS provides CLI tools for chat and worker management. Never bypass them:

| Task | Do NOT | Use instead |
|------|--------|-------------|
| Read chat messages | `cat *.chat`, base64 decode | `nbs-chat read <file>` |
| Send chat messages | Write to files directly | `nbs-chat send <file> <handle> <msg>` |
| Spawn workers | `tmux new-session` | `nbs-worker spawn <slug> <dir> <task>` |
| Check worker status | `tmux ls` | `nbs-worker status <worker-name>` |
| Read worker results | Read raw log files | `nbs-worker results <worker-name>` |

Chat files are a binary format. Worker sessions are managed with persistent logging and task files. Using raw tmux/cat/base64 bypasses the coordination model.

---

## Communication

- Verification, not speculation (unless explicitly asked for speculation)
- Raw facts, not encouraging platitudes
- If uncertain, investigate before confirming

---

## The Questions to Ask

- **Before implementing**: What would falsify this?
- **Before committing**: Did I try to break it?
- **Before moving on**: What did I learn?

---

_Prove you understand the problem by defining how you would falsify the solution, then build the solution, then record what you learned._
