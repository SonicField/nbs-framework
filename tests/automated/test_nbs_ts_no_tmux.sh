#!/bin/bash
# Test: nbs-ts Phase 6 — tmux removal verification
#
# Tests NT1-NT2 from nbs-ts-test-plan.md
# This is the final gate: no tmux references remain in src/ or bin/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

ERRORS=0

pass() { echo "   PASS: $1"; }
fail() { echo "   FAIL: $1"; ERRORS=$((ERRORS + 1)); }

echo "=== nbs-ts Phase 6 Verification — No tmux ==="
echo ""

# NT1: No tmux references in source or bin
echo "NT1. No tmux references in src/ or bin/..."
TMUX_HITS=$(grep -rn 'tmux\|pty-session\|capture-pane\|send-keys\|pipe-pane\|has-session\|kill-session\|new-session' \
    "$PROJECT_ROOT/src/" "$PROJECT_ROOT/bin/" \
    --include='*.c' --include='*.h' --include='*.sh' \
    2>/dev/null || true)

if [[ -z "$TMUX_HITS" ]]; then
    pass "Zero tmux/pty-session references in src/ and bin/"
else
    HITCOUNT=$(echo "$TMUX_HITS" | wc -l)
    fail "$HITCOUNT tmux/pty-session references remain:"
    echo "$TMUX_HITS" | head -20
    if [[ $HITCOUNT -gt 20 ]]; then
        echo "   ... ($((HITCOUNT - 20)) more)"
    fi
fi

# NT1b: Also check for pty-session binary and lock script
echo "NT1b. pty-session artifacts removed..."
ARTIFACTS_FOUND=0
if [[ -d "$PROJECT_ROOT/src/nbs-pty-session" ]]; then
    fail "src/nbs-pty-session/ directory still exists"
    ARTIFACTS_FOUND=1
fi
if [[ -f "$PROJECT_ROOT/bin/pty-session" ]]; then
    fail "bin/pty-session binary still exists"
    ARTIFACTS_FOUND=1
fi
if [[ -f "$PROJECT_ROOT/bin/pty-session-lock" ]]; then
    fail "bin/pty-session-lock still exists"
    ARTIFACTS_FOUND=1
fi
if [[ -f "$PROJECT_ROOT/src/nbs-sidecar/transport_tmux.c" ]]; then
    fail "src/nbs-sidecar/transport_tmux.c still exists"
    ARTIFACTS_FOUND=1
fi
if [[ $ARTIFACTS_FOUND -eq 0 ]]; then
    pass "All pty-session and tmux artifacts removed"
fi

# NT2: Full nbs-ts test suite passes without tmux
echo "NT2. Full nbs-ts test suite passes..."
SUITE_PASS=0
SUITE_FAIL=0
for t in "$PROJECT_ROOT/tests/automated/test_nbs_ts_"*.sh; do
    TNAME=$(basename "$t")
    # Skip ourselves to avoid infinite recursion
    [[ "$TNAME" == "test_nbs_ts_no_tmux.sh" ]] && continue
    if timeout 120 bash "$t" >/dev/null 2>&1; then
        SUITE_PASS=$((SUITE_PASS + 1))
    else
        SUITE_FAIL=$((SUITE_FAIL + 1))
        fail "Test suite failed: $TNAME"
    fi
done
if [[ $SUITE_FAIL -eq 0 ]]; then
    pass "All $SUITE_PASS nbs-ts test suites pass"
else
    fail "$SUITE_FAIL/$((SUITE_PASS + SUITE_FAIL)) test suites failed"
fi

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: Phase 6 verification complete — tmux fully removed"
    exit 0
else
    echo "FAIL: $ERRORS issues found"
    exit 1
fi
