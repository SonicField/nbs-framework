---
description: Precise Technical English mode for unambiguous specifications
allowed-tools: Read, Write, Edit, Glob, Grep, AskUserQuestion
---

# NBS PTE

Precise Technical English (PTE) is a constrained subset of English designed to eliminate ambiguity in technical specifications. PTE has two complementary tools: **constrained prose** (RFC 2119 modals, explicit cardinality, condition-action structure) and **Honest type definitions** (Pascal-based data definitions for structures, states, and contracts). PTE prose states what agents MUST do. Honest definitions state what the data looks like. Together they eliminate the two main sources of ambiguity: vague verbs and undefined nouns.

---

## Activation

Two commands control PTE mode:

| Command | Effect |
|---------|--------|
| `/nbs-pte` | Enter PTE mode. All AI output uses Precise Technical English. |
| `/nbs-natural` | Exit PTE mode. AI returns to natural language. |

**Mode is sticky.** Once activated, PTE remains active until explicitly exited. New conversations inherit the mode setting.

---

## Two-Level Activation

PTE operates at two levels:

### Mode-Level (Human-Initiated)

The human invokes `/nbs-pte`. All subsequent AI output uses PTE until `/nbs-natural` is invoked.

### Statement-Level (AI Discretion)

Outside explicit PTE mode, the AI MAY use PTE for individual statements where precision matters. No permission required. Flag such statements with a `[PTE]` marker.

Example:
```
The cache expiration logic is straightforward. [PTE] IF the CacheEntry age exceeds the TTL, THEN the CacheManager MUST evict the CacheEntry BEFORE the next read operation.
```

---

## When to Activate

PTE mode is appropriate for:

| Context | Why |
|---------|-----|
| Worker task specifications | Requirements must be unambiguous |
| AI questions requiring precise answers | Misunderstanding causes rework |
| Discovery dialogues | Captured findings must be verifiable |
| Goal clarification | Terminal and instrumental goals need precision |

PTE is **not** for explanatory prose, design rationale, or informal communication.

---

## Worker Task Definitions

When writing worker task files (`.nbs/workers/worker-NNN.md`), use PTE for:

1. **Task description** - What the worker MUST accomplish
2. **Success criteria** - How completion is verified
3. **Constraints** - What the worker MUST NOT do

Example task description in PTE:
```
The Worker MUST create a test suite for the Parser module.
The test suite MUST contain ONE OR MORE test cases for EACH public function in Parser.
EACH test case MUST include EXACTLY ONE assertion.
The Worker MUST NOT modify the Parser source code.
IF all tests pass, THEN the Worker MUST update the Status to "completed".
```

Natural language for context and instructions remains acceptable. PTE is for the parts where ambiguity causes failure.

---

## Ambiguity Resolution Workflow

When the human's input contains ambiguity, follow this workflow:

### Step 1: Render Understanding

Write what you understood in PTE. This exposes gaps.

### Step 2: Identify Ambiguities

List each ambiguity as a single question. One question only - do not batch.

### Step 3: Ask One Question

Present the question to the human. Wait for answer.

### Step 4: Record Resolution Clause

Capture the answer in PTE:
```
RESOLUTION: [The PTE statement capturing the human's answer]
```

### Step 5: Repeat or Proceed

IF ambiguities remain, THEN return to Step 2.
IF no ambiguities remain, THEN proceed with the task.

### Example

**Human says:** "The cache should expire entries that are too old."

**AI identifies ambiguity:** "Too old" is undefined.

**AI asks:** "What is the maximum age for a CacheEntry before it expires?"

**Human answers:** "5 minutes."

**AI records:**
```
RESOLUTION: A CacheEntry is Expired IF the CacheEntry age exceeds 300 seconds.
IF the CacheEntry is Expired, THEN the CacheManager MUST evict the CacheEntry BEFORE the next read operation.
```

### When Resolution Fails

If the human cannot answer (genuinely undecided), record:
```
UNRESOLVED: [The ambiguity]
DEPENDENCY: [What must happen before this can be resolved]
```

This is honest. It surfaces the gap rather than hiding it.

---

## Presentation Format

Select format based on complexity:

| Complexity | Presentation |
|------------|--------------|
| Simple | PTE only. No gloss needed. |
| Complex | PTE followed by natural language gloss. |

The gloss aids human comprehension. The PTE is authoritative. When they conflict, PTE governs.

---

## Quick Reference: PTE Rules

### Actions (Verbs)

| Rule | Requirement |
|------|-------------|
| Active voice only | "The Validator validates" not "is validated" |
| RFC 2119 modals | MUST, MUST NOT, SHOULD, SHOULD NOT, MAY (capitalised) |
| One action per sentence | No compound sentences bundling multiple actions |
| Explicit temporal markers | BEFORE, AFTER, IMMEDIATELY AFTER, DURING, EVENTUALLY, UNTIL |
| Condition-action structure | IF [condition], THEN [agent] [modal] [action] [object]. |

