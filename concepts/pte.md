# Precise Technical English (PTE)

Natural language fails at precision. Not because English lacks expressive power, but because it optimises for the wrong thing: speaker economy. Humans compress. They rely on context, shared assumptions, and the listener's willingness to infer. This works in conversation. It fails in specifications.

PTE is a constrained subset of English designed for one purpose: eliminating ambiguity in technical communication. It is verbose, tedious, and unnatural. These are features. The goal is mutual intelligibility when precision is non-negotiable.

## Why PTE Exists

Consider: "The service calls the API. It fails."

Which failed? The service? The API? The call? Natural English does not say. The reader guesses. The guess becomes an assumption. The assumption becomes code. Sometimes the code is wrong.

This is not an edge case. Every specification contains dozens of such ambiguities:

| Failure mode | Example | What goes wrong |
|--------------|---------|-----------------|
| Pronoun ambiguity | "It fails" | Which noun? |
| Modal confusion | "should validate" | Obligation? Recommendation? |
| Temporal vagueness | "after processing" | Immediately? Eventually? |
| Scope ambiguity | "old men and women" | Are the women old? |
| Metonymy | "the database returned an error" | Databases do not act |
| Category slippage | "the user" | Person? Account? Session? |

Each ambiguity is a potential bug. Each bug traces back to "I thought you meant..."

## The Connection to Falsifiability

A claim without a potential falsifier is bullshit. A specification without precise semantics is the same thing dressed in technical prose.

PTE makes specifications falsifiable. When the PTE says "IF the Response status is Error, THEN the Client MUST retry the Request", you can check:
- Did the Response have an Error status?
- Did the Client retry?
- If yes and no, the implementation violates the specification.

Vague specifications cannot be falsified because they accommodate any interpretation. PTE removes the accommodation.

## The Rules

PTE constrains both verbs (what happens) and nouns (what exists). The constraint is not arbitrary - each rule addresses a specific class of ambiguity.

### Verbs (Actions)

**Active voice only.** Passive voice hides the agent. "The input is validated" says nothing about who validates. Write: "The Validator validates the Input."

**RFC 2119 modals.** English modals (should, could, might) carry inconsistent meanings. PTE uses capitalised RFC 2119 terms with fixed semantics:

| Modal | Meaning |
|-------|---------|
| MUST | Absolute requirement. Violation is a defect. |
| MUST NOT | Absolute prohibition. Violation is a defect. |
| SHOULD | Recommended. Deviation requires justification. |
| MAY | Truly optional. No preference either way. |

No other modals are permitted. "Could", "might", "would", "can" are banned.

**One action per sentence.** Compound sentences bundle actions, creating ordering and conditional ambiguity. Split them.

**Explicit temporal markers.** English tense is insufficient. Use BEFORE, AFTER, IMMEDIATELY AFTER, DURING, EVENTUALLY, UNTIL.

**Condition-action structure.** All conditionals follow: IF [condition], THEN [agent] [modal] [action] [object]. No implicit conditionals. No embedded conditions.

### Nouns (Objects)

**Define before use.** Every noun must have a definition stating composition, identity criteria, and state space. What is a Request? What makes two Requests identical? What states can a Request occupy?

**State qualification.** When an action depends on state, the state is part of the noun. A ValidatedRequest and a PendingRequest are different types, not the same type with a flag.

**Explicit cardinality.** Singular and plural are insufficient. Use: EXACTLY ONE, ZERO OR MORE, ONE OR MORE, AT MOST ONE, BETWEEN N AND M.

**No pronouns.** Pronouns create reference ambiguity. Repeat the noun.

**No metonymy.** Things do not act unless they are agents. "The API threw an exception" is false. Write: "The APIClient raised an Exception."

**Explicit relationships.** State relationship types: CONTAINS, REFERENCES, IS-A-KIND-OF, CONSISTS-OF.

## Extended Scope

PTE goes beyond modal verbs but stops short of pseudo-code:

| Category | Coverage |
|----------|----------|
| Verbs | Actions with explicit agents, RFC 2119 modals |
| Nouns | Defined entities with identity criteria and state space |
| Conditions | IF/THEN structure with evaluable predicates |
| Temporal | BEFORE/AFTER/DURING/UNTIL/EVENTUALLY markers |
| Cardinality | EXACTLY ONE/ZERO OR MORE/ONE OR MORE/AT MOST ONE |

The reader should not need to know a programming language. But the structure is rigid enough that translation to code is mechanical.

## The Workflow

Humans do not write PTE directly. They lack the patience and working memory. Instead:

1. Human writes requirements in natural language
2. AI renders the requirements into PTE
3. Human verifies: "Yes, that is what I meant" or "No, you have misunderstood"
4. Where ambiguity prevents rendering, AI asks exactly one question
5. Human answers; AI incorporates the answer
6. Process repeats until no ambiguities remain

The output is a hybrid document: natural language for context, PTE for the sections where ambiguity is intolerable.

## When to Use PTE

**Appropriate for:**
- Interface contracts: what does this accept, return, under what conditions?
- State machines: what states exist, what transitions are valid?
- Invariants: what must always be true?
- Failure modes: what can go wrong, what happens when it does?
- Preconditions and postconditions

**Not appropriate for:**
- Explanatory prose, tutorials, design rationale
- User-facing documentation
- Informal communication

## The Value Proposition

Ambiguity is debt. A vague requirement becomes a guess in design, becomes an assumption in implementation, becomes a bug in production. Each translation loses fidelity.

PTE front-loads the cost. The specification phase becomes slower and more tedious. But the implementation phase becomes faster. Fewer wrong guesses. Fewer bugs that trace back to misunderstanding.

The AI does the tedious expansion. The human does the verification. Neither can do the other's job. Together, they can produce specifications that would be impractical to write by either alone.

---

## Pillar Check

Have you read all pillars in this session?

- goals.md
- falsifiability.md
- rhetoric.md
- bullshit-detection.md
- verification-cycle.md
- zero-code-contract.md
- pte.md *(you are here)*
- engineering-standards.md

If you cannot clearly recall reading each one, read them now. Next: `goals.md`
