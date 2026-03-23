#!/bin/bash
# nbs-chat-terminal-restart.sh — Auto-restart the NBS agent team
#
# Called by the terminal watchdog daemon when it detects <3 agent
# sessions alive. Performs a full Level 4 cold restart:
#   1. Kill all agent sessions and sidecars
#   2. Run digest to preserve institutional memory
#   3. Reset cursors
#   4. Spawn fresh agents
#   5. Inject skills
#   6. Post continuation directive
#
# Usage:
#   nbs-chat-terminal-restart.sh <project-root> <chat-file>
#
# Exit codes:
#   0 - Restart completed
#   1 - Error
#   4 - Invalid arguments

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: nbs-chat-terminal-restart.sh <project-root> <chat-file>" >&2
    exit 4
fi

PROJECT_ROOT="$1"
CHAT_FILE="$2"

[[ -d "$PROJECT_ROOT" ]] || { echo "Error: project root not found: $PROJECT_ROOT" >&2; exit 1; }
[[ -f "$CHAT_FILE" ]] || { echo "Error: chat file not found: $CHAT_FILE" >&2; exit 1; }

cd "$PROJECT_ROOT"

# Find tools: prefer .nbs/bin/ (installed projects), fall back to bin/ (framework itself)
if [[ -x ".nbs/bin/nbs-chat" ]]; then
    NBS_BIN=".nbs/bin"
elif [[ -x "bin/nbs-chat" ]]; then
    NBS_BIN="bin"
else
    echo "Error: cannot find nbs-chat in .nbs/bin/ or bin/" >&2
    exit 1
fi
NBS_CHAT="${NBS_BIN}/nbs-chat"
NBS_DIGEST="${NBS_BIN}/nbs-digest-spawn"
NBS_TS="${NBS_BIN}/nbs-ts"
USE_NBS_TS=0
[[ -x "$NBS_TS" ]] && USE_NBS_TS=1

[[ -x "$NBS_CHAT" ]] || { echo "Error: nbs-chat not found at $NBS_CHAT" >&2; exit 1; }

# Derive session tag from chat filename (matches watchdog logic in terminal.c)
# live.chat → "live", nn.Module.chat → "nn-Module"
CHAT_BASE=$(basename "$CHAT_FILE" .chat)
CHAT_TAG="${CHAT_BASE//./-}"

echo "[watchdog] Restarting team for ${CHAT_TAG}..."

# 1. Kill all agent sessions and sidecars for this chat
if [[ "$USE_NBS_TS" == "1" ]]; then
    # Kill nbs-ts sessions by listing and matching handles stored in metadata
    while IFS=$'\t' read -r handle status cmd; do
        [[ -n "$handle" ]] || continue
        "$NBS_TS" kill "$handle" 2>/dev/null || true
    done < <("$NBS_TS" list 2>/dev/null || true)
    # Kill sidecars by handle pattern
    for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
        pkill -f "nbs-sidecar.*--handle=${h}" 2>/dev/null || true
    done
else
    for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
        tmux kill-session -t "nbs-${h}-${CHAT_TAG}" 2>/dev/null || true
    done
    for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
        pane=$(tmux display-message -t "nbs-${h}-${CHAT_TAG}" -p '#{pane_id}' 2>/dev/null) || continue
        pkill -f "nbs-sidecar.*--pane-id=${pane}" 2>/dev/null || true
    done
fi
rm -f .nbs/pids/*.pid 2>/dev/null || true

sleep 2

# 2. Run digest (preserves institutional memory across restarts)
# Skip for empty/new chats — nothing to digest, and the digest worker
# would hang waiting for a completion signal that never comes.
CHAT_LINES=$(wc -l < "$CHAT_FILE" 2>/dev/null || echo 0)
if [[ "$CHAT_LINES" -le 10 ]]; then
    echo "[watchdog] New chat ($CHAT_LINES lines) — skipping digest" >&2
elif [[ -x "$NBS_DIGEST" ]]; then
    bash "$NBS_DIGEST" "$CHAT_FILE" >/dev/null 2>&1 || {
        echo "[watchdog] Warning: digest failed, continuing without it" >&2
    }
else
    echo "[watchdog] Warning: nbs-digest-spawn not found, skipping digest" >&2
fi

# 2b. Ensure bus events directory and registry entries exist
CHAT_ABS=$(cd "$(dirname "$CHAT_FILE")" && pwd)/$(basename "$CHAT_FILE")
EVENTS_DIR="${PROJECT_ROOT}/.nbs/events"
mkdir -p "${EVENTS_DIR}/processed"
for handle in scribe gatekeeper testkeeper supervisor generalist theologian; do
    REG="${PROJECT_ROOT}/.nbs/control-registry-${handle}"
    # Ensure chat entry
    if ! grep -qF "chat:${CHAT_ABS}" "$REG" 2>/dev/null; then
        echo "chat:${CHAT_ABS}" >> "$REG"
    fi
    # Ensure bus entry
    if ! grep -qF "bus:${EVENTS_DIR}" "$REG" 2>/dev/null; then
        echo "bus:${EVENTS_DIR}" >> "$REG"
    fi
done

# 3. Reset cursors to current end
HEADER_LINES=6
MESSAGE_COUNT=$(( $(wc -l < "$CHAT_FILE") - HEADER_LINES ))
for handle in scribe gatekeeper testkeeper supervisor generalist theologian; do
    if [ -f "${CHAT_FILE}.cursors" ]; then
        if grep -q "^${handle}=" "${CHAT_FILE}.cursors" 2>/dev/null; then
            sed -i "s/^${handle}=.*/${handle}=${MESSAGE_COUNT}/" "${CHAT_FILE}.cursors"
        else
            echo "${handle}=${MESSAGE_COUNT}" >> "${CHAT_FILE}.cursors"
        fi
    fi
