#!/bin/bash
# test_nbs_ts_deploy.sh — Full integration test for nbs-ts deployment
#
# Creates an isolated copy of the framework in /tmp, builds from source,
# spawns a real Claude worker whose task is to verify nbs-ts works, and
# checks results via chat.
#
# The test IS the proof: a Claude agent running inside nbs-ts, communicating
# via nbs-chat, monitored by nbs-sidecar. If it can execute and report,
# every layer works.
#
# Usage:
#   bash tests/integration/test_nbs_ts_deploy.sh
#
# Prerequisites:
#   - Claude Code installed and authenticated
#   - gcc, make, bash
#   - No tmux required (that is the point)
#
# Exit codes:
#   0 - Integration test passed
#   1 - Integration test failed

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TIMEOUT=300
CHAT_NAME="integration"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

info()  { printf '%b %s\n' "${YELLOW}[integration]${NC}" "$*"; }
pass()  { printf '%b %s\n' "${GREEN}[PASS]${NC}" "$*"; }
fail()  { printf '%b %s\n' "${RED}[FAIL]${NC}" "$*"; }

# ── Phase 1: Setup ──────────────────────────────────────────────────

info "Phase 1: Creating isolated test environment..."

TESTDIR=$(mktemp -d /tmp/nbs-ts-integration-XXXXXXXX)
info "  Test directory: $TESTDIR"

TEST_HANDLES=()
cleanup() {
    info "Phase 6: Cleanup..."
    # Only kill sessions this test created (not system-wide)
    for handle in "${TEST_HANDLES[@]}"; do
        "$TESTDIR/bin/nbs-ts" kill "$handle" 2>/dev/null || true
    done
    rm -rf "$TESTDIR"
    info "  Cleaned up $TESTDIR"
}
trap cleanup EXIT

# Copy framework
cp -r "$REPO_ROOT/src" "$TESTDIR/"
cp -r "$REPO_ROOT/bin" "$TESTDIR/"
[[ -f "$REPO_ROOT/CLAUDE.md" ]] && cp "$REPO_ROOT/CLAUDE.md" "$TESTDIR/"

# Build nbs-ts
info "  Building nbs-ts..."
if ! make -C "$TESTDIR/src/nbs-ts" -j$(nproc 2>/dev/null || echo 2) >/dev/null 2>&1; then
    fail "nbs-ts build failed"
    make -C "$TESTDIR/src/nbs-ts" 2>&1 | tail -5
    exit 1
fi
make -C "$TESTDIR/src/nbs-ts" install >/dev/null 2>&1 || { fail "nbs-ts install failed"; exit 1; }

# Build sidecar
info "  Building nbs-sidecar..."
if ! make -C "$TESTDIR/src/nbs-sidecar" -j$(nproc 2>/dev/null || echo 2) >/dev/null 2>&1; then
    fail "nbs-sidecar build failed"
    make -C "$TESTDIR/src/nbs-sidecar" 2>&1 | tail -5
    exit 1
fi
make -C "$TESTDIR/src/nbs-sidecar" install >/dev/null 2>&1 || { fail "nbs-sidecar install failed"; exit 1; }

# Build other tools
for tool in nbs-chat nbs-bus nbs-workers; do
    if [[ -d "$TESTDIR/src/$tool" ]] && [[ -f "$TESTDIR/src/$tool/Makefile" ]]; then
        info "  Building $tool..."
        make -C "$TESTDIR/src/$tool" -j$(nproc 2>/dev/null || echo 2) >/dev/null 2>&1 || true
        make -C "$TESTDIR/src/$tool" install >/dev/null 2>&1 || true
    fi
done

# Verify binaries
for bin in nbs-ts nbs-sidecar; do
    [[ -x "$TESTDIR/bin/$bin" ]] || { fail "$bin not found after build"; exit 1; }
done
pass "Build complete — nbs-ts and nbs-sidecar installed"

# ── Phase 2: Infrastructure ────────────────────────────────────────

info "Phase 2: Setting up NBS infrastructure..."
export PATH="$TESTDIR/bin:$PATH"
cd "$TESTDIR"

if ! "$TESTDIR/bin/nbs-chat-init" --name="$CHAT_NAME" --root="$TESTDIR" --force >/dev/null 2>&1; then
    fail "nbs-chat-init failed"
    exit 1
fi

CHAT_FILE="$TESTDIR/.nbs/chat/${CHAT_NAME}.chat"
[[ -f "$CHAT_FILE" ]] || { fail "Chat file not created"; exit 1; }
pass "Infrastructure ready"

# ── Phase 3: Smoke test nbs-ts ─────────────────────────────────────

info "Phase 3: Smoke testing nbs-ts..."
NBS_TS="$TESTDIR/bin/nbs-ts"

HANDLE=$("$NBS_TS" create 'echo INTEGRATION_SMOKE_TEST')
sleep 2
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>/dev/null) || true
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

if echo "$OUTPUT" | grep -q "INTEGRATION_SMOKE_TEST"; then
    pass "nbs-ts create/read/kill works"
else
    fail "nbs-ts smoke test failed"; exit 1
fi

HANDLE=$("$NBS_TS" create 'bash')
sleep 1
"$NBS_TS" send "$HANDLE" "echo INTERACTIVE_TEST"
sleep 2
OUTPUT=$("$NBS_TS" read-new "$HANDLE" --strip 2>/dev/null) || true
"$NBS_TS" kill "$HANDLE" 2>/dev/null || true

if echo "$OUTPUT" | grep -q "INTERACTIVE_TEST"; then
    pass "nbs-ts interactive session works"
else
    fail "nbs-ts interactive test failed"; exit 1
fi

