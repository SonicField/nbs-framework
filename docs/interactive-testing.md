# Interactive Testing with Claude

This guide covers testing Claude Code behaviour that requires multi-turn interaction.

## When to Use Interactive Testing

| Scenario | Method |
|----------|--------|
| Evaluate output from a single prompt | `claude -p` (one-shot) |
| Test behaviour that requires AskUserQuestion | pty-session (interactive) |
| Test permission prompt handling | pty-session |
| Test multi-turn conversation flow | pty-session |

Use `claude -p` when you can evaluate the output in isolation. Use pty-session when the test requires responding to prompts or observing interactive behaviour.

## The Pattern

```bash
PTY_SESSION="$PROJECT_ROOT/bin/pty-session"
SESSION_NAME="test_$$"
TEST_REPO=$(mktemp -d)

# Setup cleanup
cleanup() {
    "$PTY_SESSION" kill "$SESSION_NAME" 2>/dev/null || true
    rm -rf "$TEST_REPO"
}
trap cleanup EXIT

# Create test environment (see "Working Directory" below)
cd "$TEST_REPO"
git init -q
# ... create test files ...

# Start Claude
"$PTY_SESSION" create "$SESSION_NAME" "cd '$TEST_REPO' && claude"

# Handle trust prompt
if "$PTY_SESSION" wait "$SESSION_NAME" 'trust' --timeout=30; then
    "$PTY_SESSION" send "$SESSION_NAME" ''  # Accept with Enter
    sleep 2
fi

# Wait for main prompt
"$PTY_SESSION" wait "$SESSION_NAME" 'Welcome' --timeout=30

# Send command
"$PTY_SESSION" send "$SESSION_NAME" '/nbs'
sleep 1
"$PTY_SESSION" send "$SESSION_NAME" ''  # Extra Enter for submission

# Wait for processing
sleep 60

# Capture and evaluate
"$PTY_SESSION" read "$SESSION_NAME" > "$OUTPUT_FILE"
```

## Gotchas

### Trust Prompt

When Claude enters a new directory, she shows "Do you trust the files in this folder?" This blocks all input until accepted.

**Solution**: Wait for 'trust' pattern and send Enter to accept.

```bash
if "$PTY_SESSION" wait "$SESSION_NAME" 'trust' --timeout=30; then
    "$PTY_SESSION" send "$SESSION_NAME" ''
    sleep 2
fi
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
"$PTY_SESSION" send "$SESSION_NAME" '/nbs'
sleep 1
"$PTY_SESSION" send "$SESSION_NAME" ''  # Second Enter
```

### Timing

Wait for prompts before sending commands. The process may not be ready immediately after create.

```bash
"$PTY_SESSION" create "$SESSION_NAME" "claude"
"$PTY_SESSION" wait "$SESSION_NAME" 'Welcome' --timeout=30  # Wait first
"$PTY_SESSION" send "$SESSION_NAME" '/nbs'            # Then send
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
    "$PTY_SESSION" kill "$SESSION_NAME" 2>/dev/null || true
    rm -rf "$TEST_REPO"
}
trap cleanup EXIT
```

## See Also

- [pty-session Reference](pty-session.md) - Command reference
- [Testing Strategy](testing-strategy.md) - Overall approach