done

# 4. Spawn agents (scribe first, 5s stagger)
declare -A AGENT_HANDLES
if [[ "$USE_NBS_TS" == "1" ]]; then
    for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
        AGENT_HANDLES[$h]=$("$NBS_TS" create "cd $(printf '%q' "$PROJECT_ROOT") && NBS_HANDLE=${h} NBS_TRANSPORT=ts ${NBS_BIN}/nbs-claude --dangerously-skip-permissions") || {
            echo "[watchdog] Error: failed to create nbs-ts session for $h" >&2
            continue
        }
        sleep 5
    done
else
    for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
        tmux new-session -d -s "nbs-${h}-${CHAT_TAG}" -c "$PROJECT_ROOT" \
            "NBS_HANDLE=${h} ${NBS_BIN}/nbs-claude --dangerously-skip-permissions"
        sleep 5
    done
fi

# 5. Wait for init, inject skills
sleep 15
if [[ "$USE_NBS_TS" == "1" ]]; then
    for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
        [[ -n "${AGENT_HANDLES[$h]:-}" ]] || continue
        local_handle="${AGENT_HANDLES[$h]}"
        case "$h" in
            scribe)     "$NBS_TS" send "$local_handle" "/nbs-scribe" 2>/dev/null || true ;;
            gatekeeper) "$NBS_TS" send "$local_handle" "/nbs-gatekeeper" 2>/dev/null || true ;;
            testkeeper) "$NBS_TS" send "$local_handle" "/nbs-testkeeper" 2>/dev/null || true ;;
            theologian) "$NBS_TS" send "$local_handle" "/nbs-theologian" 2>/dev/null || true ;;
            generalist) "$NBS_TS" send "$local_handle" "/nbs-teams-chat" 2>/dev/null || true ;;
            supervisor) "$NBS_TS" send "$local_handle" "/nbs-supervisor" 2>/dev/null || true ;;
        esac
        sleep 1
    done
else
    tmux send-keys -t "nbs-scribe-${CHAT_TAG}" "/nbs-scribe" Enter 2>/dev/null || true; sleep 1
    tmux send-keys -t "nbs-gatekeeper-${CHAT_TAG}" "/nbs-gatekeeper" Enter 2>/dev/null || true; sleep 1
    tmux send-keys -t "nbs-testkeeper-${CHAT_TAG}" "/nbs-testkeeper" Enter 2>/dev/null || true; sleep 1
    tmux send-keys -t "nbs-theologian-${CHAT_TAG}" "/nbs-theologian" Enter 2>/dev/null || true; sleep 1
    tmux send-keys -t "nbs-generalist-${CHAT_TAG}" "/nbs-teams-chat" Enter 2>/dev/null || true; sleep 1
    tmux send-keys -t "nbs-supervisor-${CHAT_TAG}" "/nbs-supervisor" Enter 2>/dev/null || true
fi

# 6. Post continuation directive
# The digest's CONTINUATION section determines what the team should do.
# Do not hardcode a plan — let the digest's analysis of the prior session
# drive the direction. If the digest identified GOALS, pursue them.
# If REVIEW, ask the human leader for direction.
"$NBS_CHAT" send "$CHAT_FILE" supervisor \
    "@team Auto-restart by terminal watchdog. Read the chat digest above — it contains a CONTINUATION section with your next steps. If CONTINUATION: GOALS, create a plan to pursue those goals and begin work immediately. If CONTINUATION: REVIEW, review the prior session and propose 3 candidate goals to the human leader — do not begin work until Alex confirms a direction. Diagnosis without implementation is not progress." 2>/dev/null || true

echo "[watchdog] Team restarted successfully"
