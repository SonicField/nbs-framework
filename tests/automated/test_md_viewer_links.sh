#!/bin/bash
# Test: nbs-md-viewer link highlighting
#
# Verifies:
#   LK1. Inline link text is visible in rendered output
#   LK2. Link URL is NOT visible in rendered output (only display text shown)
#   LK3. Bare URL is visible in rendered output
#   LK4. Underline SGR sequence present for links
#   LK5. Multiple links on same line render correctly

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

echo "=== nbs-md-viewer Link Highlighting Test ==="
echo ""

TMPDIR=$(mktemp -d)

cat > "$TMPDIR/links.md" << 'EOF'
# Link Test

Here is an [inline link](https://example.com) in a paragraph.

And a [second link](https://docs.example.com/guide) with more text.

Bare URL: https://bare.example.com/path

Multiple: [first](https://a.com) and [second](https://b.com) on one line.
EOF

# LK1: Inline link text visible
echo "LK1. Inline link text visible..."
HANDLE=$("$NBS_TS" create --name=md-links "$MD_VIEWER < $TMPDIR/links.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2

OUTPUT=$("$NBS_TS" read "$HANDLE" --last=50 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)
if echo "$OUTPUT" | grep -q "inline link"; then
    pass "Inline link text 'inline link' visible"
else
    fail "Inline link text not visible"
    echo "   Output: $(echo "$OUTPUT" | head -10)"
fi

# LK2: Link URL must NOT appear (only link text is shown)
echo "LK2. Link URL not visible..."
if echo "$OUTPUT" | grep -q "example.com"; then
    fail "Link URL 'example.com' should not be visible"
else
    pass "Link URL correctly hidden"
fi

# LK3: Bare URL visible
echo "LK3. Bare URL visible..."
if echo "$OUTPUT" | grep -q "bare.example.com"; then
    pass "Bare URL visible"
else
    fail "Bare URL not visible"
fi

# LK4: Underline SGR present in raw output
echo "LK4. Underline SGR sequence present..."
RAW=$("$NBS_TS" read "$HANDLE" --last=50 2>&1 | "$NBS_TS_RENDER" --no-strip --width=80 --height=24)
# SGR underline is \033[4m or embedded in compound sequences like \033[4;38;5;74m
if echo "$RAW" | grep -q "\[4m\|\[4;\|;4m\|;4;"; then
    pass "Underline SGR sequence found in output"
else
    fail "No underline SGR sequence"
    echo "   Raw sample: $(echo "$RAW" | grep -i "link\|inline" | head -3)"
fi

# LK5: Multiple links on same line
echo "LK5. Multiple links on same line..."
if echo "$OUTPUT" | grep -q "first" && echo "$OUTPUT" | grep -q "second"; then
    pass "Multiple links visible"
else
    fail "Multiple links not visible"
fi

# Quit viewer
"$NBS_TS" send "$HANDLE" "q"
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true

# LK6: Links inside table cells are styled
echo "LK6. Links inside table cells..."
cat > "$TMPDIR/table_links.md" << 'EOF'
| Document | Description |
|----------|-------------|
| [Guide](https://example.com/guide) | The main guide |
| [API Ref](https://example.com/api) | API reference docs |
EOF

HANDLE2=$("$NBS_TS" create --name=md-table-links "$MD_VIEWER < $TMPDIR/table_links.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 2

OUTPUT2=$("$NBS_TS" read "$HANDLE2" --last=50 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)
if echo "$OUTPUT2" | grep -q "Guide" && echo "$OUTPUT2" | grep -q "API Ref"; then
    pass "Links in table cells: text visible"
else
    fail "Links in table cells: text not visible"
    echo "   Output: $(echo "$OUTPUT2" | head -10)"
fi

# LK7: Table link has underline SGR
echo "LK7. Table links have underline..."
RAW2=$("$NBS_TS" read "$HANDLE2" --last=50 2>&1 | "$NBS_TS_RENDER" --no-strip --width=80 --height=24)
if echo "$RAW2" | grep -q "\[4m\|\[4;\|;4m\|;4;"; then
    pass "Table links have underline SGR"
else
    fail "Table links missing underline"
    echo "   Raw: $(echo "$RAW2" | grep -i "guide\|api" | head -3)"
fi

"$NBS_TS" send "$HANDLE2" "q"
"$NBS_TS" wait-complete "$HANDLE2" --timeout=5 2>/dev/null || true

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
