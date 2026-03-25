# Investigation Mode

An investigation is a hypothesis-driven side quest. You have a question that can only be answered by experiment — does this code actually do what we think? Is this a race condition or a logic error? What happens at the boundary?

Investigations are not features. They produce evidence, not artefacts.

## Starting One

Run `/nbs-investigation` at any point in a conversation. It is not part of the normal discovery/recovery flow — it stands alone.

The command walks through hypothesis identification: narrowing a vague concern ("something is wrong with the cache") into a falsifiable claim ("the LRU eviction fires before the entry expires"). No experiment runs until the hypothesis is confirmed.

## Branch Convention

All investigation work happens on a branch named `investigation/<topic>`. This isolates experiments from main development. The branch is preserved after the investigation concludes — it is a record, not a deliverable.

## INVESTIGATION-STATUS.md

Created in the project root at the start. Contains:

| Section | Purpose |
|---------|---------|
| Hypothesis | One sentence, falsifiable |
| Falsification criteria | What would prove it wrong |
| Experiment log | Raw observations per experiment |
| Verdict | Falsified / Failed to falsify / Inconclusive |

The experiment log records observations, not interpretations. Each entry states what was run, what result would falsify, what actually happened, and what it means.

## How /nbs Detects Investigation Context

Two breadcrumbs:

1. **Branch pattern** — current branch matches `investigation/*`
2. **Status file** — `INVESTIGATION-STATUS.md` exists in the project root

When `/nbs` finds both, it switches to investigation review: checking hypothesis quality, experiment design, and observation recording. It does not perform normal project review.

## Lifecycle

1. **Hypothesis** — identify and sharpen the claim until it is specific, falsifiable, and testable
2. **Isolate** — create the branch and status document
3. **Design** — plan experiments with clear pass/fail criteria; present to human for approval
4. **Execute** — run one experiment at a time, record raw results, update status, discuss before proceeding
5. **Verdict** — synthesise findings into one of three outcomes:
   - **Falsified**: evidence contradicts the hypothesis
   - **Failed to falsify**: evidence is consistent (not proof — confidence)
   - **Inconclusive**: no clear signal either way
6. **Return** — check out the main branch; investigation branch stays for reference

The human confirms each step. No bulk execution. Unexpected results get discussion, not dismissal — they are often the point.

## When to Use

- You suspect something but have not verified it
- Discovery raised questions that require execution to answer
- You want confidence before building on an assumption
- You think there is dead code, a race condition, or a subtle bug

The goal is falsification. You are trying to prove yourself wrong, not right.
