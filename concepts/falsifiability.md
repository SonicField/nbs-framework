# Falsifiability

You cannot prove code correct. You can prove it wrong.

A single counterexample demolishes a claim. A thousand passing tests merely fail to falsify. This asymmetry is not a limitation - it is the only epistemically honest stance available.

## The Contract

Any claim worth making carries three obligations:

1. I can articulate what would prove me wrong
2. I have tried to find that counterexample
3. I am reporting actual confidence, not performing confidence

A claim without a potential falsifier is not wrong. It is not even wrong. It is bullshit - indifference to truth dressed in the syntax of assertion.

## Application

| Domain | Falsification method |
|--------|---------------------|
| Code | Assert the invariant, try to break it |
| Reasoning | State the assumption, seek counterexamples |
| Documents | Cite the source, verify against it |
| Numerical work | Check convergence, boundaries, known solutions |
| AI output | Do not trust. Verify. |
| Methodology | State the conditions under which the process is wrong. Test against outcomes. |

## Falsifying the Process

Rules and documents are frozen claims. They encode evidence from a specific context — but contexts change, and a rule without a revision condition is unfalsifiable authority.

"No X under any circumstances" is a strong claim. It carries the same three obligations as any other: what would prove it wrong, have we looked, and are we reporting honestly? A document that says "NO EXCEPTIONS" has declared itself beyond falsification. That is Ethos dressed as Logos.

The terminal weathering pivot from Rust to C is a concrete example. The documentation said "RUST ONLY. NO C. NO EXCEPTIONS." When measurements showed Rust could not access the layer where the overhead lived, the evidence falsified the rule. The rule was not wrong when written — it was correct in its original context. It became wrong when the context changed. A rule that cannot accommodate new evidence is not rigorous. It is brittle.

The practical question for any process rule: under what measured outcome would we change this? If you cannot answer, the rule is not a tool. It is a ritual.

## Layered Confidence

Tests and assertions colour in the state space where invariants hold. This is not proof - it cannot be, in the general case - but it builds a foundation for further invariants.

Rigour compounds. So does sloppiness.

A test that cannot fail is not a test. A claim that cannot be wrong is not knowledge. This principle underlies all work: code, reasoning, documents, analysis, collaboration.

## Property-Based Testing

Traditional tests confirm: "does it work for these examples?" Property-based testing falsifies: "can I find any input that breaks it?"

The technique: define properties that must always hold, then generate many inputs to search for counterexamples. The framework (Hypothesis, QuickCheck, proptest) automates the search and shrinks failing cases to minimal reproductions.

```python
# Example-based: confirms one case
def test_sort_example():
    assert sort([3, 1, 2]) == [1, 2, 3]

# Property-based: mechanised falsification
from hypothesis import given, strategies as st

@given(st.lists(st.integers()))
def test_sort_properties(data):
    result = sort(data)
    # Property 1: output is sorted
    assert all(result[i] <= result[i+1]
               for i in range(len(result)-1))
    # Property 2: output is a permutation of input
    assert sorted(result) == sorted(data)
    # Property 3: idempotent
    assert sort(result) == result
```

The first test confirms one case. The second tries to falsify three properties across thousands of generated inputs. If it cannot find a counterexample, confidence is earned — not assumed.

For any function, systematically generate adversarial inputs: empty inputs, boundary values (MAX_INT, MIN_INT, epsilon), type confusion (string "0" vs integer 0), resource exhaustion (very large inputs), malformed data (invalid UTF-8, truncation), and timing attacks (race conditions, reordering).

Property-based testing is falsifiability automated. Use it wherever properties can be stated.

## The Practical Question

Before committing to any choice, ask: what evidence would show this is the wrong choice? If you cannot answer, you do not yet understand the choice you are making.

---

## Pillar Check

Have you read all pillars in this session?

- goals.md
- falsifiability.md *(you are here)*
- rhetoric.md
- bullshit-detection.md
- verification-cycle.md
- zero-code-contract.md
- engineering-standards.md

If you cannot clearly recall reading each one, read them now. Next: `rhetoric.md`
