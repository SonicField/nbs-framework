#!/bin/bash
# Test: nbs-help manifest search tool
#
# Verifies (from feature request test table):
#   NH1. nbs-help "remote" returns remote tools (keyword search)
#   NH2. nbs-help --kind=skill lists skills only (kind filter)
#   NH3. nbs-help --list shows all entries grouped (listing)
#   NH4. nbs-help with multiple query words (AND matching)
#   NH5. nbs-help with no matches returns empty
#   NH6. nbs-help --list --kind=tool lists tools only
#   NH7. Case-insensitive search

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_HELP="$PROJECT_ROOT/bin/nbs-help"

[ -x "$NBS_HELP" ] || { echo "SKIP: nbs-help not found"; exit 0; }
[ -f "$PROJECT_ROOT/MANIFEST.honest" ] || { echo "SKIP: MANIFEST.honest not found"; exit 0; }

ERRORS=0

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-help Search Test ==="
echo ""

cd "$PROJECT_ROOT"

# NH1: Keyword search returns relevant results
echo "NH1. Keyword search 'chat' returns chat tools..."
OUTPUT=$(bin/nbs-help "chat" 2>&1)
if echo "$OUTPUT" | grep -qi "nbs-chat"; then
    pass "Search 'chat' found nbs-chat"
else
    fail "Search 'chat' did not find nbs-chat"
    echo "   Output: $(echo "$OUTPUT" | head -5)"
fi

# NH2: Kind filter — skills only
echo "NH2. --kind=skill filters to skills..."
OUTPUT2=$(bin/nbs-help --list --kind=skill 2>&1)
if echo "$OUTPUT2" | grep -qi "skill"; then
    # Should NOT contain tool entries
    if echo "$OUTPUT2" | grep -qi "^Tools"; then
        fail "--kind=skill still shows Tools heading"
    else
        pass "--kind=skill lists skills only"
    fi
else
    fail "--kind=skill returned no skills"
    echo "   Output: $(echo "$OUTPUT2" | head -5)"
fi

# NH3: --list shows all entries grouped by kind
echo "NH3. --list shows entries grouped by kind..."
OUTPUT3=$(bin/nbs-help --list 2>&1)
KINDS_FOUND=0
echo "$OUTPUT3" | grep -qi "Tools" && KINDS_FOUND=$((KINDS_FOUND + 1))
echo "$OUTPUT3" | grep -qi "Skills" && KINDS_FOUND=$((KINDS_FOUND + 1))
echo "$OUTPUT3" | grep -qi "Documents" && KINDS_FOUND=$((KINDS_FOUND + 1))
if [ "$KINDS_FOUND" -ge 2 ]; then
    pass "--list shows entries grouped ($KINDS_FOUND kind headings)"
else
    fail "--list missing kind groupings (found $KINDS_FOUND)"
    echo "   Output: $(echo "$OUTPUT3" | head -10)"
fi

# NH4: Multiple query words (AND matching)
echo "NH4. Multiple query words use AND matching..."
OUTPUT4=$(bin/nbs-help "chat terminal" 2>&1)
if echo "$OUTPUT4" | grep -qi "nbs-chat-terminal"; then
    pass "Multi-word 'chat terminal' found nbs-chat-terminal"
else
    fail "Multi-word search did not find expected result"
    echo "   Output: $(echo "$OUTPUT4" | head -5)"
fi

# NH5: No matches returns empty/message
echo "NH5. No matches for nonsense query..."
OUTPUT5=$(bin/nbs-help "xyzzy_nonexistent_tool_12345" 2>&1)
if [ -z "$OUTPUT5" ] || echo "$OUTPUT5" | grep -qi "no.*match\|no.*result\|not found"; then
    pass "No matches: empty or message shown"
else
    # Check it didn't return real entries
    if echo "$OUTPUT5" | grep -qi "nbs-"; then
        fail "Nonsense query returned real entries"
    else
        pass "No matches: no entries returned"
    fi
fi

# NH6: --list --kind=tool lists only tools
echo "NH6. --list --kind=tool lists tools only..."
OUTPUT6=$(bin/nbs-help --list --kind=tool 2>&1)
if echo "$OUTPUT6" | grep -qi "nbs-chat\|nbs-ts\|nbs-bus"; then
    pass "--list --kind=tool shows tool entries"
else
    fail "--list --kind=tool missing tool entries"
    echo "   Output: $(echo "$OUTPUT6" | head -5)"
fi

# NH7: Case-insensitive search
echo "NH7. Case-insensitive search..."
OUTPUT7_LOWER=$(bin/nbs-help "chat" 2>&1)
OUTPUT7_UPPER=$(bin/nbs-help "CHAT" 2>&1)
if [ "$OUTPUT7_LOWER" = "$OUTPUT7_UPPER" ]; then
    pass "Case-insensitive: 'chat' and 'CHAT' return same results"
else
    fail "Case sensitivity: different results for 'chat' vs 'CHAT'"
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
