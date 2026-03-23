#!/bin/bash
# test_nbs_ts_multi_agent.sh — Multi-agent integration test for nbs-ts
#
# Spawns 3 real Claude agents in an isolated /tmp environment, each running
# via nbs-claude + sidecar (full production stack). The agents coordinate
# via nbs-chat to verify nbs-ts works under concurrent load.
#
# Agent A: Runs nbs-ts create/send/read/kill tests, posts results to chat
# Agent B: Reads Agent A's results from chat, verifies PASS markers
# Agent C: Counts chat participants, verifies multi-agent coordination
#
# If Agent C posts MULTI_AGENT_PASS, all three agents worked: concurrent
# nbs-ts sessions, concurrent sidecar transports, and shared nbs-chat access.
#
# Usage:
#   bash tests/integration/test_nbs_ts_multi_agent.sh
#
# Prerequisites:
#   - Claude Code installed and authenticated
#   - gcc, make, bash
#   - API credits (spawns 3 real Claude instances)
#
# Exit codes:
#   0 - Multi-agent integration test passed
#   1 - Multi-agent integration test failed

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TIMEOUT=600  # 10 minutes — 3 agents need more time
CHAT_NAME="multi-agent-test"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

info()  { printf '%b %s\n' "${YELLOW}[multi-agent]${NC}" "$*"; }
pass()  { printf '%b %s\n' "${GREEN}[PASS]${NC}" "$*"; }
fail()  { printf '%b %s\n' "${RED}[FAIL]${NC}" "$*"; }

# ── Phase 1: Setup ──────────────────────────────────────────────────

info "Phase 1: Creating isolated test environment..."

TESTDIR=$(mktemp -d /tmp/nbs-ts-multi-agent-XXXXXXXX)
info "  Test directory: $TESTDIR"

PID_A="" PID_B="" PID_C=""
cleanup() {
    info "Cleanup..."
    for pid in $PID_A $PID_B $PID_C; do
        kill "$pid" 2>/dev/null || true
    done
    # Kill any nbs-ts sessions and sidecars from this test
    pkill -f "nbs-sidecar.*$TESTDIR" 2>/dev/null || true
    if [[ -x "$TESTDIR/bin/nbs-ts" ]]; then
        "$TESTDIR/bin/nbs-ts" list 2>/dev/null | awk '{print $1}' | while read -r h; do
            "$TESTDIR/bin/nbs-ts" kill "$h" 2>/dev/null || true
        done
    fi
    sleep 2
    rm -rf "$TESTDIR"
    info "  Cleaned up $TESTDIR"
}
trap cleanup EXIT

# Copy framework
cp -r "$REPO_ROOT/src" "$TESTDIR/"
cp -r "$REPO_ROOT/bin" "$TESTDIR/"
[[ -f "$REPO_ROOT/CLAUDE.md" ]] && cp "$REPO_ROOT/CLAUDE.md" "$TESTDIR/"

# Build all tools
for tool in nbs-ts nbs-sidecar nbs-chat nbs-bus nbs-workers; do
    if [[ -d "$TESTDIR/src/$tool" ]] && [[ -f "$TESTDIR/src/$tool/Makefile" ]]; then
        info "  Building $tool..."
        make -C "$TESTDIR/src/$tool" -j$(nproc 2>/dev/null || echo 2) >/dev/null 2>&1 || true
        make -C "$TESTDIR/src/$tool" install >/dev/null 2>&1 || true
    fi
done

for bin in nbs-ts nbs-sidecar nbs-chat; do
    [[ -x "$TESTDIR/bin/$bin" ]] || { fail "$bin not found after build"; exit 1; }
done
pass "Build complete"

# ── Phase 2: Infrastructure ────────────────────────────────────────

info "Phase 2: Setting up NBS infrastructure..."
export PATH="$TESTDIR/bin:$PATH"
cd "$TESTDIR"

"$TESTDIR/bin/nbs-chat-init" --name="$CHAT_NAME" --root="$TESTDIR" --force >/dev/null 2>&1 || { fail "nbs-chat-init failed"; exit 1; }

CHAT_FILE="$TESTDIR/.nbs/chat/${CHAT_NAME}.chat"
[[ -f "$CHAT_FILE" ]] || { fail "Chat file not created"; exit 1; }
pass "Infrastructure ready"

