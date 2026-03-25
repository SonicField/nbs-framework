#!/bin/bash
# Test: nbs-workers search with ANSI stripping and context
#
# Falsification approach:
# - Write test data directly to log file for deterministic testing
# - Also test via tmux pipe-pane for ANSI stripping verification
# - Verify context lines are correct count
# - Verify regex patterns work

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_WORKER="$PROJECT_ROOT/bin/nbs-workers"

TEST_DIR=$(mktemp -d)
ORIGINAL_DIR=$(pwd)

ERRORS=0

NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
HANDLES=()

cleanup() {
    cd "$ORIGINAL_DIR"
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== nbs-workers Search Test ==="
echo "Test directory: $TEST_DIR"
echo ""

# Set up test project
mkdir -p "$TEST_DIR/.nbs/workers"
cd "$TEST_DIR"

# --- Test 1: Search with context lines (deterministic — write log directly) ---
echo "1. Search with context lines..."

WORKER_NAME="context-a1b2"
TASK_FILE=".nbs/workers/${WORKER_NAME}.md"
LOG_FILE=".nbs/workers/${WORKER_NAME}.log"

cat > "$TASK_FILE" <<EOF
# Worker: context-test

## Task

Context test worker.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF

# Write 200 numbered lines directly to log
for i in $(seq 1 200); do
    echo "LINE_$i"
done > "$LOG_FILE"

# Search for LINE_100 with context=5
SEARCH_OUT=$("$NBS_WORKER" search "$WORKER_NAME" "^LINE_100$" --context=5 2>&1)

if echo "$SEARCH_OUT" | grep -q "LINE_100"; then
    echo "   PASS: Found LINE_100"
else
    echo "   FAIL: LINE_100 not found"
    echo "   Log file size: $(wc -c < "$LOG_FILE" 2>/dev/null || echo 'missing')"
    ERRORS=$((ERRORS + 1))
fi

# Check that context includes nearby lines
CONTEXT_CHECK=true
for ctx_line in LINE_96 LINE_97 LINE_98 LINE_99 LINE_101 LINE_102 LINE_103 LINE_104; do
    if ! echo "$SEARCH_OUT" | grep -q "$ctx_line"; then
        echo "   FAIL: Context line $ctx_line not found"
        CONTEXT_CHECK=false
        ERRORS=$((ERRORS + 1))
        break
    fi
done
if [[ "$CONTEXT_CHECK" == true ]]; then
    echo "   PASS: Context lines present around match"
fi

# Verify context does NOT include lines too far away
if echo "$SEARCH_OUT" | grep -q "^LINE_90$"; then
    echo "   FAIL: Context too wide — LINE_90 should not be in context=5"
    ERRORS=$((ERRORS + 1))
else
    echo "   PASS: Context correctly bounded (LINE_90 excluded)"
fi

# --- Test 2: ANSI stripping (write ANSI codes directly to log) ---
echo "2. ANSI escape code stripping..."

ANSI_WORKER="ansistrip-c3d4"
ANSI_TASK=".nbs/workers/${ANSI_WORKER}.md"
ANSI_LOG=".nbs/workers/${ANSI_WORKER}.log"

cat > "$ANSI_TASK" <<EOF
# Worker: ansistrip-test

## Task

ANSI strip test.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF

# Write log with ANSI escape codes embedded
ANSI_MARKER="COLOURED_MARKER_$(date +%s)"
printf 'Before line\n' > "$ANSI_LOG"
printf '\033[31m%s\033[0m\n' "$ANSI_MARKER" >> "$ANSI_LOG"
printf 'After line\n' >> "$ANSI_LOG"
printf '\033[1;32mBOLD_GREEN_TEXT\033[0m\n' >> "$ANSI_LOG"
printf '\033[38;5;196mEXTENDED_COLOUR\033[0m\n' >> "$ANSI_LOG"

# Search for plain text (should match despite ANSI codes in log)
ANSI_SEARCH=$("$NBS_WORKER" search "$ANSI_WORKER" "$ANSI_MARKER" --context=1 2>&1)
if echo "$ANSI_SEARCH" | grep -q "$ANSI_MARKER"; then
    echo "   PASS: Found marker through raw ANSI escape codes"
else
    echo "   FAIL: Could not find marker through ANSI codes"
    echo "   Log hex: $(xxd "$ANSI_LOG" | head -5)"
    ERRORS=$((ERRORS + 1))
fi

# Search for bold green text
BOLD_SEARCH=$("$NBS_WORKER" search "$ANSI_WORKER" "BOLD_GREEN_TEXT" --context=0 2>&1)
if echo "$BOLD_SEARCH" | grep -q "BOLD_GREEN_TEXT"; then
    echo "   PASS: Found text through bold+colour ANSI codes"
else
    echo "   FAIL: Could not find bold+colour text"
    ERRORS=$((ERRORS + 1))
fi

# Search for extended colour (256-colour mode)
EXT_SEARCH=$("$NBS_WORKER" search "$ANSI_WORKER" "EXTENDED_COLOUR" --context=0 2>&1)
if echo "$EXT_SEARCH" | grep -q "EXTENDED_COLOUR"; then
    echo "   PASS: Found text through 256-colour ANSI codes"
else
    echo "   FAIL: Could not find 256-colour text"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 3: ANSI stripping via live nbs-ts session ---
echo "3. ANSI via live nbs-ts session..."

LIVE_WORKER="ansi-a7b8"
LIVE_TASK=".nbs/workers/${LIVE_WORKER}.md"
LIVE_LOG=".nbs/workers/${LIVE_WORKER}.log"

cat > "$LIVE_TASK" <<EOF
# Worker: ansi-test

## Task

Live ANSI test.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF

LIVE_HANDLE=$("$NBS_TS" create --name="nbs-${LIVE_WORKER}" bash 2>&1 | tail -1)
HANDLES+=("$LIVE_HANDLE")
sleep 1

LIVE_MARKER="LIVE_ANSI_$(date +%s)"
"$NBS_TS" send "$LIVE_HANDLE" "printf '\\033[31m${LIVE_MARKER}\\033[0m\\n'" 2>/dev/null
sleep 2

# Read output and write to log for search
"$NBS_TS" read-new "$LIVE_HANDLE" --strip 2>/dev/null > "$LIVE_LOG"

LIVE_SEARCH=$("$NBS_WORKER" search "$LIVE_WORKER" "$LIVE_MARKER" --context=1 2>&1)
if echo "$LIVE_SEARCH" | grep -q "$LIVE_MARKER"; then
    echo "   PASS: Found marker through live nbs-ts ANSI output"
else
    echo "   FAIL: Could not find marker in live nbs-ts output"
    if [[ -f "$LIVE_LOG" ]]; then
        echo "   Log size: $(wc -c < "$LIVE_LOG")"
    fi
    ERRORS=$((ERRORS + 1))
fi

"$NBS_TS" kill "$LIVE_HANDLE" 2>/dev/null || true

# --- Test 4: Regex patterns ---
echo "4. Regex pattern matching..."

# Use the context-test worker's log — append some pattern lines
echo 'ERROR: connection refused at port 8080' >> ".nbs/workers/context-a1b2.log"
echo 'WARNING: timeout at port 9090' >> ".nbs/workers/context-a1b2.log"
echo 'INFO: all clear' >> ".nbs/workers/context-a1b2.log"

# Search with regex
REGEX_OUT=$("$NBS_WORKER" search "context-a1b2" "ERROR.*port [0-9]+" --context=1 2>&1)
if echo "$REGEX_OUT" | grep -q "ERROR.*connection refused"; then
    echo "   PASS: Regex pattern matched"
else
    echo "   FAIL: Regex pattern did not match"
    echo "   Output: $REGEX_OUT"
    ERRORS=$((ERRORS + 1))
fi

# Search for alternation
ALT_OUT=$("$NBS_WORKER" search "context-a1b2" "(ERROR|WARNING).*port" --context=0 2>&1)
ERROR_COUNT=$(echo "$ALT_OUT" | grep -c "port" || true)
if [[ "$ERROR_COUNT" -ge 2 ]]; then
    echo "   PASS: Alternation pattern found both ERROR and WARNING lines"
else
    echo "   FAIL: Alternation pattern found $ERROR_COUNT matches, expected >= 2"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 5: Search with no matches ---
echo "5. Search with no matches..."
NO_MATCH=$("$NBS_WORKER" search "context-a1b2" "DEFINITELY_NOT_IN_OUTPUT_xyz123" 2>&1) || true
if echo "$NO_MATCH" | grep -q "No matches found"; then
    echo "   PASS: No-match reported correctly"
else
    echo "   FAIL: No-match not reported"
    echo "   Output: $NO_MATCH"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 6: Search nonexistent worker ---
echo "6. Search nonexistent worker..."
NONEXIST=$("$NBS_WORKER" search "nonexist-0000" "pattern" 2>&1) || true
if echo "$NONEXIST" | grep -q "No log file found"; then
    echo "   PASS: Nonexistent worker reported correctly"
else
    echo "   FAIL: Nonexistent worker not reported"
    echo "   Output: $NONEXIST"
    ERRORS=$((ERRORS + 1))
fi

# --- Test 7: Default context size (50 lines) ---
echo "7. Default context size (50 lines)..."

BIGCTX_WORKER="bigctx-e5f6"
BIGCTX_TASK=".nbs/workers/${BIGCTX_WORKER}.md"
BIGCTX_LOG=".nbs/workers/${BIGCTX_WORKER}.log"

cat > "$BIGCTX_TASK" <<EOF
# Worker: bigctx-test

## Task

Big context test.

## Status

State: running
Started: $(date '+%Y-%m-%d %H:%M:%S')
Completed:

## Log

[Worker appends findings here]
EOF

# Write 300 lines with a target in the middle
{
    for i in $(seq 1 150); do echo "BEFORE_$i"; done
    echo "TARGET_LINE"
    for i in $(seq 1 150); do echo "AFTER_$i"; done
} > "$BIGCTX_LOG"

# Search with default context (50)
DEFAULT_CTX=$("$NBS_WORKER" search "$BIGCTX_WORKER" "TARGET_LINE" 2>&1)

# Should include BEFORE_101 (150-50+1=101) but not BEFORE_99 (150-50-1=99)
if echo "$DEFAULT_CTX" | grep -q "BEFORE_101"; then
    echo "   PASS: Default context=50 includes BEFORE_101"
else
    echo "   FAIL: Default context=50 missing BEFORE_101"
    ERRORS=$((ERRORS + 1))
fi

if echo "$DEFAULT_CTX" | grep -q "AFTER_50"; then
    echo "   PASS: Default context=50 includes AFTER_50"
else
    echo "   FAIL: Default context=50 missing AFTER_50"
    ERRORS=$((ERRORS + 1))
fi

if echo "$DEFAULT_CTX" | grep -q "^BEFORE_99$"; then
    echo "   FAIL: Default context=50 incorrectly includes BEFORE_99"
    ERRORS=$((ERRORS + 1))
else
    echo "   PASS: Default context=50 correctly excludes BEFORE_99"
fi

# --- Summary ---
echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All search tests passed"
    exit 0
else
    echo "FAIL: $ERRORS test(s) failed"
    exit 1
fi
