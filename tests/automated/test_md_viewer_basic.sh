#!/bin/bash
# Test: nbs-md-viewer basic rendering — headings, paragraphs, horizontal rules
#
# Spawns the viewer in a PTY via nbs-ts, feeds markdown through stdin,
# sends 'q' to quit, and verifies the rendered output via nbs-ts-render.
# Plan section 8.2: test_md_viewer_basic.sh

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

echo "=== nbs-md-viewer Basic Test ==="
echo ""

# Create temp input
TMPDIR=$(mktemp -d)

cat > "$TMPDIR/test.md" << 'EOF'
# Main Title

This is a paragraph with some text content.

## Section Two

Another paragraph here with detail.

---

### Subsection

Final paragraph at the end.
EOF

# B1: Viewer renders without crash, exits cleanly on 'q'
echo "B1. Viewer renders and exits cleanly on 'q'..."
HANDLE=$("$NBS_TS" create --name=md-basic "$MD_VIEWER < $TMPDIR/test.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 1

"$NBS_TS" send "$HANDLE" "q"
"$NBS_TS" wait-complete "$HANDLE" --timeout=5 2>/dev/null || true
sleep 1

EXIT_CODE=$("$NBS_TS" exit-code "$HANDLE" 2>&1) || EXIT_CODE="unknown"
if [ "$EXIT_CODE" = "0" ]; then
    pass "Viewer exited cleanly with code 0"
else
    fail "Viewer exit code: $EXIT_CODE (expected 0)"
fi

# B2: Capture rendered output — heading text appears
echo "B2. Output contains heading text..."
OUTPUT=$("$NBS_TS" read "$HANDLE" 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)
if echo "$OUTPUT" | grep -q "Main Title"; then
    pass "Output contains 'Main Title'"
else
    fail "Output missing 'Main Title'"
    echo "   Output (first 5 lines): $(echo "$OUTPUT" | head -5)"
fi

# B3: Paragraph text appears
echo "B3. Output contains paragraph text..."
if echo "$OUTPUT" | grep -q "paragraph"; then
    pass "Output contains paragraph text"
else
    fail "Output missing paragraph text"
    echo "   Output: $(echo "$OUTPUT" | head -10)"
fi

# B4: Horizontal rule renders as box drawing character
echo "B4. Output contains horizontal rule..."
if echo "$OUTPUT" | grep -q "─"; then
    pass "Output contains horizontal rule character (─)"
else
    fail "Output missing horizontal rule character (─)"
    echo "   Output: $(echo "$OUTPUT" | head -15)"
fi

# B5: Second heading appears
echo "B5. Output contains second heading..."
if echo "$OUTPUT" | grep -q "Section Two"; then
    pass "Output contains 'Section Two'"
else
    fail "Output missing 'Section Two'"
fi

# B6: Third-level heading appears
echo "B6. Output contains subsection heading..."
if echo "$OUTPUT" | grep -q "Subsection"; then
    pass "Output contains 'Subsection'"
else
    fail "Output missing 'Subsection'"
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
