# Discovery and Recovery

Some projects were built without epistemic structure. No written goals. No triage of what worked. Results mixed with dead ends. Decisions made but never recorded.

Discovery maps the territory. Recovery rebuilds from the map.

## Discovery

`/nbs-discovery` is a collaborative, read-only process. It makes no changes. The human is the primary source of truth — the agent asks constantly and confirms before concluding.

### The Four Phases

**Phase 1: Establish Context.** Before searching for anything, ask the human what the project was trying to achieve. What timeframe. What locations. What they remember about dead ends. Do not proceed without answers.

**Phase 2: Archaeology.** Search each location the human suggests. List what exists. Present findings. Read only what the human confirms is relevant. Checkpoint after each location.

**Phase 3: Triage.** For each artefact, ask the human: what was this? Did it work? Keep, discard, or evaluate? Build a triage table together. The agent cannot determine value alone — it lacks the context of why something was built.

**Phase 4: Gap Analysis.** What exists is now known. What is missing? Identify 3-6 questions about the gap between current state and terminal goal. Work through them one at a time — one question, one answer, one confirmed restatement. Never batch. The confirmed restatements are the most valuable output: distilled human knowledge in verified form.

### The Discovery Report

Everything goes into a single report: terminal goal, artefacts found, triage table, gap analysis with full confirmed restatements, open questions, recommended next steps.

This report is the sole input to recovery. If it is not in the report, recovery will not have it.

## Discovery Verification

`/nbs-discovery-verify` runs after discovery and before recovery. It checks the report against a required-sections checklist: terminal goal, artefacts, triage, valuable outcomes, instrumental goals, confirmed understanding, open questions, next steps.

It also checks for context window leakage — things the human said during discovery that were summarised too aggressively or lost entirely. The confirmed restatements must appear in full, not compressed to one-liners.

The verification is a checkpoint, not a redo. It catches what the agent missed before the conversation ends and the context is gone.

## Recovery

`/nbs-recovery` is the action phase. It reads the discovery report and creates a step-wise plan. Each step is atomic, reversible, and described before execution. The human confirms each step individually.

Recovery preserves before moving. It archives rather than deletes. It stops on unexpected issues rather than powering through.

After restructuring, it establishes epistemic discipline: plan files, progress logs, version control, falsification criteria for preserved results. The project ends recovery with structure it lacked before.

## The Relationship

Discovery asks: what do we have, and what is it worth?
Recovery asks: how do we preserve what matters and build forward?

The boundary between them is deliberate. Discovery is read-only and collaborative. Recovery changes things and requires confirmation. Mixing the two — changing files while still uncertain about their value — is how artefacts get lost.
