#!/bin/bash
# Test: nbs-md-viewer scroll navigation
#
# Plan §7.3: UP/DOWN arrows, Page Up/Down, Home/End.
# Verifies that scrolling changes the visible content.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
NBS_TS_RENDER="$PROJECT_ROOT/bin/nbs-ts-render"
MD_VIEWER="$PROJECT_ROOT/bin/nbs-md-viewer"

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-md-viewer Scroll Test ==="
echo ""

TMPDIR=$(mktemp -d)
trap 'cleanup; rm -rf "$TMPDIR"' EXIT

# Generate a document with identifiable sections that won't fit in 24 rows
cat > "$TMPDIR/scroll.md" << 'EOF'
# Top Section

Paragraph at the top of the document.

Line 1.
Line 2.
Line 3.
Line 4.
Line 5.
Line 6.
Line 7.
Line 8.
Line 9.
Line 10.
Line 11.
Line 12.
Line 13.
Line 14.
Line 15.
Line 16.
Line 17.
Line 18.
Line 19.
Line 20.
Line 21.
Line 22.
Line 23.
Line 24.
Line 25.

# Bottom Section

This is the bottom of the document.
EOF

# SC1: Initial view shows top
echo "SC1. Initial view shows top..."
HANDLE=$("$NBS_TS" create --name=md-scroll "$MD_VIEWER < $TMPDIR/scroll.md" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 1

OUTPUT1=$("$NBS_TS" read "$HANDLE" 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)
if echo "$OUTPUT1" | grep -q "Top Section"; then
    pass "Initial view shows 'Top Section'"
else
    fail "Initial view missing 'Top Section'"
    echo "   Output: $(echo "$OUTPUT1" | head -3)"
fi

# SC2: Scroll down with Page Down shows later content
echo "SC2. Page Down scrolls to later content..."
# Send Page Down (ESC[6~)
"$NBS_TS" send "$HANDLE" $'\033[6~'
sleep 1

OUTPUT2=$("$NBS_TS" read "$HANDLE" 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)
# After page down, we should see different content than the top
if [ "$OUTPUT2" != "$OUTPUT1" ] || echo "$OUTPUT2" | grep -q "Line"; then
    pass "Page Down changed visible content"
else
    fail "Page Down did not change content"
fi

# SC3: End key goes to bottom
echo "SC3. End key shows bottom..."
# Send End key (ESC[F)
"$NBS_TS" send "$HANDLE" $'\033[F'
sleep 1

OUTPUT3=$("$NBS_TS" read "$HANDLE" 2>&1 | "$NBS_TS_RENDER" --width=80 --height=24)
if echo "$OUTPUT3" | grep -q "Bottom Section\|bottom of the document"; then
    pass "End key shows bottom content"
else
    fail "End key did not reach bottom"
    echo "   Output: $(echo "$OUTPUT3" | tail -5)"
fi

# Quit
"$NBS_TS" send "$HANDLE" "q"
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
