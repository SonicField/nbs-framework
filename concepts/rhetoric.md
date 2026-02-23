# Rhetoric

Aristotle proposed three modes of persuasion: Ethos (authority), Pathos (what people want), Logos (logic). Most engineers believe they operate purely in Logos. They are mistaken.

## The Hidden Pathos

The logical requirements of any system are an attempt to meet the emotional requirements of the people paying for it. By not being explicit about this, we create constant misalignment between the real world - "I need to please my client" - and the imagined logical world - "I need to please the type checker."

Logic is tautologically correct. It has no truth value outside itself. When we attempt purely logical work, we smuggle in Pathos - a moral or aesthetic motivation - to convert the tautology into something externalisable.

When someone insists that functional programming is "cleaner" or a particular architecture is "more elegant," they are making Pathos claims dressed in Logos clothing. There is nothing wrong with this. But call it what it is.

## The Ethos Problem

We trust code that:
- Currently runs in production (survival bias)
- Was written by a respected person or organisation (authority)

There is no logical basis for this trust. A library from a known author gets adopted over a better alternative from an unknown one. "Best practices" propagate through authority, not demonstrated superiority.

## The Information Problem

Operating in the wrong mode is one failure. Operating with incomplete information is another.

Before proceeding on assumption, exhaust available sources:

| Source | What it provides |
|--------|------------------|
| The human | Intent, context, experience, constraints you cannot see |
| Existing documentation | What has already been learned and recorded |
| Available tools | Searches, databases, APIs that contain answers |
| The system itself | What the code actually does, not what you assume |

Guessing when you could verify is Ethos failure - trusting your own assumed knowledge over available evidence.

Asking is not weakness. It is the only honest response to uncertainty.

## The Practical Questions

1. Which mode am I operating in? Is that the right mode for this decision?
2. Am I trusting authority (including my own) over evidence?
3. What information sources have I not consulted?
4. Does the human have context I am missing?

---

## Pillar Check

Have you read all pillars in this session?

- goals.md
- falsifiability.md
- rhetoric.md *(you are here)*
- bullshit-detection.md
- verification-cycle.md
- zero-code-contract.md
- engineering-standards.md

If you cannot clearly recall reading each one, read them now. Next: `bullshit-detection.md`
