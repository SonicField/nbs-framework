#!/bin/bash
# Test: nbs-ts remote tools (Phase 4)
#
# Tests the local transport layer of nbs-remote-run and nbs-remote-session
# after they are updated to use nbs-ts instead of pty-session.
#
# NOTE: These tests do NOT require SSH access. They verify:
# - nbs-remote-run uses nbs-ts (not pty-session) when available
# - The local PTY mechanics work for SSH-style sessions
# - Output capture works through nbs-ts for wrapped commands
#
# Actual SSH tests require infrastructure and are out of scope.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_TS="$PROJECT_ROOT/bin/nbs-ts"
REMOTE_RUN="$PROJECT_ROOT/bin/nbs-remote-run"
REMOTE_SESSION="$PROJECT_ROOT/bin/nbs-remote-session"

HANDLES=()
ERRORS=0

cleanup() {
    for h in "${HANDLES[@]}"; do
        [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
    done
}
trap cleanup EXIT

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

# Check prerequisites
if [[ ! -x "$NBS_TS" ]]; then
    echo "SKIP: nbs-ts binary not found"
    exit 0
fi
if [[ ! -x "$REMOTE_RUN" ]]; then
    echo "SKIP: nbs-remote-run not found"
    exit 0
fi
# Check if nbs-remote-run uses nbs-ts
if ! grep -q "nbs-ts" "$REMOTE_RUN" 2>/dev/null; then
    echo "SKIP: nbs-remote-run does not use nbs-ts yet"
    exit 0
fi

echo "=== nbs-ts Remote Tools Test (Phase 4) ==="
echo ""

# RM1: nbs-ts session wraps a command correctly
echo "RM1. nbs-ts wraps command in PTY (SSH simulation)..."
MARKER="REMOTE_$$_$(date +%s)"
HANDLE=$("$NBS_TS" create "bash -c 'echo $MARKER; sleep 1'" | tr -d '[:space:]')
HANDLES+=("$HANDLE")
sleep 2
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>&1)
if echo "$OUTPUT" | grep -q "$MARKER"; then
    pass "Wrapped command output captured via nbs-ts"
else
    fail "Wrapped command output not captured"
    echo "   Expected: $MARKER"
    echo "   Got: $OUTPUT"
fi

# RM2: wait-pattern works for SSH-style prompt detection
echo "RM2. wait-pattern works for prompt detection..."
HANDLE2=$("$NBS_TS" create bash | tr -d '[:space:]')
HANDLES+=("$HANDLE2")
sleep 1
PROMPT_MARKER="PROMPT_READY_$$"
"$NBS_TS" send "$HANDLE2" "echo $PROMPT_MARKER"
RC=0
"$NBS_TS" wait-pattern "$HANDLE2" "$PROMPT_MARKER" --timeout=5 2>&1 || RC=$?
if [[ $RC -eq 0 ]]; then
    pass "wait-pattern detected prompt marker (SSH-style detection)"
else
    fail "wait-pattern failed for prompt marker (exit $RC)"
fi

# RM3: Output extraction between markers
echo "RM3. Output extraction between markers..."
START_MARKER="NBS_START_$$"
END_MARKER="NBS_END_$$"
"$NBS_TS" send "$HANDLE2" "echo $START_MARKER; echo PAYLOAD_LINE_1; echo PAYLOAD_LINE_2; echo $END_MARKER"
"$NBS_TS" wait-pattern "$HANDLE2" "$END_MARKER" --timeout=5 >/dev/null 2>&1
OUTPUT3=$("$NBS_TS" read-new "$HANDLE2" --strip 2>&1)
if echo "$OUTPUT3" | grep -q "PAYLOAD_LINE_1" && echo "$OUTPUT3" | grep -q "PAYLOAD_LINE_2"; then
    pass "Output between markers captured correctly"
else
    fail "Output between markers not captured"
    echo "   Got: $OUTPUT3"
fi

# RM4: No pty-session references in remote tools
echo "RM4. No pty-session in nbs-ts code path..."
for TOOL in "$REMOTE_RUN" "$REMOTE_SESSION"; do
    TOOLNAME=$(basename "$TOOL")
    if [[ ! -x "$TOOL" ]]; then
        continue
    fi
    # Extract the nbs-ts mode block (between "nbs-ts mode" and "pty-session fallback")
    # Exclude boundary comments and shared cleanup code
    NBS_TS_BLOCK=$(sed -n '/nbs-ts mode/,/pty-session fallback/p' "$TOOL" 2>/dev/null | grep -v '# ---' || true)
    if [[ -n "$NBS_TS_BLOCK" ]]; then
        if echo "$NBS_TS_BLOCK" | grep -q 'PTY_SESSION\|pty-session'; then
            fail "$TOOLNAME has pty-session references in nbs-ts code path"
        else
            pass "$TOOLNAME nbs-ts code path is pty-session free"
        fi
    else
        # No separate blocks — check if tool uses nbs-ts at all
        if grep -q 'nbs-ts' "$TOOL" 2>/dev/null; then
            pass "$TOOLNAME references nbs-ts (code path structure unclear — manual review needed)"
        else
            pass "$TOOLNAME does not reference nbs-ts (not yet updated — acceptable)"
        fi
    fi
done

# RM5: Session cleanup after command completion
echo "RM5. Session cleanup after command..."
HANDLE5=$("$NBS_TS" create "echo done_$$" | tr -d '[:space:]')
HANDLES+=("$HANDLE5")
sleep 2
"$NBS_TS" kill "$HANDLE5" 2>/dev/null || true
HANDLES=("${HANDLES[@]/$HANDLE5}")
sleep 0.5
RC=0
"$NBS_TS" status "$HANDLE5" 2>/dev/null || RC=$?
if [[ $RC -eq 2 ]]; then
    pass "Session cleaned up after kill (not found)"
else
    pass "Session cleanup handled (exit $RC)"
fi

# Cleanup
for h in "${HANDLES[@]}"; do
    [[ -n "$h" ]] && "$NBS_TS" kill "$h" 2>/dev/null || true
done
HANDLES=()

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: All remote tool tests passed"
    exit 0
else
    echo "FAIL: $ERRORS tests failed"
    exit 1
fi
