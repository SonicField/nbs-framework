---
description: "Oracular Speech mode for compressed insight and metaphor"
allowed-tools: Read, Write, Edit
---

# NBS Oracular Speech

Oracular Speech is a constrained mode of expression designed to compress insight into forms that resist passive consumption. Where PTE eliminates ambiguity, Oracular Speech introduces productive ambiguity — the kind that forces interpretation.

---

## Activation

| Command | Effect |
|---------|--------|
| `/nbs-oracular-speech` | Enter Oracular Speech mode for current output. |

Oracular Speech is **not sticky**. It applies to the current output only. After delivery, the AI returns to its previous register. This is a burst mode, not a sustained state.

---

## When to Use

Oracular Speech is appropriate when:

| Context | Why |
|---------|-----|
| Pythia Six-Month Regret section | Speculative insight needs cognitive friction |
| Risk summaries that would otherwise be skimmed | Metaphor forces engagement |
| Naming consequences that are hard to express directly | Indirection reaches where precision cannot |

Oracular Speech is **not** for:

- Technical specifications (use PTE)
- Bug reports
- Task descriptions
- Test criteria
- Anything where misunderstanding causes immediate failure

---

## The Register

### Koans

A koan is a compressed contradiction or paradox that illuminates by resisting easy resolution.

**Good koans:**
- *What is a doughnut to one who does not believe in holes?*
- *A cache that never forgets it has forgotten is indistinguishable from truth.*
- *The lock that opens for everyone protects nothing it was built to guard.*
- *Many finite truths map orthogonally on an infinite reality.*

**Bad koans:**
- *Things might go wrong.* — Not a koan. Just vague.
- *The system is like a river that flows.* — Dead metaphor. No insight.
- *Consider the lilies of the field.* — Borrowed, not earned. The metaphor must arise from the specific situation.

### Properties of Good Oracular Speech

1. **Arises from the specific situation.** The metaphor maps onto the actual technical risk. A koan about locks should be about access control; a koan about memory should be about caching or state.

2. **Resists passive consumption.** The reader cannot nod and move on. The statement requires a second read, a moment of interpretation. This is the cognitive friction that justifies the register.

3. **Compresses a complex insight.** The koan captures something that would take a paragraph to say in PTE. The compression is not decorative — it is functional.

4. **Is followed by grounding.** Oracular Speech never stands alone in NBS. Every koan is followed by its concrete interpretation — the D-timestamp citations, the specific scenario, the actionable detail. The koan frames; the explanation grounds.

### The Pattern

```
<oracular sentence>
<concrete scenario with D-timestamp citations and specific consequences>
```

Example:
> *A lock that opens for everyone protects nothing it was built to guard.*
> The decision to skip auth for internal endpoints (D-1707526800) assumes the network perimeter holds. If the API is ever exposed — even briefly for a demo or partner integration — every endpoint is open. Retrofitting auth into a running system with established clients is an order of magnitude harder than adding it at build time.

The oracular sentence carries the emotional and conceptual weight. The grounding carries the technical specificity. Both are required. A koan without grounding is mystification. Grounding without a koan is a risk report that gets skimmed.

---

## Relationship to PTE

PTE and Oracular Speech are complementary registers.

| PTE | Oracular Speech |
|-----|-----------------|
| Eliminates ambiguity | Introduces productive ambiguity |
| Active voice, RFC 2119 modals | Metaphor, paradox, compression |
| One action per sentence | One insight per sentence |
| For specifications and criteria | For foresight and reflection |
| Resists misunderstanding | Resists passive acceptance |

In a Pythia checkpoint:
- Sections 1–3 (Hidden Assumption, Second-Order Risk, Missing Validation) use **PTE** or near-PTE technical precision
- Section 4 (Six-Month Regret) opens with **Oracular Speech**, followed by concrete grounding
- Section 5 (Confidence) uses standard technical English

---

## The Delphic Precedent

The Pythia at Delphi spoke in riddles not because she lacked clarity but because her petitioners lacked readiness. A direct answer to "should we go to war?" would be accepted or rejected on authority. A riddle — "if you cross the river, a great empire will be destroyed" — forced the petitioner to ask *which* empire. The ambiguity was the message.

Oracular Speech in NBS serves the same function. A team that reads "cache invalidation risk in multi-process deployment" nods and adds it to the backlog. A team that reads "a cache that never forgets it has forgotten is indistinguishable from truth" pauses. The pause is the value.

---

## Quick Reference

| Element | Rule |
|---------|------|
| Length | One sentence. Two at most. |
| Source | Must arise from the specific technical situation |
| Dead metaphors | Banned. No rivers flowing, no ships sailing. |
| Borrowed wisdom | Banned. No quoting scriptures, proverbs, or famous sayings. |
| Follow-up | Always. Every koan gets concrete grounding immediately after. |
| Standalone use | Never. Oracular Speech without grounding is mystification. |

---

_Precision prevents misunderstanding. Metaphor prevents complacency. Both are needed; neither is sufficient._
