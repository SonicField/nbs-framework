#!/bin/bash
# Test: Manifest validation
#
# Verifies (from feature request test table):
#   MI1. MANIFEST.honest parses cleanly with honest-parse
#   MI2. nbs-help finds known tools (keyword search works)
#   MI3. Manifest contains entries for key bin/ tools

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

[ -f "$PROJECT_ROOT/MANIFEST.honest" ] || { echo "SKIP: MANIFEST.honest not found"; exit 0; }

ERRORS=0

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== Manifest Validation Test ==="
echo ""

cd "$PROJECT_ROOT"

# MI1: Manifest parses cleanly
echo "MI1. MANIFEST.honest parses with honest-parse..."
if ~/.nbs/bin/honest-parse MANIFEST.honest >/dev/null 2>&1; then
    pass "honest-parse validates MANIFEST.honest"
else
    fail "honest-parse failed on MANIFEST.honest"
fi

# MI2: Key tools searchable via nbs-help
echo "MI2. nbs-help finds key tools..."
for tool in nbs-chat nbs-ts nbs-md-viewer nbs-bus; do
    OUTPUT=$(bin/nbs-help "$tool" 2>&1)
    if echo "$OUTPUT" | grep -qi "$tool"; then
        pass "nbs-help finds '$tool'"
    else
        fail "nbs-help cannot find '$tool'"
    fi
done

# MI3: Manifest content covers key bin/ tools
echo "MI3. Manifest contains key tool entries..."
MANIFEST_CONTENT=$(cat MANIFEST.honest)
MISSING=0
CHECKED=0
for tool in nbs-chat nbs-ts nbs-bus nbs-sidecar nbs-md-viewer nbs-help nbs-chat-terminal nbs-chat-edit; do
    CHECKED=$((CHECKED + 1))
    if ! echo "$MANIFEST_CONTENT" | grep -qi "$tool"; then
        echo "   WARNING: $tool not found in MANIFEST.honest"
        MISSING=$((MISSING + 1))
    fi
done

if [ "$MISSING" -eq 0 ]; then
    pass "All $CHECKED key tools found in manifest"
else
    fail "$MISSING of $CHECKED key tools missing from manifest"
fi

# MI4: Entry count is reasonable (should have 40+ entries)
echo "MI4. Manifest has sufficient entries..."
ENTRY_COUNT=$(grep -cE '^\s+kind\s+:' MANIFEST.honest 2>/dev/null || echo 0)
if [ "$ENTRY_COUNT" -ge 30 ]; then
    pass "Manifest has $ENTRY_COUNT entries (>= 30)"
else
    fail "Manifest has only $ENTRY_COUNT entries (expected >= 30)"
fi

# MI5: All manifest paths resolve under NBS_HOME after install
echo "MI5. Manifest paths resolve under NBS_HOME..."
NBS_HOME="${NBS_HOME:-$HOME/.nbs}"
# Extract all path values from manifest
PATHS=$(grep -oP "path\s*:\s*'\K[^']+" MANIFEST.honest 2>/dev/null || true)
TOTAL_PATHS=0
MISSING_PATHS=0
MISSING_LIST=""
for p in $PATHS; do
    TOTAL_PATHS=$((TOTAL_PATHS + 1))
    # Map repo paths to install paths:
    #   docs/    -> $NBS_HOME/docs/
    #   bin/     -> $NBS_HOME/bin/
    #   commands/ or claude_tools/ -> $NBS_HOME/commands/
    #   concepts/ -> $NBS_HOME/concepts/
    case "$p" in
        docs/*|bin/*|concepts/*)
            resolved="$NBS_HOME/$p"
            ;;
        claude_tools/*)
            # claude_tools/foo.md -> commands/foo.md
            resolved="$NBS_HOME/commands/${p#claude_tools/}"
            ;;
        *)
            # src/ or other non-install paths — must resolve somewhere
            resolved="$NBS_HOME/$p"
            ;;
    esac
    if [ ! -e "$resolved" ]; then
        MISSING_PATHS=$((MISSING_PATHS + 1))
        MISSING_LIST="$MISSING_LIST  $p -> $resolved\n"
    fi
done

if [ "$MISSING_PATHS" -eq 0 ]; then
    pass "All $TOTAL_PATHS manifest paths resolve under $NBS_HOME"
else
    fail "$MISSING_PATHS of $TOTAL_PATHS paths do not resolve under $NBS_HOME"
    printf "$MISSING_LIST" | head -10
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
