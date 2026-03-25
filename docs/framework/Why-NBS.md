# Why NBS

NBS stands for No Bullshit. The name is blunt because the problem is blunt.

Bullshit is not lying. Liars know the truth and conceal it. Bullshitters are indifferent to truth. They say what sounds right, what fits the meeting, what satisfies the prompt. Whether it happens to be true is incidental.

Software engineering is drowning in bullshit. "Best practice" that nobody tested. Architecture decisions justified by vibes. AI-generated code that looks correct and compiles and passes the obvious tests and fails in production.

NBS is a framework for not doing that.

## The Core Question

Before any claim, ask: what would prove this wrong?

If you cannot answer, you do not understand what you are claiming. You are making noises shaped like assertions.

This is not philosophy for its own sake. It has immediate practical consequences.

## Worked Example: Adversarial Testing

You write a function to validate user input:

```python
def validate_email(email: str) -> bool:
    return "@" in email and "." in email
```

You test it:

```python
assert validate_email("user@example.com") == True
assert validate_email("invalid") == False
```

Both pass. You commit. The code is tested. You are done.

You have produced bullshit.

### What Would Prove This Wrong?

The function claims to validate email addresses. What would falsify that claim?

- `"@."` passes but is not valid
- `"user@localhost"` fails but is valid
- `"user@[192.168.1.1]"` fails but is valid
- `"user+tag@example.com"` passes (good) but you did not check
- `"user@example..com"` passes but is not valid

You did not try to break it. You tried to confirm it.

### The NBS Approach

1. **State the goal**: Validate email addresses according to RFC 5321, rejecting common attack vectors.

2. **State the falsifier**: Find an input that is accepted when it should be rejected, or rejected when it should be accepted.

3. **Try to falsify**: Generate adversarial inputs. Edge cases. Malformed data. Known attack patterns.

4. **Report honestly**: The simple regex fails on 40% of edge cases. Either accept limited validation and document the constraints, or use a proper parser.

The difference is not sophistication. The difference is asking what would prove you wrong and actually looking.

## Why "Epistemic"?

Epistemology is the study of knowledge. What can we know? How do we know it? When are we wrong?

These are not abstract questions. Every line of code embodies an epistemic claim: this is correct. Every test embodies an epistemic method: this is how I checked.

NBS applies epistemology to engineering. The fancy word is just there to locate the concepts in existing philosophy. The practice is simple: do not bullshit yourself about what you know.

## The Pillars

NBS rests on several concepts, each documented separately:

| Concept | Core Question |
|---------|---------------|
| Goals | What are we actually trying to achieve? |
| Falsifiability | What would prove this wrong? |
| Rhetoric | Who needs to be convinced, and of what? |
| Verification | How do we check at each stage? |

None stands alone. Goals without falsifiability are wishful thinking. Falsifiability without goals is undirected scepticism. Rhetoric without verification is persuasion without warrant.

## The Practice

Run `/nbs` during work. It asks uncomfortable questions:

- What is the terminal goal?
- What would falsify the current approach?
- Is this report honest about confidence?
- What does this failure reveal?

The discomfort is not a bug. Comfortable collaboration produces comfortable bullshit.

## Summary

NBS is not a methodology. It does not prescribe how to work. It prescribes how to think about whether your work is any good.

The answer is often: not as good as you thought. That is the point.
