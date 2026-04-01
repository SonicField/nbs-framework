#!/bin/bash
# Test: nbs-md-viewer table rendering with box drawing
#
# Plan section 8.2: test_md_viewer_tables.sh
# Verifies table content appears and box drawing characters are used.

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

echo "=== nbs-md-viewer Tables Test ==="
echo ""

TMPDIR=$(mktemp -d)

cat > "$TMPDIR/tables.md" << 'EOF'
# Table Test

| Name    | Age | City     |
|:--------|:---:|---------:|
| Alice   | 30  | London   |
| Bob     | 25  | New York |
| Charlie | 40  | Paris    |
EOF

# T1: Table renders with box drawing characters
echo "T1. Table renders with box drawing characters..."
HANDLE=$("$NBS_TS" create --name=md-table "$MD_VIEWER < $TMPDIR/tables.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 1

OUTPUT=$("$NBS_TS" read "$HANDLE" 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)

# Check for box drawing characters (─ horizontal, │ vertical, ┌ ┐ └ ┘ corners)
if echo "$OUTPUT" | grep -q "─\|│\|┌\|└"; then
    pass "Box drawing characters present"
else
    fail "Missing box drawing characters (expected ─, │, ┌, or └)"
    echo "   Output (first 10 lines):"
    echo "$OUTPUT" | head -10 | sed 's/^/   /'
fi

# T2: Header text appears
echo "T2. Table header text appears..."
if echo "$OUTPUT" | grep -q "Name"; then
    pass "Header 'Name' appears"
else
    fail "Header 'Name' missing"
    echo "   Output:"
    echo "$OUTPUT" | head -10 | sed 's/^/   /'
fi

# T3: Cell data appears
echo "T3. Table cell data appears..."
if echo "$OUTPUT" | grep -q "Alice"; then
    pass "Cell data 'Alice' appears"
else
    fail "Cell data 'Alice' missing"
fi

# T4: Multiple rows rendered
echo "T4. Multiple rows rendered..."
if echo "$OUTPUT" | grep -q "Bob"; then
    pass "Second row 'Bob' appears"
else
    fail "Second row 'Bob' missing"
fi

# T5: Vertical separator present (│)
echo "T5. Vertical separator character present..."
if echo "$OUTPUT" | grep -q "│"; then
    pass "Vertical separator │ present"
else
    fail "Missing vertical separator │"
fi

# T6: Horizontal rule present (─)
echo "T6. Horizontal rule character present..."
if echo "$OUTPUT" | grep -q "─"; then
    pass "Horizontal rule ─ present"
else
    fail "Missing horizontal rule ─"
fi

# Clean up viewer
"$NBS_TS" send "$HANDLE" "q" 2>/dev/null || true
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true
sleep 1

echo ""
echo "=== Results ==="
if [ $ERRORS -eq 0 ]; then
    echo "All tests passed"
    exit 0
else
    echo "$ERRORS test(s) failed"
    exit 1
fi
