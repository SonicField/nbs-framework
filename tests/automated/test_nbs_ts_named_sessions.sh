#!/bin/bash
# Test: nbs-ts named sessions — create/list/find with --name flag
#
# Tests N1-N10 from nbs-ts-named-sessions-plan.md
# Falsification: each test targets a specific named-session behaviour.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"

HANDLES=()
ERRORS=0
SESSIONS_DIR="$HOME/.nbs-ts/sessions"

cleanup() {
    for h in "${HANDLES[@]}"; do
        [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-ts Named Sessions Test ==="
echo ""

# N1: Create with --name
echo "N1. Create with --name=test-alpha..."
H1=$("$NBS_TS" create --name=test-alpha bash 2>&1 | tr -d '[:space:]')
if [[ $? -eq 0 ]] && [[ ${#H1} -eq 8 ]]; then
    HANDLES+=("$H1")
    NAME_FILE="$SESSIONS_DIR/$H1/name"
    if [[ -f "$NAME_FILE" ]]; then
        NAME_CONTENT=$(cat "$NAME_FILE")
        if [[ "$NAME_CONTENT" == "test-alpha" ]]; then
            pass "Created session $H1 with name 'test-alpha'"
        else
            fail "Name file contains '$NAME_CONTENT', expected 'test-alpha'"
        fi
    else
        fail "Name file does not exist at $NAME_FILE"
    fi
else
    fail "Create with --name returned unexpected handle: '$H1'"
fi

sleep 0.5

# N2: Create without --name — no name file
echo "N2. Create without --name..."
H2=$("$NBS_TS" create bash 2>&1 | tr -d '[:space:]')
if [[ $? -eq 0 ]] && [[ ${#H2} -eq 8 ]]; then
    HANDLES+=("$H2")
    NAME_FILE2="$SESSIONS_DIR/$H2/name"
    if [[ ! -f "$NAME_FILE2" ]]; then
        pass "No name file for unnamed session $H2"
    else
        fail "Unnamed session has name file (should not exist)"
    fi
else
    fail "Create without --name returned unexpected handle: '$H2'"
fi

sleep 0.5

# N3: Name validation rejects bad characters
echo "N3. Name validation rejects bad chars..."
BEFORE_COUNT=$(ls -1 "$SESSIONS_DIR" 2>/dev/null | wc -l)
H3=$("$NBS_TS" create --name='bad name!' bash 2>&1) && RC3=$? || RC3=$?
AFTER_COUNT=$(ls -1 "$SESSIONS_DIR" 2>/dev/null | wc -l)
if [[ $RC3 -ne 0 ]] && [[ "$AFTER_COUNT" -eq "$BEFORE_COUNT" ]]; then
    pass "Rejected 'bad name!' with exit code $RC3, no session created"
else
    fail "Expected non-zero exit and no new session (rc=$RC3, before=$BEFORE_COUNT, after=$AFTER_COUNT)"
    # Try to clean up if a session was created
    if [[ ${#H3} -eq 8 ]]; then
        HANDLES+=("$H3")
    fi
fi

# N4: Name validation rejects too long (65 chars)
echo "N4. Name validation rejects too-long name..."
LONG_NAME=$(python3 -c "print('a'*65)")
BEFORE_COUNT=$(ls -1 "$SESSIONS_DIR" 2>/dev/null | wc -l)
H4=$("$NBS_TS" create --name="$LONG_NAME" bash 2>&1) && RC4=$? || RC4=$?
AFTER_COUNT=$(ls -1 "$SESSIONS_DIR" 2>/dev/null | wc -l)
if [[ $RC4 -ne 0 ]] && [[ "$AFTER_COUNT" -eq "$BEFORE_COUNT" ]]; then
    pass "Rejected 65-char name with exit code $RC4"
else
    fail "Expected non-zero exit for 65-char name (rc=$RC4)"
    if [[ ${#H4} -eq 8 ]]; then
        HANDLES+=("$H4")
    fi
fi

# N5: List shows name column for named + unnamed sessions
echo "N5. List shows name column..."
LIST_OUTPUT=$("$NBS_TS" list 2>/dev/null)
# H1 should show test-alpha, H2 should show -
if echo "$LIST_OUTPUT" | grep -q "${H1}.*test-alpha" && \
   echo "$LIST_OUTPUT" | grep -q "${H2}.*-"; then
    pass "List shows name for named session and '-' for unnamed"
else
    fail "List output does not show expected name column"
    echo "   Output:"
    echo "$LIST_OUTPUT" | sed 's/^/      /'
fi

# N6: List --name= filters by substring
echo "N6. List --name=alpha filters correctly..."
# Create a second named session for contrast
H6=$("$NBS_TS" create --name=test-beta bash 2>&1 | tr -d '[:space:]')
HANDLES+=("$H6")
sleep 0.5

FILTERED=$("$NBS_TS" list --name=alpha 2>/dev/null)
if echo "$FILTERED" | grep -q "$H1" && \
   ! echo "$FILTERED" | grep -q "$H6"; then
    pass "list --name=alpha shows test-alpha, hides test-beta"
else
    fail "list --name=alpha did not filter correctly"
    echo "   Output:"
    echo "$FILTERED" | sed 's/^/      /'
fi

# N7: Find exact match
echo "N7. Find exact match..."
FOUND=$("$NBS_TS" find test-alpha 2>/dev/null) && RC7=$? || RC7=$?
FOUND=$(echo "$FOUND" | tr -d '[:space:]')
if [[ $RC7 -eq 0 ]] && [[ "$FOUND" == "$H1" ]]; then
    pass "find test-alpha returned handle $H1"
else
    fail "find test-alpha: rc=$RC7, output='$FOUND', expected '$H1'"
fi

# N8: Find no match
echo "N8. Find no match..."
NOTFOUND=$("$NBS_TS" find nonexistent 2>/dev/null) && RC8=$? || RC8=$?
if [[ $RC8 -eq 2 ]]; then
    pass "find nonexistent returned exit code 2"
else
    fail "find nonexistent: expected exit 2, got $RC8"
fi

# N9: Find partial no match (find is exact, not substring)
echo "N9. Find partial no match..."
PARTIAL=$("$NBS_TS" find test 2>/dev/null) && RC9=$? || RC9=$?
if [[ $RC9 -eq 2 ]]; then
    pass "find test (partial) returned exit code 2"
else
    fail "find test (partial): expected exit 2, got $RC9"
fi

# N10: Name survives session death
echo "N10. Name survives session death..."
H10=$("$NBS_TS" create --name=test-mortal "exit 0" 2>&1 | tr -d '[:space:]')
HANDLES+=("$H10")
sleep 2
# Session should be dead but name file should still exist
STATUS10=$("$NBS_TS" status "$H10" 2>&1) || true
NAME10_FILE="$SESSIONS_DIR/$H10/name"
if [[ -f "$NAME10_FILE" ]]; then
    NAME10=$(cat "$NAME10_FILE")
    if [[ "$NAME10" == "test-mortal" ]]; then
        pass "Name 'test-mortal' survives session death"
    else
        fail "Name file contents changed after death: '$NAME10'"
    fi
else
    fail "Name file removed after session death"
fi

# Also check list shows the dead session with its name
LIST_DEAD=$("$NBS_TS" list 2>/dev/null)
if echo "$LIST_DEAD" | grep -q "${H10}.*dead.*test-mortal"; then
    pass "Dead session shows name in list"
else
    fail "Dead session does not show name in list"
    echo "   List output for $H10:"
    echo "$LIST_DEAD" | grep "$H10" | sed 's/^/      /' || echo "      (not found)"
fi

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All named session tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
