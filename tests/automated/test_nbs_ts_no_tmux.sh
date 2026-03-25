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

# NT1b: Check tmux-specific artifacts are removed
# Note: pty-session is a standalone PTY multiplexer (NOT tmux) — it stays.
echo "NT1b. tmux artifacts removed..."
ARTIFACTS_FOUND=0
if [[ -f "$PROJECT_ROOT/src/nbs-sidecar/transport_tmux.c" ]]; then
    fail "src/nbs-sidecar/transport_tmux.c still exists"
    ARTIFACTS_FOUND=1
fi
if [[ $ARTIFACTS_FOUND -eq 0 ]]; then
    pass "All tmux-specific artifacts removed"
fi

# NT2 removed — it recursively ran all test_nbs_ts_*.sh tests, each of
# which spawns nbs-ts sessions. When multiple agents ran run_all.sh
# simultaneously, NT2 cascaded into 10,000+ processes (fork bomb).
# Individual nbs-ts tests are run directly by run_all.sh — NT2 was
# redundant and dangerous.
echo "NT2. (removed — individual tests run by run_all.sh)"
pass "NT2 skipped (fork bomb prevention)"

echo ""
echo "=== Result ==="
if [[ $ERRORS -eq 0 ]]; then
    echo "PASS: Phase 6 verification complete — tmux fully removed"
    exit 0
else
    echo "FAIL: $ERRORS issues found"
    exit 1
fi
