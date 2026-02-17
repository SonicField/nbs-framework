# Testing Strategy

This document explains the testing philosophy and approach used in the NBS framework.

## The Problem

AI output is non-deterministic. The same prompt can produce different outputs across runs. String matching fails. Traditional unit testing does not apply.

## The Solution: AI Evaluates AI

A second AI instance evaluates the output against explicit criteria.

**The pattern**:
1. Run the command on a known scenario
2. A second Claude instance evaluates the output
3. The verdict is written to a JSON file
4. Exit code reflects the verdict

```bash
# Run the command
RESULT=$(claude -p "/nbs" --output-format text)

# Evaluate with a second AI
EVAL_PROMPT="Evaluate this output against these criteria..."
VERDICT=$(echo "$EVAL_PROMPT" | claude -p - --output-format text)

# Extract and act on verdict
if echo "$VERDICT" | grep -q '"verdict".*"PASS"'; then
    exit 0
else
    exit 1
fi
```

The verdict file is the deterministic state of truth. The test is falsifiable: explicit criteria define what causes failure.

## Two Testing Modes

| Mode | Tool | When to Use |
|------|------|-------------|
| One-shot | `claude -p` | Evaluate output, no interaction needed |
| Interactive | pty-session | Multi-turn, permission prompts, AskUserQuestion |

**One-shot**: Feed a prompt, get output, evaluate. Fast and simple. Use for most tests.

**Interactive**: Manage a persistent Claude session. Send commands, wait for prompts, respond. Use when testing requires multi-turn interaction. See [Interactive Testing](interactive-testing.md).

## Adversarial Testing

Tests come in pairs:

| Type | Purpose | Naming |
|------|---------|--------|
| Confirmational | Verify correct behaviour occurs | `test_<feature>.sh` |
| Adversarial | Verify wrong behaviour does NOT occur | `test_<feature>_adv_*.sh` |

**Example**:

- `test_investigation_file.sh` - When `INVESTIGATION-STATUS.md` exists at root, AI should dispatch to investigation review
- `test_investigation_adv_no_normal.sh` - When file exists at root, AI should NOT produce normal review

Both are needed. A test that only checks for correct behaviour can pass even if the system also produces incorrect behaviour in other cases.

**Adversarial test pattern**:
```bash
# Check for markers of wrong behaviour
if echo "$OUTPUT" | grep -q "## Status" && echo "$OUTPUT" | grep -q "## Recommendations"; then
    IS_NORMAL_REVIEW=true
fi

# Adversarial condition: should NOT have produced normal review
if [[ "$IS_NORMAL_REVIEW" == true ]]; then
    VERDICT="FAIL"
    REASONING="Produced normal review when investigation file was present"
fi
```

## Test Isolation for AI Systems

Traditional software testing isolates dependencies. AI testing must also isolate *context*.

**The problem**: Claude can see and reason about visible files. If she sees test infrastructure, she may reason about the meta-situation rather than behaving as she would in production.

**Case study**: A test for investigation dispatch failed because Claude saw:
- Test files named `test_investigation_*.sh`
- A plan file about "investigation dispatch testing"
- An `INVESTIGATION-STATUS.md` in a `scenarios/` subdirectory

Claude correctly deduced she was being tested and asked for clarification instead of following dispatch logic.

**Solution**: Run tests in isolated temporary repositories.

```bash
TEST_REPO=$(mktemp -d)
cd "$TEST_REPO"
git init -q
# Create only the files the test scenario needs
# No test infrastructure, no planning docs, no scenarios/ subdirectory
```

The AI sees a realistic scenario, not a test harness.

**Key insight**: The AI's context window is part of her input. Everything visible shapes behaviour, including test infrastructure.

See [~/docs/ai-meta-reasoning-in-testing.md](~/docs/ai-meta-reasoning-in-testing.md) for detailed analysis.

## Direct Pattern Matching vs AI Evaluation

For adversarial tests, direct pattern matching is often more reliable than AI evaluation.

**Why**: The evaluator AI can also reason about meta-context. It may refuse to evaluate, ask questions, or produce unexpected output.

**Pattern matching example**:
```bash
IS_INVESTIGATION_REVIEW=false
if echo "$OUTPUT" | grep -qi "hypothesis.*falsif\|experiment\|investigation review"; then
    IS_INVESTIGATION_REVIEW=true
fi
```

**When to use which**:

| Approach | When |
|----------|------|
| AI evaluation | Complex semantic judgement ("is this a good investigation?") |
| Pattern matching | Presence/absence of specific markers ("is this an investigation review format?") |

## Test Structure

```
tests/
├── automated/           # Automated tests
│   ├── scenarios/       # Test fixtures with known ground truth
│   ├── verdicts/        # Generated outputs (git-ignored)
│   └── test_*.sh        # Test scripts
└── manual/              # Human QA procedures
```

See [tests/README.md](../tests/README.md) for running tests and adding new ones.