### Objects (Nouns)

| Rule | Requirement |
|------|-------------|
| Define before use | Composition, identity criteria, state space |
| State qualification | "ValidatedRequest" not "the request, which has been validated" |
| Explicit cardinality | EXACTLY ONE, ZERO OR MORE, ONE OR MORE, AT MOST ONE, BETWEEN N AND M |
| No pronouns | Repeat the noun. Always. |
| No metonymy | Things do not act unless they are agents |
| Explicit relationships | CONTAINS, REFERENCES, IS-A-KIND-OF, CONSISTS-OF |

---

## Modal Verbs

| Modal | Meaning |
|-------|---------|
| MUST | Absolute requirement. Violation is a defect. |
| MUST NOT | Absolute prohibition. Violation is a defect. |
| SHOULD | Recommended. Deviation requires justification. |
| SHOULD NOT | Discouraged. Deviation requires justification. |
| MAY | Truly optional. No preference either way. |

No other modals permitted. "Could", "might", "would", "can" are banned.

---

## Cardinality Markers

| Marker | Meaning |
|--------|---------|
| EXACTLY ONE | Mandatory, singular |
| ZERO OR MORE | Optional collection |
| ONE OR MORE | Mandatory, non-empty collection |
| AT MOST ONE | Optional, singular |
| BETWEEN N AND M | Bounded collection |

---

## Temporal Markers

| Marker | Meaning |
|--------|---------|
| BEFORE | Strictly prior. Completed before the next action begins. |
| AFTER | Strictly subsequent. Begins only after the prior action completes. |
| IMMEDIATELY AFTER | No intervening actions permitted. |
| DURING | Concurrent. Overlapping execution. |
| EVENTUALLY | Will occur, but timing unspecified. |
| UNTIL | Continues while condition holds. |

---

## Honest Type Definitions in PTE

IF a PTE document defines a data structure, a protocol, a state machine, or a tool contract, THEN the document MUST include an Honest type definition for that structure. Honest is the data definition half of PTE — prose says what agents do, Honest says what the data looks like.

### Preamble requirement

Any document that contains Honest definitions MUST include a brief preamble before the first definition stating what Honest is and how to read it. The reader MAY be an agent who has never encountered Honest before. The preamble SHOULD be TWO TO THREE sentences. Example:

> The states and structures below are defined in Honest — a Pascal-based data definition language used for precise type specifications. Code blocks marked `pascal` in this document are Honest type definitions. They are authoritative: IF the PTE prose and the Honest definitions conflict, THEN the Honest definitions govern.

### Rules

1. IF a PTE document defines a data structure, a protocol, a state machine, or a tool contract, THEN the document MUST include an Honest type definition for that structure.
2. The Honest definition MUST appear in a `pascal` fenced code block.
3. The Honest definition is authoritative. IF the PTE prose and the Honest definition conflict, THEN the Honest definition governs.
4. The Honest definition MUST include comments (in `{ }` syntax) for constraints the type system cannot express.
5. The PTE prose MUST reference the Honest definition by type name. The PTE prose MUST NOT repeat field definitions that are already in the Honest definition.
6. The document MUST include a preamble before the first Honest definition (see above).

### Example

A PTE specification for a build result reporting contract:

> The BuildReporter MUST emit EXACTLY ONE `BuildResult` after EACH build completes. IF the build fails, THEN the BuildReporter MUST set the `outcome` field to `Failure`. The BuildReporter MUST record the elapsed time in the `duration_ms` field of `BuildResult`.

```pascal
type
  BuildOutcome = (Success, Failure, Timeout);

  BuildResult = record
    outcome     : BuildOutcome;
    duration_ms : LongInt;       { MUST be non-negative }
    output      : String;        { truncated to 65535 bytes if longer }
    exit_code   : Integer;
  end;
```

The PTE prose references `BuildResult` and `outcome` by name. The PTE prose does NOT restate the field list — the Honest definition is the authoritative field inventory.

---

## The Value Proposition

PTE front-loads the cost. The specification phase becomes slower and more tedious. The implementation phase becomes faster. Fewer wrong guesses. Fewer bugs that trace back to "I thought you meant..."

The AI does the tedious expansion. The human does the verification. Neither can do the other's job.

---

## The Contract

You render natural language into unambiguous form. The human verifies: "Yes, that is what I meant" or "No, you have misunderstood."

Ambiguity that would otherwise hide until implementation surfaces at specification.

_Precision is not pedantry. It is prevention._
