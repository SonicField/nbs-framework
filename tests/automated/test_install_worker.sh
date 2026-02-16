#!/bin/bash
# Test: Worker Path Verification (Adversarial)
#
# Uses a worker to verify paths in installed commands.
# Includes tripwire to ensure worker doesn't access production ~/.nbs/
#
# This is the BELT (AI review) to complement the BRACES (regex check).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
PTY_SESSION="$PROJECT_ROOT/bin/pty-session"

echo "=== Worker Path Verification Test (Adversarial) ==="
echo ""

# Create unique temp directory for test installation
TEST_DIR=$(mktemp -d)
echo "Test directory: $TEST_DIR"

# IMPORTANT: Override HOME so install.sh creates symlinks in the test
# directory, not in real ~/.claude/commands/. Without this, install.sh
# pollutes the real HOME with symlinks to the temp prefix, and when
# cleanup deletes the temp dir, all skill symlinks become dangling.
REAL_HOME="$HOME"
export HOME="$TEST_DIR"
mkdir -p "$HOME/.claude" "$HOME/.nbs/commands"

# Cleanup on exit
cleanup() {
    # Kill worker if still running
    "$PTY_SESSION" kill worker-path-check 2>/dev/null || true
    # Restore real HOME before cleanup
    export HOME="$REAL_HOME"
    rm -rf "$TEST_DIR"
    # Remove tripwire if it exists
    rm -f "$HOME/.nbs/commands/TRIPWIRE_DO_NOT_READ.md" 2>/dev/null || true
    echo "Cleaned up"
}
trap cleanup EXIT

# Step 1: Create tripwire in production ~/.nbs (if it exists)
echo "Step 1: Setting up tripwire..."
if [[ -d "$REAL_HOME/.nbs/commands" ]]; then
    echo "TRIPWIRE: If you read this file, the test has FAILED. You should only be reading from $TEST_DIR" > "$REAL_HOME/.nbs/commands/TRIPWIRE_DO_NOT_READ.md"
    echo "  Tripwire placed in $REAL_HOME/.nbs/commands/"
else
    echo "  No $REAL_HOME/.nbs/commands/ - skipping tripwire (first install)"
fi

# Step 2: Install to test directory
# HOME is overridden so ~/.claude/commands/ symlinks go into TEST_DIR
echo ""
echo "Step 2: Installing to test directory..."
echo "N" | "$PROJECT_ROOT/bin/install.sh" --prefix="$TEST_DIR"

# Step 3: Create worker task file
echo ""
echo "Step 3: Creating worker task..."
mkdir -p "$TEST_DIR/worker"
cat > "$TEST_DIR/worker/task.md" << EOF
# Worker Task: Path Verification

## Your Task

You are verifying that an NBS Framework installation has correct paths.

**CRITICAL: You must ONLY read from: $TEST_DIR**

Do NOT read from ~/.nbs/ under any circumstances. If you see any reference to ~/.nbs/, report it but do NOT follow it.

## Instructions

1. List all .md files in: $TEST_DIR/commands/
2. For EACH file, read it and identify ALL file paths mentioned (paths containing / or ~)
3. For each path found, classify it:
   - VALID: Starts with "$TEST_DIR"
   - VALID: Starts with "~/.claude/commands"
   - VALID: Is a relative reference like "[project]/" or "[docs]/"
   - INVALID: Any other absolute path

## Output Format

Report your findings in this exact format:

\`\`\`
FILES_CHECKED: [count]
VALID_PATHS: [count]
INVALID_PATHS: [count]

INVALID PATH DETAILS:
[If any invalid paths, list: file:line - path]

TRIPWIRE_CHECK: <YES_OR_NO>

VERDICT: <PASS_OR_FAIL>
\`\`\`

## Success Criteria

- VERDICT is PASS only if:
  - INVALID_PATHS is 0
  - TRIPWIRE_CHECK is NO

Update this file with your findings when complete.

## Status

State: pending

## Findings

[Worker fills this in]
EOF

echo "  Task created at $TEST_DIR/worker/task.md"

# Step 4: Spawn worker
echo ""
echo "Step 4: Spawning worker..."
"$PTY_SESSION" create worker-path-check "cd $TEST_DIR && claude --dangerously-skip-permissions"

sleep 3

# Send task to worker
"$PTY_SESSION" send worker-path-check "Read $TEST_DIR/worker/task.md and execute it. Write your findings into that file when complete."

echo "  Worker spawned and task sent"
echo "  Waiting for worker to complete (max 3 minutes)..."

# Step 5: Wait for worker with timeout
TIMEOUT=180
ELAPSED=0
while [[ $ELAPSED -lt $TIMEOUT ]]; do
    sleep 10
    ELAPSED=$((ELAPSED + 10))

    # Check if worker has written findings (placeholder removed)
    if ! grep -q "\[Worker fills this in\]" "$TEST_DIR/worker/task.md" 2>/dev/null; then
        echo "  Worker completed after ${ELAPSED}s"
        break
    fi

    echo "  Still waiting... (${ELAPSED}s)"
done

if [[ $ELAPSED -ge $TIMEOUT ]]; then
    echo "  Worker timed out after ${TIMEOUT}s"
    "$PTY_SESSION" read worker-path-check --scrollback=50
    exit 1
fi

# Step 6: Analyse results
echo ""
echo "Step 5: Analysing worker results..."
echo ""

cat "$TEST_DIR/worker/task.md"

echo ""
echo "=== Verdict Extraction ==="

# Extract verdict
if grep -q "VERDICT: PASS" "$TEST_DIR/worker/task.md"; then
    echo "Worker verdict: PASS"

    # Double-check tripwire
    if grep -q "TRIPWIRE_CHECK: NO" "$TEST_DIR/worker/task.md"; then
        echo "Tripwire check: PASS (worker didn't access ~/.nbs/)"
        echo ""
        echo "=== TEST PASSED ==="
        exit 0
    else
        echo "Tripwire check: UNCLEAR"
        echo "Manual review required"
        exit 1
    fi
else
    echo "Worker verdict: FAIL or inconclusive"
    echo ""
    echo "=== TEST FAILED ==="
    exit 1
fi
