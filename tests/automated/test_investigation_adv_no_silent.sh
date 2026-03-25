#!/bin/bash
# Test: /nbs does NOT silently proceed when INVESTIGATION-STATUS.md in subdirectory
#
# ADVERSARIAL TEST - verifies wrong behaviour does NOT occur
# When file is in subdirectory (ambiguous), AI should ask user,
# NOT silently proceed with any complete review.
#
# Falsification: Test fails if AI produces complete review without asking

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
VERDICT_FILE="$SCRIPT_DIR/verdicts/investigation_adv_no_silent_verdict.json"

# Create isolated test environment
TEST_REPO=$(mktemp -d)
SESSION_HANDLE=""
CAPTURE_FILE=$(mktemp)

cleanup() {
    [[ -n "$SESSION_HANDLE" ]] && "$NBS_TS" kill "$SESSION_HANDLE" 2>/dev/null || true
    rm -rf "$TEST_REPO"
    rm -f "$CAPTURE_FILE"
}
trap cleanup EXIT

echo "=== ADVERSARIAL: /nbs Should NOT Silently Proceed When File in Subdirectory ==="
echo "Isolated test repo: $TEST_REPO"
echo ""

# Setup: Create repo with INVESTIGATION-STATUS.md in subdirectory only
echo "Setup: Creating isolated test repo with file in subdirectory..."

cd "$TEST_REPO"

git init -q
git config user.email "test@test.com"
git config user.name "Test"

mkdir -p src old_investigations

cat > src/app.py << 'EOF'
def main():
    print("Hello")
EOF

cat > README.md << 'EOF'
# Project
A project.
EOF

# File in subdirectory with ACTIVE status - ambiguous situation
cat > old_investigations/INVESTIGATION-STATUS.md << 'EOF'
# Investigation: Memory Leak

## Status: In Progress

## Hypothesis
Memory leak in the cache module.

## Observations
- Memory grows over time
- No obvious source yet
EOF

git add -A
git commit -q -m "Setup"

echo "Created repo with INVESTIGATION-STATUS.md in old_investigations/"
echo "NO file at root, NOT on investigation branch"
echo ""

# Start claude via nbs-ts
echo "Starting claude in isolated repo..."
SESSION_HANDLE=$("$NBS_TS" create "cd '$TEST_REPO' && claude" 2>&1 | tail -1)

# Handle trust prompt
echo "Waiting for trust prompt..."
if "$NBS_TS" wait-pattern "$SESSION_HANDLE" 'trust' --timeout=30 2>/dev/null; then
    echo "Trust prompt detected, accepting..."
    "$NBS_TS" send "$SESSION_HANDLE" '' 2>/dev/null
    sleep 2
fi

# Wait for main prompt
echo "Waiting for main prompt..."
if ! "$NBS_TS" wait-pattern "$SESSION_HANDLE" 'Welcome' --timeout=30 2>/dev/null; then
    echo "FAIL: Claude did not show welcome screen"
    exit 1
fi
sleep 2
echo "Claude ready."
echo ""

# Send /nbs
echo "Sending /nbs command..."
"$NBS_TS" send "$SESSION_HANDLE" '/nbs' 2>/dev/null
sleep 1
"$NBS_TS" send "$SESSION_HANDLE" '' 2>/dev/null

# Wait for response
echo "Waiting for response..."
sleep 60

# Capture output
echo "Capturing output..."
"$NBS_TS" read "$SESSION_HANDLE" 2>/dev/null > "$CAPTURE_FILE"

NBS_OUTPUT=$(cat "$CAPTURE_FILE")

# Check for indicators
ASKED_USER=false
PRODUCED_COMPLETE_REVIEW=false

# Check if it asked user
if echo "$NBS_OUTPUT" | grep -qi "are you.*investigation\|is this.*investigation\|test fixture\|Active investigation\|old file\|☐\|❯ 1\."; then
    ASKED_USER=true
fi

# Check if it produced a complete review (either type) without asking
if echo "$NBS_OUTPUT" | grep -q "## Status" && echo "$NBS_OUTPUT" | grep -q "## Recommendations"; then
    PRODUCED_COMPLETE_REVIEW=true
fi
if echo "$NBS_OUTPUT" | grep -qi "## Verdict\|investigation review\|hypothesis.*falsif"; then
    PRODUCED_COMPLETE_REVIEW=true
fi

echo "Analysis:"
echo "  Asked user: $ASKED_USER"
echo "  Produced complete review: $PRODUCED_COMPLETE_REVIEW"
echo ""

# Adversarial condition: Should NOT proceed without asking
# PASS if asked OR did not produce complete review
# FAIL if produced complete review without asking

if [[ "$ASKED_USER" == true ]]; then
    VERDICT="PASS"
    REASONING="AI correctly asked user before proceeding"
elif [[ "$PRODUCED_COMPLETE_REVIEW" == false ]]; then
    VERDICT="PASS"
    REASONING="AI did not silently produce complete review"
else
    VERDICT="FAIL"
    REASONING="AI silently produced complete review without asking about subdirectory file"
fi

echo "=== RESULT ==="
echo "Verdict: $VERDICT"
echo "Reasoning: $REASONING"

cat > "$VERDICT_FILE" << EOF
{
  "verdict": "$VERDICT",
  "asked_user": $ASKED_USER,
  "produced_complete_review": $PRODUCED_COMPLETE_REVIEW,
  "reasoning": "$REASONING"
}
EOF

echo "Details: $VERDICT_FILE"
echo ""

if [[ "$VERDICT" == "PASS" ]]; then
    echo "TEST PASSED: /nbs correctly avoided silent proceed"
    exit 0
else
    echo "TEST FAILED: /nbs silently proceeded without asking"
    echo ""
    echo "Captured output:"
    cat "$CAPTURE_FILE"
    exit 1
fi
