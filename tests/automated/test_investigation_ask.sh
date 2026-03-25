#!/bin/bash
# Test: /nbs asks user when INVESTIGATION-STATUS.md found in subdirectory only
#
# Uses nbs-ts for proper interactive testing - can detect and respond to
# AskUserQuestion prompts.
#
# Falsification: Test fails if AI proceeds without asking (either investigation or normal review)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
EXTRACT_JSON="$PROJECT_ROOT/bin/extract_json.py"
VERDICT_FILE="$SCRIPT_DIR/verdicts/investigation_ask_verdict.json"

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

echo "=== Testing /nbs Asks When INVESTIGATION-STATUS.md in Subdirectory ==="
echo "Isolated test repo: $TEST_REPO"
echo ""

# Step 0: Create isolated git repo with status file in subdirectory only
echo "Step 0: Creating isolated test repo..."

cd "$TEST_REPO"

git init -q
git config user.email "test@test.com"
git config user.name "Test"

mkdir -p src docs/investigations

cat > src/main.py << 'EOF'
# Main application
def hello():
    return "Hello, World!"
EOF

cat > README.md << 'EOF'
# My Project
A simple project.
EOF

# Status file in subdirectory with ACTIVE investigation
cat > docs/investigations/INVESTIGATION-STATUS.md << 'EOF'
# Investigation: API Response Time Regression

## Status: In Progress

## Hypothesis
The v3.2 release introduced a regression in API response times.

## Falsification Criteria
- If response times return to baseline after revert, hypothesis supported
- If response times remain elevated after revert, hypothesis wrong

## Observations
- 2026-01-28: P95 latency up 40%
- 2026-01-27: No obvious changes

## Next Steps
Run profiler on production traffic
EOF

git add -A
git commit -q -m "Initial setup"

echo "Created repo with INVESTIGATION-STATUS.md in docs/investigations/"
echo "NO status file at repo root, NOT on investigation branch"
echo ""

# Step 1: Start claude in the test repo via nbs-ts
echo "Step 1: Starting claude in isolated repo..."
SESSION_HANDLE=$("$NBS_TS" create "cd '$TEST_REPO' && claude" 2>&1 | tail -1)

# Wait for trust prompt and accept it
echo "Waiting for trust prompt..."
if "$NBS_TS" wait-pattern "$SESSION_HANDLE" 'trust' --timeout=30 2>/dev/null; then
    echo "Trust prompt detected, accepting..."
    "$NBS_TS" send "$SESSION_HANDLE" '' 2>/dev/null  # Send Enter to accept
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

# Step 2: Send /nbs command
echo "Step 2: Sending /nbs command..."
"$NBS_TS" send "$SESSION_HANDLE" '/nbs' 2>/dev/null
# Send extra Enter to ensure submission (some TUIs need this)
sleep 1
"$NBS_TS" send "$SESSION_HANDLE" '' 2>/dev/null

# Wait for output to appear (processing time)
echo "Waiting for response..."
sleep 60

# Step 3: Capture output
echo "Step 3: Capturing output..."
"$NBS_TS" read "$SESSION_HANDLE" 2>/dev/null > "$CAPTURE_FILE"

echo "Output captured. Analyzing..."
echo ""

# Step 4: Evaluate the output
NBS_OUTPUT=$(cat "$CAPTURE_FILE")

# Check for indicators
ASKED_USER=false
IS_NORMAL_REVIEW=false
IS_INVESTIGATION_REVIEW=false
FOUND_FILE=false

# Check if it found the file
if echo "$NBS_OUTPUT" | grep -qi "INVESTIGATION-STATUS\|investigation.*subdirectory\|docs/investigations"; then
    FOUND_FILE=true
fi

# Check if it asked the user about investigation context
# AskUserQuestion appears as a selection UI with options
if echo "$NBS_OUTPUT" | grep -qi "are you.*investigation\|is this.*investigation\|investigation.*active\|confirm.*investigation\|currently.*investigation\|test fixture\|Active investigation\|old file"; then
    ASKED_USER=true
fi

# Also check for the selection UI pattern (☐ or numbered options)
if echo "$NBS_OUTPUT" | grep -q "☐\|❯ 1\.\|1\. Active"; then
    ASKED_USER=true
fi

# Check if it produced normal review format
if echo "$NBS_OUTPUT" | grep -q "## Status" && echo "$NBS_OUTPUT" | grep -q "## Recommendations"; then
    IS_NORMAL_REVIEW=true
fi

# Check if it produced investigation review format
if echo "$NBS_OUTPUT" | grep -qi "hypothesis.*falsif\|experiment.*design\|investigation review"; then
    IS_INVESTIGATION_REVIEW=true
fi

echo "Analysis:"
echo "  Found file: $FOUND_FILE"
echo "  Asked user: $ASKED_USER"
echo "  Is normal review: $IS_NORMAL_REVIEW"
echo "  Is investigation review: $IS_INVESTIGATION_REVIEW"
echo ""

# Step 5: Determine verdict
# PASS if: Asked user about investigation context
# FAIL if: Proceeded without asking (either normal or investigation review completed)

if [[ "$ASKED_USER" == true ]]; then
    VERDICT="PASS"
    REASONING="AI correctly asked user about ambiguous investigation context before proceeding"
elif [[ "$IS_NORMAL_REVIEW" == true ]] || [[ "$IS_INVESTIGATION_REVIEW" == true ]]; then
    VERDICT="FAIL"
    REASONING="AI proceeded with complete review without asking about subdirectory investigation file"
else
    # Neither asked nor produced complete review - check if still processing
    if echo "$NBS_OUTPUT" | grep -q "●\|⠋\|⠙\|⠹"; then
        VERDICT="INCONCLUSIVE"
        REASONING="AI appears to still be processing - may need more time"
    else
        VERDICT="FAIL"
        REASONING="AI did not ask and did not produce clear review - unclear behaviour"
    fi
fi

echo "=== RESULT ==="
echo "Verdict: $VERDICT"
echo "Reasoning: $REASONING"

# Write verdict
cat > "$VERDICT_FILE" << EOF
{
  "verdict": "$VERDICT",
  "found_file": $FOUND_FILE,
  "asked_user": $ASKED_USER,
  "is_normal_review": $IS_NORMAL_REVIEW,
  "is_investigation_review": $IS_INVESTIGATION_REVIEW,
  "reasoning": "$REASONING"
}
EOF

echo "Details: $VERDICT_FILE"
echo ""

if [[ "$VERDICT" == "PASS" ]]; then
    echo "TEST PASSED: /nbs correctly asked user about ambiguous context"
    exit 0
elif [[ "$VERDICT" == "INCONCLUSIVE" ]]; then
    echo "TEST INCONCLUSIVE: Need more time or manual inspection"
    echo ""
    echo "Captured output:"
    cat "$CAPTURE_FILE"
    exit 1
else
    echo "TEST FAILED: /nbs did not ask user when it should have"
    echo ""
    echo "Captured output:"
    cat "$CAPTURE_FILE"
    exit 1
fi
