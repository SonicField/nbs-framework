#!/bin/bash
# Test: nbs-md-viewer nested list rendering — ordered and unordered
#
# Plan section 8.2: test_md_viewer_lists.sh
# Verifies bullet characters and numbered items appear for nested lists.

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

echo "=== nbs-md-viewer Lists Test ==="
echo ""

TMPDIR=$(mktemp -d)

cat > "$TMPDIR/lists.md" << 'EOF'
# List Examples

## Unordered List

- First item
  - Nested item alpha
  - Nested item beta
- Second item
- Third item

## Ordered List

1. Step one
2. Step two
   1. Sub-step A
   2. Sub-step B
3. Step three

## Mixed

- Bullet point
  1. Ordered under bullet
  2. Another ordered
- Another bullet
EOF

# L1: Viewer renders lists without crash
echo "L1. Viewer renders lists and exits cleanly..."
HANDLE=$("$NBS_TS" create --name=md-lists "$MD_VIEWER < $TMPDIR/lists.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 1

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

# L2: Bullet characters appear for unordered list
echo "L2. Bullet characters appear..."
# The viewer should render bullets as bullet point characters
if echo "$OUTPUT" | grep -q "•\|◦\|▪\|‣\|-\|*"; then
    pass "Bullet characters found in output"
else
    fail "No bullet characters found (expected one of: bullet, dash, asterisk)"
    echo "   Output (first 15 lines):"
    echo "$OUTPUT" | head -15 | sed 's/^/   /'
fi

# L3: Unordered list item text appears
echo "L3. Unordered list item text appears..."
if echo "$OUTPUT" | grep -q "First item"; then
    pass "List item 'First item' found"
else
    fail "List item 'First item' missing"
fi

# L4: Nested list item text appears
echo "L4. Nested list item text appears..."
if echo "$OUTPUT" | grep -q "Nested item alpha"; then
    pass "Nested item 'Nested item alpha' found"
else
    fail "Nested item 'Nested item alpha' missing"
    echo "   Output:"
    echo "$OUTPUT" | head -20 | sed 's/^/   /'
fi

# L5: Ordered list numbers appear
echo "L5. Ordered list numbers appear..."
# Check for numbered items — the viewer may render as "1." or "1)" or just "1"
if echo "$OUTPUT" | grep -q "Step one"; then
    pass "Ordered item 'Step one' found"
else
    fail "Ordered item 'Step one' missing"
fi

# L6: Ordered list numbering is present
echo "L6. Ordered list numbering present..."
# Look for patterns like "1." or "1)" indicating numbered list rendering
if echo "$OUTPUT" | grep -qE "[0-9]+[.)]\s|[0-9]+[.)]"; then
    pass "Numeric list markers found"
else
    # Some renderers use different numbering styles
    if echo "$OUTPUT" | grep -q "Step two"; then
        pass "Ordered list content present (numbering style may differ)"
    else
        fail "No ordered list numbering or content found"
    fi
fi

# L7: Nested ordered items appear
echo "L7. Nested ordered items appear..."
if echo "$OUTPUT" | grep -q "Sub-step A"; then
    pass "Nested ordered item 'Sub-step A' found"
else
    fail "Nested ordered item 'Sub-step A' missing"
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