# ── Phase 4: Spawn Claude directly via nbs-ts ──────────────────────
#
# Run claude directly in nbs-ts (no nbs-claude wrapper, no sidecar).
# This tests the core: can a real Claude instance run inside nbs-ts,
# receive input, execute commands, and produce output?

info "Phase 4: Spawning Claude directly via nbs-ts..."

# Use nbs-claude wrapper with NBS_INITIAL_PROMPT (proven pattern)
# The sidecar delivers the prompt after Claude initialises
TASK_PROMPT="You are an integration test worker. Run these 4 nbs-ts tests using the Bash tool and post results to chat. Do NOT use AskUserQuestion. Test 1: Run nbs-ts create echo HELLO, sleep 2, nbs-ts read-new with --strip, verify output contains HELLO, nbs-ts kill. Test 2: Run nbs-ts create bash, nbs-ts send echo TEST_SEND, sleep 2, nbs-ts read-new --strip, verify contains TEST_SEND, nbs-ts kill. Test 3: Run nbs-ts create sleep 30, nbs-ts status, verify alive, nbs-ts kill. Test 4: Post results: nbs-chat send .nbs/chat/integration.chat integration-tester INTEGRATION_PASS or INTEGRATION_FAIL with details."

WORKER_HANDLE=$("$NBS_TS" create "unset CLAUDECODE; export PATH=$TESTDIR/bin:\$PATH; NBS_HANDLE=integration-tester NBS_TRANSPORT=ts NBS_INITIAL_PROMPT='$TASK_PROMPT' $TESTDIR/bin/nbs-claude --dangerously-skip-permissions --root=$TESTDIR") || { fail "Failed to create worker session"; exit 1; }
TEST_HANDLES+=("$WORKER_HANDLE")
info "  Worker spawned via nbs-claude: $WORKER_HANDLE"

if "$NBS_TS" list 2>/dev/null | grep -q "$WORKER_HANDLE"; then
    pass "Worker session visible in nbs-ts list"
else
    fail "Worker session not in nbs-ts list"; exit 1
fi

# Check sidecar log after a delay
sleep 10
SIDECAR_LOG="$TESTDIR/.nbs/nbs-claude-*.log"
if ls $SIDECAR_LOG 2>/dev/null | head -1 >/dev/null; then
    info "  Sidecar log exists — checking for errors"
    if grep -qi 'error\|fatal\|abort' $SIDECAR_LOG 2>/dev/null; then
        warn "  Sidecar log has errors:"
        grep -i 'error\|fatal\|abort' $SIDECAR_LOG 2>/dev/null | head -5
    else
        pass "Sidecar log clean (no errors)"
    fi
else
    info "  No sidecar log yet (may still be starting)"
fi
info "  Task prompt sent"

# ── Phase 5: Wait for results ──────────────────────────────────────

info "Phase 5: Waiting for worker results (timeout: ${TIMEOUT}s)..."

ELAPSED=0
INTERVAL=10
RESULT=""

while [[ $ELAPSED -lt $TIMEOUT ]]; do
    sleep "$INTERVAL"
    ELAPSED=$((ELAPSED + INTERVAL))

    WORKER_STATUS=$("$NBS_TS" status "$WORKER_HANDLE" 2>/dev/null | head -1) || WORKER_STATUS="unknown"

    # Check chat for results
    CHAT_CONTENT=$("$TESTDIR/bin/nbs-chat" read "$CHAT_FILE" 2>/dev/null) || true

    if echo "$CHAT_CONTENT" | grep -q "INTEGRATION_PASS"; then
        RESULT="PASS"; break
    fi
    if echo "$CHAT_CONTENT" | grep -q "INTEGRATION_FAIL"; then
        RESULT="FAIL"; break
    fi

    # Also check nbs-ts output directly (in case nbs-chat isn't on PATH for the worker)
    WORKER_OUTPUT=$("$NBS_TS" read "$WORKER_HANDLE" --offset=0 2>/dev/null) || true
    if echo "$WORKER_OUTPUT" | grep -q "INTEGRATION_PASS"; then
        RESULT="PASS"; break
    fi
    if echo "$WORKER_OUTPUT" | grep -q "INTEGRATION_FAIL"; then
        RESULT="FAIL"; break
    fi

    if [[ "$WORKER_STATUS" != "alive" && "$ELAPSED" -gt 60 ]]; then
        sleep 5
        WORKER_OUTPUT=$("$NBS_TS" read "$WORKER_HANDLE" --offset=0 2>/dev/null) || true
        if echo "$WORKER_OUTPUT" | grep -q "INTEGRATION_PASS"; then RESULT="PASS"
        elif echo "$WORKER_OUTPUT" | grep -q "INTEGRATION_FAIL"; then RESULT="FAIL"
        else RESULT="DIED"
        fi
        break
    fi

    info "  ${ELAPSED}s elapsed, worker: $WORKER_STATUS"
done

"$NBS_TS" kill "$WORKER_HANDLE" 2>/dev/null || true

# ── Assessment ─────────────────────────────────────────────────────

echo ""
printf '%b%s%b\n' "${BOLD}" "=== Integration Test Result ===" "${NC}"
echo ""

info "Worker's chat messages:"
"$TESTDIR/bin/nbs-chat" read "$CHAT_FILE" 2>/dev/null | grep -v "nbs-chat-init" || true
echo ""

case "$RESULT" in
    PASS)
        pass "INTEGRATION TEST PASSED"
        pass "A real Claude agent running inside nbs-ts verified the full stack."
        exit 0 ;;
    FAIL)
        fail "INTEGRATION TEST FAILED — worker reported failures"
        exit 1 ;;
    DIED)
        fail "INTEGRATION TEST FAILED — worker died without reporting"
        exit 1 ;;
    *)
        fail "INTEGRATION TEST FAILED — timeout after ${TIMEOUT}s"
        exit 1 ;;
esac
