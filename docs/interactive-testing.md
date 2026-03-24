# Interactive Testing with Claude

This guide covers testing Claude Code behaviour that requires multi-turn interaction.

## When to Use Interactive Testing

| Scenario | Method |
|----------|--------|
| Evaluate output from a single prompt | `claude -p` (one-shot) |
| Test behaviour that requires AskUserQuestion | nbs-ts (interactive) |
| Test permission prompt handling | nbs-ts |
| Test multi-turn conversation flow | nbs-ts |

Use `claude -p` when you can evaluate the output in isolation. Use nbs-ts when the test requires responding to prompts or observing interactive behaviour.

## The Pattern

```bash
SESSION_NAME="test_$$"
TEST_REPO=$(mktemp -d)

# Setup cleanup
cleanup() {
    HANDLE=$(nbs-ts find "$SESSION_NAME" 2>/dev/null) && nbs-ts kill "$HANDLE" 2>/dev/null || true
    rm -rf "$TEST_REPO"
}
trap cleanup EXIT

# Create test environment (see "Working Directory" below)
cd "$TEST_REPO"
git init -q
# ... create test files ...

# Start Claude
HANDLE=$(nbs-ts create --name="$SESSION_NAME" "cd '$TEST_REPO' && claude")

# Handle trust prompt (poll for 'trust' in output)
for i in $(seq 1 30); do
    if nbs-ts read-new "$HANDLE" --strip | grep -q 'trust'; then
        nbs-ts send "$HANDLE" ''  # Accept with Enter
        sleep 2
        break
    fi
    sleep 1
done

# Wait for main prompt
for i in $(seq 1 30); do
    if nbs-ts read-new "$HANDLE" --strip | grep -q 'Welcome'; then break; fi
    sleep 1
done

# Send command
nbs-ts send "$HANDLE" '/nbs'
sleep 1
nbs-ts send "$HANDLE" ''  # Extra Enter for submission

# Wait for processing
sleep 60

# Capture and evaluate
nbs-ts read-new "$HANDLE" --strip > "$OUTPUT_FILE"
```

## Gotchas

### Trust Prompt

When Claude enters a new directory, she shows "Do you trust the files in this folder?" This blocks all input until accepted.

**Solution**: Poll for 'trust' pattern and send Enter to accept.

```bash
for i in $(seq 1 30); do
    if nbs-ts read-new "$HANDLE" --strip | grep -q 'trust'; then
        nbs-ts send "$HANDLE" ''
        sleep 2
        break
    fi
    sleep 1
done
```

### AskUserQuestion Rendering

AskUserQuestion does not render as plain text. It appears as a selection UI:

```
☐ Investigation?

I found an INVESTIGATION-STATUS.md file...

❯ 1. Active investigation
  2. Test fixture / old file
```

**Detection patterns**:
```bash
if echo "$OUTPUT" | grep -q "☐\|❯ 1\.\|1\. Active"; then
    ASKED_USER=true
fi
```

### Double Enter

Some prompts require:
1. Enter after typing the command
2. Another Enter to submit

```bash
nbs-ts send "$HANDLE" '/nbs'
sleep 1
nbs-ts send "$HANDLE" ''  # Second Enter
```

### Timing

Wait for prompts before sending commands. The process may not be ready immediately after create.

```bash
HANDLE=$(nbs-ts create --name="$SESSION_NAME" "claude")
# Poll for prompt before sending
for i in $(seq 1 30); do
    if nbs-ts read-new "$HANDLE" --strip | grep -q 'Welcome'; then break; fi
    sleep 1
done
nbs-ts send "$HANDLE" '/nbs'            # Then send
```

## Working Directory: Isolated Repositories

Run tests in isolated temporary git repositories, not in the framework directory itself.

**Why**: Claude can see and reason about visible files. If she sees test infrastructure, planning documents about the test, or obviously synthetic fixtures, she may reason about the *meta-situation* rather than behaving normally.

**Example**: A test for investigation dispatch failed because Claude saw `test_investigation_*.sh` files, a plan file titled "investigation-testing-plan.md", and correctly deduced she was being tested.

**Solution**: Create a clean temporary repository with only the files needed for the test scenario.

```bash
TEST_REPO=$(mktemp -d)
cd "$TEST_REPO"
git init -q
git config user.email "test@test.com"
git config user.name "Test"

# Create only what the test needs
mkdir -p concepts
cat > concepts/goals.md << 'EOF'
# Goals
Investigate the cache race condition.
EOF

cat > INVESTIGATION-STATUS.md << 'EOF'
# Investigation: Cache Race
## Status: In Progress
EOF

git add -A
git commit -q -m "Setup"
```

Now Claude sees a small project with an investigation, not a test harness.

## Cleanup

Always kill sessions when done. Use trap to ensure cleanup on any exit.

```bash
cleanup() {
    HANDLE=$(nbs-ts find "$SESSION_NAME" 2>/dev/null) && nbs-ts kill "$HANDLE" 2>/dev/null || true
    rm -rf "$TEST_REPO"
}
trap cleanup EXIT
```

## See Also

- [Testing Strategy](testing-strategy.md) - Overall approach
