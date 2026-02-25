---
description: "NBS Chat Digest: Extract learnings from chat files"
allowed-tools: Bash, Read, Write
---

# NBS Chat Digest

Extract structured learnings from `.chat` files. Produces a sanitised summary safe to commit or share — no sensitive data in the output.

## When to Use

- At project milestones (phase gates, releases)
- After a significant multi-participant chat session
- When archiving chat files before cleanup
- When the supervisor wants to capture process learnings

## Instructions

### 1. Read the Chat

Read the full chat file directly using the Read tool (not `--last=999` which may truncate). For very large files, read in chunks using offset/limit parameters.

### 2. Analyse

Extract four categories of information:

**Decisions** — Architectural or process choices made during the chat.
For each decision, capture:
- What was decided
- Why (the rationale, including alternatives considered)
- Who was involved (roles, not handles — described by function)
- **Status**: active, or superseded (if a later decision reversed it)

If a decision was later overturned, mark it as **Superseded by:** and reference the replacing decision. This is important — omitting reversed decisions hides the reasoning process.

**3Ws — What Went Well**
- Effective coordination patterns
- Tools or processes that worked
- Good decisions that paid off

**3Ws — What Didn't Work**
- Bugs discovered through live use (not design review)
- Miscommunication or coordination failures
- Tool limitations that caused friction

**3Ws — What We Can Do Better**
- Process improvements identified
- Tool changes needed
- Patterns to adopt or avoid

### 3. Sanitise

Remove from the output:
- Absolute file paths (generalise to relative or descriptive)
- User handles and names (use roles that describe function — e.g. "supervisor", "documentation lead", "benchmark lead", "human". Roles are not a fixed list; use whatever describes the participant's function in the conversation)
- Project-specific identifiers (task IDs, diff numbers, internal URLs)
- Any credentials, tokens, or secrets

Keep:
- Technical patterns and architectural reasoning
- Process observations
- Tool behaviour and limitations
- Reusable learnings

### 4. Write the Digest

Write the output to a file. Default location: `.nbs/digests/<date>-<topic>.md`

For long conversations that span multiple phases or topics, split into sections by phase. Each phase gets its own Decisions and 3Ws blocks.

Format:

```markdown
# Chat Digest: <topic>

Date: <YYYY-MM-DD>
Participants: <N> (roles: <list of functional roles>)
Messages: <count> (message sends, not lines)

## TL;DR

<2-3 sentence summary of what this chat session was about and the key outcomes. A reader with no context should understand the significance after reading this.>

## Context

<Brief description of the project, what stage it was at, and why this conversation happened. Assume the reader has no familiarity with the subject. Include enough background that the decisions and learnings make sense on their own.>

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

### 5. Verify

Spot-check at least 3 claims in the digest against the source chat. Confirm:
- Decisions are accurately represented (not misattributed or mischaracterised)
- Reversed decisions are marked as superseded
- No sensitive data leaked through

### 6. Report

Tell the user where the digest was written using the **absolute path** (not relative — humans need the full path to find the file). Summarise the key findings in 2-3 sentences. If the request came via a chat channel, post the absolute file path back to the chat so the requester can open it (e.g. with `/edit` in nbs-chat-terminal).

## Arguments

The skill accepts an optional argument: the path to the chat file to digest.

```
/nbs-chat-digest .nbs/chat/live.chat
```

If no argument is given, prompt the user for which chat file to digest.

## Important

- The digest must be **self-contained** — readable without access to the original chat.
- The digest must be **safe to commit** — no sensitive data.
- Be honest about what didn't work. The value is in the learnings.
- If the chat contains no substantive decisions or learnings, say so.
- Message count means number of message sends, not lines of text.
