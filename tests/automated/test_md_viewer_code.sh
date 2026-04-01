#!/bin/bash
# Test: nbs-md-viewer code fence rendering
#
# Plan section 8.2: test_md_viewer_code.sh
# Verifies code fences with and without language tags render correctly.
# Checks that code content appears and border characters are present.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_TS_RENDER="$PROJECT_ROOT/bin/nbs-ts-render"
MD_VIEWER="$PROJECT_ROOT/bin/nbs-md-viewer"

[ -x "$MD_VIEWER" ] || { echo "SKIP: nbs-md-viewer not found"; exit 0; }
[ -x "$NBS_TS" ] || { echo "SKIP: nbs-ts not found"; exit 0; }
[ -x "$NBS_TS_RENDER" ] || { echo "SKIP: nbs-ts-render not found"; exit 0; }

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-md-viewer Code Fence Test ==="
echo ""

TMPDIR=$(mktemp -d)

cat > "$TMPDIR/code.md" << 'ENDOFMD'
# Code Examples

Here is a C code block:

```c
int main(void) {
    printf("hello world\n");
    return 0;
}
```

And a plain code block without language tag:

```
plain code here
no language specified
```

Some text after the code blocks.
ENDOFMD

# C1: Viewer renders code fences without crash
echo "C1. Viewer renders code fences and exits cleanly..."
HANDLE=$("$NBS_TS" create --name=md-code "$MD_VIEWER < $TMPDIR/code.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 1

# Capture output before quitting
OUTPUT=$("$NBS_TS" read "$HANDLE" 2>&1 | "$NBS_TS_RENDER" --width=80 --height=40)

"$NBS_TS" send "$HANDLE" "q"
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true
sleep 1

EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE" 2>&1) || EXIT_CODE="unknown"
if [ "$EXIT_CODE" = "0" ]; then
    pass "Viewer exited cleanly with code 0"
else
    fail "Viewer exit code: $EXIT_CODE (expected 0)"
fi

# C2: Code content from the C block appears
echo "C2. C code content appears in output..."
if echo "$OUTPUT" | grep -q "printf\|hello world\|main"; then
    pass "C code content found in output"
else
    fail "C code content missing from output"
    echo "   Output (first 20 lines):"
    echo "$OUTPUT" | head -20 | sed 's/^/   /'
fi

# C3: Plain code block content appears
echo "C3. Plain code block content appears..."
if echo "$OUTPUT" | grep -q "plain code here"; then
    pass "Plain code block content found"
else
    fail "Plain code block content missing"
    echo "   Output:"
    echo "$OUTPUT" | head -25 | sed 's/^/   /'
fi

# C4: Border characters appear (box drawing for code fence border)
echo "C4. Code fence border characters appear..."
# Code fences typically render with box drawing: │ (vertical) or ─ (horizontal)
if echo "$OUTPUT" | grep -q "│\|┌\|└\|─"; then
    pass "Border characters found in output"
else
    fail "No border characters found (expected │, ┌, └, or ─)"
    echo "   Output:"
    echo "$OUTPUT" | head -25 | sed 's/^/   /'
fi

# C5: Language tag (c) does not appear as literal text in the rendered output
# The language tag should be consumed by the parser, not displayed raw
echo "C5. Language tag is processed (not rendered as raw text)..."
# Check that the literal string "```c" does not appear in the rendered output
if echo "$OUTPUT" | grep -q '```c'; then
    fail "Raw fence marker '\`\`\`c' appears in rendered output"
else
    pass "Fence markers not shown as raw text"
fi

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