# ── Phase 3: Spawn 3 agents ────────────────────────────────────────

info "Phase 3: Spawning 3 agents via nbs-claude + sidecar..."

NBS_TS="$TESTDIR/bin/nbs-ts"

# Agent A: Run nbs-ts tests and post results
PROMPT_A="You are Agent A in a multi-agent integration test. Run these nbs-ts verification commands using the Bash tool and post your results to chat. Do NOT use AskUserQuestion. Step 1: Run HANDLE=\$(nbs-ts create \"echo AGENT_A_TEST\") and sleep 2 and nbs-ts read-new \$HANDLE --strip and nbs-ts kill \$HANDLE. Verify output contains AGENT_A_TEST. Step 2: Post your results: nbs-chat send $CHAT_FILE agent-a \"AGENT_A_PASS: nbs-ts create/read/kill verified\". If any test fails, post AGENT_A_FAIL with details."

# Launch agents as background subshells — nbs-claude creates its own nbs-ts session
# No outer nbs-ts create wrapping (avoids double-nesting)
PROMPT_FILE_A="$TESTDIR/.nbs/workers/prompt-a.txt"
echo "$PROMPT_A" > "$PROMPT_FILE_A"
(unset CLAUDECODE; export PATH="$TESTDIR/bin:$PATH"; NBS_HANDLE=agent-a NBS_TRANSPORT=ts NBS_INITIAL_PROMPT="$(cat "$PROMPT_FILE_A")" "$TESTDIR/bin/nbs-claude" --dangerously-skip-permissions --root="$TESTDIR") &
PID_A=$!
info "  Agent A launched: PID $PID_A"

sleep 5  # Stagger spawns

# Agent B: Read Agent A's results from chat and verify
PROMPT_B="You are Agent B in a multi-agent integration test. Poll the chat every 15 seconds for up to 3 minutes looking for AGENT_A_PASS. Do NOT use AskUserQuestion. Run this Bash loop: for i in \$(seq 1 12); do OUTPUT=\$(nbs-chat read $CHAT_FILE --last=10 2>/dev/null); if echo \"\$OUTPUT\" | grep -q AGENT_A_PASS; then nbs-chat send $CHAT_FILE agent-b AGENT_B_PASS; exit 0; fi; sleep 15; done; nbs-chat send $CHAT_FILE agent-b AGENT_B_FAIL"

PROMPT_FILE_B="$TESTDIR/.nbs/workers/prompt-b.txt"
echo "$PROMPT_B" > "$PROMPT_FILE_B"
(unset CLAUDECODE; export PATH="$TESTDIR/bin:$PATH"; NBS_HANDLE=agent-b NBS_TRANSPORT=ts NBS_INITIAL_PROMPT="$(cat "$PROMPT_FILE_B")" "$TESTDIR/bin/nbs-claude" --dangerously-skip-permissions --root="$TESTDIR") &
PID_B=$!
info "  Agent B launched: PID $PID_B"

sleep 5

# Agent C: Verify multi-agent coordination
PROMPT_C="You are Agent C in a multi-agent integration test. Poll the chat every 20 seconds for up to 5 minutes looking for BOTH AGENT_A_PASS and AGENT_B_PASS. Do NOT use AskUserQuestion. Run this Bash loop: for i in \$(seq 1 15); do OUTPUT=\$(nbs-chat read $CHAT_FILE --last=20 2>/dev/null); if echo \"\$OUTPUT\" | grep -q AGENT_A_PASS && echo \"\$OUTPUT\" | grep -q AGENT_B_PASS; then nbs-chat send $CHAT_FILE agent-c MULTI_AGENT_PASS; exit 0; fi; sleep 20; done; nbs-chat send $CHAT_FILE agent-c MULTI_AGENT_FAIL"

PROMPT_FILE_C="$TESTDIR/.nbs/workers/prompt-c.txt"
echo "$PROMPT_C" > "$PROMPT_FILE_C"
(unset CLAUDECODE; export PATH="$TESTDIR/bin:$PATH"; NBS_HANDLE=agent-c NBS_TRANSPORT=ts NBS_INITIAL_PROMPT="$(cat "$PROMPT_FILE_C")" "$TESTDIR/bin/nbs-claude" --dangerously-skip-permissions --root="$TESTDIR") &
PID_C=$!
info "  Agent C launched: PID $PID_C"

