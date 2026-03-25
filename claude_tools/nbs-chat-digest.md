---
description: "NBS Chat Digest: Extract learnings from chat files"
allowed-tools: Bash, Read, Write, Task
---

# NBS Chat Digest

Extract structured learnings from `.chat` files. Produces a sanitised summary safe to commit or share.

## When to Use

- At project milestones (phase gates, releases)
- After a significant multi-participant chat session
- When archiving chat files before cleanup
- Automatically during team restart (via `nbs-digest-spawn`)

## Instructions

### 1. Measure the Chat

```bash
nbs-chat participants <file>
```

Count the total messages. This determines whether to read directly or split into parallel sub-agents.

### 2. Read the Chat

**Under 200 messages:** Read the full file directly using the Read tool.

**Over 200 messages:** Split into chunks using `--last` and `--offset`:

```bash
# 5 chunks of 100 messages each from the last 500
nbs-chat read <file> --last=100                # newest 100
nbs-chat read <file> --last=100 --offset=100   # 100-200 from end
nbs-chat read <file> --last=100 --offset=200   # 200-300 from end
nbs-chat read <file> --last=100 --offset=300   # 300-400 from end
nbs-chat read <file> --last=100 --offset=400   # oldest 100 of the 500
```

Launch one sub-agent per chunk in parallel using the Task tool. Each sub-agent summarises:
- **Decisions made** in its window
- **Blockers** encountered and how they were resolved
- **3Ws** observations (what worked, what didn't, what to improve)
- **Key outcomes** (commits, test results, benchmarks)

### 3. Analyse

Extract four categories:

**Decisions** — What was decided, why, who was involved (roles, not handles), status (active or superseded).

**What Went Well** — Effective patterns, good decisions, tools that worked.

**What Didn't Work** — Bugs, miscommunication, tool limitations.

**What We Can Do Better** — Process improvements, tool changes, patterns to adopt or avoid.

### 4. Sanitise

Remove: absolute file paths, user handles (use roles), project-specific IDs, credentials.

Keep: technical patterns, architectural reasoning, process observations, reusable learnings.

### 5. Write the Digest

Default location: `.nbs/digests/<date>-<topic>.md`

Format:

```markdown
# Chat Digest: <topic>

Date: <YYYY-MM-DD>
Participants: <N> (roles: <list of functional roles>)
Messages: <count>

## TL;DR

<2-3 sentences. A reader with no context should understand the significance.>

## Context

<Brief background. Assume no familiarity with the subject.>

## Decisions

### <Decision Title>
**Status:** Active | Superseded by <other decision>
**Decided:** <what>
**Rationale:** <why>
**Alternatives considered:** <what was rejected and why>

## What Went Well

- <observation>

## What Didn't Work

- <observation>

## What We Can Do Better

- <observation>
```

### 6. Continuation Analysis

After analysing what happened, reason about **what should happen next**. This section is critical — restarted agents use it to determine their direction.

Examine the final messages for:
- Explicit next steps, options, or "paths forward" proposed by any participant
- Open tasks, blocked work, or identified follow-ups
- Whether the session ended cleanly (deliberate close by supervisor or the human leader) vs crash/timeout
- Unresolved questions or decisions deferred to the human leader

Produce one of two continuation types:

**CONTINUATION: GOALS** — when you can identify specific, actionable goals from the conversation:
```
CONTINUATION: GOALS
1. <specific goal extracted from conversation>
2. <specific goal extracted from conversation>
Source: <quote or paraphrase from the message that proposed this>
```

**CONTINUATION: REVIEW** — when no clear goals exist, or the session ended without forward direction:
```
CONTINUATION: REVIEW
The previous session ended with <brief description>. No explicit next steps were identified.
The team should: review the scribe log and prior session outcomes, then propose 3 candidate goals
to the human leader with expected outcomes for each. Do not begin work until the human leader confirms a direction.
```

**Rules for continuation:**
- Never invent goals that weren't discussed. Only extract what was actually proposed.
- If the human leader gave a direction (e.g., "accept standings", "focus on X"), that IS the continuation.
- If the session closed cleanly with "session complete" and no forward direction, use REVIEW.
- If the session crashed or timed out mid-work, use GOALS with the interrupted work as the goal.

### 7. Post to Chat

Post the full digest (including the Continuation section) to chat. This is the primary output — restarted agents read it on startup.

```bash
nbs-chat send <chat-file> <handle> "CHAT DIGEST:

<full digest content including Continuation section>"
```

Also write to `.nbs/digests/<date>-<topic>.md` as a permanent record.

### 8. Verify

Spot-check at least 3 claims against the source chat. No sensitive data, no misattributed decisions. Verify the Continuation section accurately reflects proposals from the conversation — no invented goals.

## Arguments

```
/nbs-chat-digest <chat-file>
```

If no argument is given, prompt the user for which chat file to digest.

## Important

- The digest must be **thorough** — at least 200 lines. Cover every decision, every 3W, every key technical outcome. This is institutional memory; brevity here means lost context.
- The digest must be **self-contained** — readable without the original chat.
- The digest must be **safe to commit** — no sensitive data.
- Be honest about what didn't work. The value is in the learnings.
- Message count means number of message sends, not lines of text.