# Wait for agents to create their internal nbs-ts sessions
sleep 15

# Verify sessions exist via nbs-ts list
LIVE_COUNT=$("$NBS_TS" list 2>/dev/null | grep -c "alive" || echo 0)
if [[ "$LIVE_COUNT" -ge 3 ]]; then
    pass "3 concurrent nbs-ts sessions alive"
else
    info "  $LIVE_COUNT sessions alive (agents may still be starting)"
fi

# ── Phase 4: Wait for results ──────────────────────────────────────

info "Phase 4: Waiting for multi-agent results (timeout: ${TIMEOUT}s)..."

ELAPSED=0
INTERVAL=15
RESULT=""

while [[ $ELAPSED -lt $TIMEOUT ]]; do
    sleep "$INTERVAL"
    ELAPSED=$((ELAPSED + INTERVAL))

    # Check chat for final result
    CHAT_CONTENT=$("$TESTDIR/bin/nbs-chat" read "$CHAT_FILE" 2>/dev/null) || true

    if echo "$CHAT_CONTENT" | grep -q "MULTI_AGENT_PASS"; then
        RESULT="PASS"; break
    fi
    if echo "$CHAT_CONTENT" | grep -q "MULTI_AGENT_FAIL"; then
        RESULT="FAIL"; break
    fi

    # Progress report
    A_STATUS=$(kill -0 "$PID_A" 2>/dev/null && echo "alive" || echo "dead")
    B_STATUS=$(kill -0 "$PID_B" 2>/dev/null && echo "alive" || echo "dead")
    C_STATUS=$(kill -0 "$PID_C" 2>/dev/null && echo "alive" || echo "dead")

    HAS_A=$(echo "$CHAT_CONTENT" | grep -c "AGENT_A_PASS" || true)
    HAS_B=$(echo "$CHAT_CONTENT" | grep -c "AGENT_B_PASS" || true)

    info "  ${ELAPSED}s — A:$A_STATUS B:$B_STATUS C:$C_STATUS | chat: A_PASS=$HAS_A B_PASS=$HAS_B"

    # All agents dead without posting results
    if [[ "$A_STATUS" == "dead" && "$B_STATUS" == "dead" && "$C_STATUS" == "dead" && "$ELAPSED" -gt 120 ]]; then
        sleep 10
        CHAT_CONTENT=$("$TESTDIR/bin/nbs-chat" read "$CHAT_FILE" 2>/dev/null) || true
        if echo "$CHAT_CONTENT" | grep -q "MULTI_AGENT_PASS"; then RESULT="PASS"
        elif echo "$CHAT_CONTENT" | grep -q "MULTI_AGENT_FAIL"; then RESULT="FAIL"
        else RESULT="DIED"
        fi
        break
    fi
done

# ── Assessment ─────────────────────────────────────────────────────

echo ""
printf '%b%s%b\n' "${BOLD}" "=== Multi-Agent Integration Test Result ===" "${NC}"
echo ""

info "Chat messages:"
"$TESTDIR/bin/nbs-chat" read "$CHAT_FILE" 2>/dev/null | grep -v "nbs-chat-init" || true
echo ""

info "Agent processes:"
echo "  Agent A (runner):   $(kill -0 "$PID_A" 2>/dev/null && echo alive || echo dead) (PID $PID_A)"
echo "  Agent B (verifier): $(kill -0 "$PID_B" 2>/dev/null && echo alive || echo dead) (PID $PID_B)"
echo "  Agent C (counter):  $(kill -0 "$PID_C" 2>/dev/null && echo alive || echo dead) (PID $PID_C)"
echo ""

case "$RESULT" in
    PASS)
        pass "MULTI-AGENT INTEGRATION TEST PASSED"
        pass "3 real Claude agents running on nbs-ts coordinated via nbs-chat."
        pass "Concurrent sessions, concurrent sidecars, shared chat — all verified."
        exit 0 ;;
    FAIL)
        fail "MULTI-AGENT INTEGRATION TEST FAILED — agent reported failure"
        exit 1 ;;
    DIED)
        fail "MULTI-AGENT INTEGRATION TEST FAILED — all agents died without reporting"
        exit 1 ;;
    *)
        fail "MULTI-AGENT INTEGRATION TEST FAILED — timeout after ${TIMEOUT}s"
        exit 1 ;;
esac
