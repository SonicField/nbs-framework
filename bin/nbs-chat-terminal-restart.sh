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
# MUST use absolute paths — the cwd can be reset by the calling environment.
if [[ -x "${PROJECT_ROOT}/.nbs/bin/nbs-chat" ]]; then
    NBS_BIN="${PROJECT_ROOT}/.nbs/bin"
elif [[ -x "${PROJECT_ROOT}/bin/nbs-chat" ]]; then
    NBS_BIN="${PROJECT_ROOT}/bin"
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

# 1. Kill all agent processes, sidecars, and sessions.
# Kill nbs-claude wrappers FIRST (they have cleanup traps that delete
# sessions). Wait for them to exit so their traps complete before we
# spawn new agents — otherwise the old traps race with the new agents
# and can destroy freshly-created sessions.
for h in scribe medic supervisor gatekeeper theologian testkeeper generalist; do
    # Kill nbs-claude wrapper (reads PID from pidfile)
    pidfile="${PROJECT_ROOT}/.nbs/pids/${h}.pid"
    if [[ -f "$pidfile" ]]; then
        oldpid=$(cat "$pidfile" 2>/dev/null)
        if [[ -n "$oldpid" && "$oldpid" =~ ^[0-9]+$ ]]; then
            kill "$oldpid" 2>/dev/null || true
        fi
    fi
    # Kill sidecar
    pkill -f "nbs-sidecar.*--handle=${h}.*${PROJECT_ROOT}" 2>/dev/null || true
done
# Kill nbs-ts sessions — only those belonging to this team (name contains CHAT_TAG)
while IFS=$'\t' read -r handle status name cmd; do
    [[ -n "$handle" ]] || continue
    "$NBS_TS" kill "$handle" 2>/dev/null || true
done < <("$NBS_TS" list --name="$CHAT_TAG" 2>/dev/null || true)

# Wait for old nbs-claude processes to finish their cleanup traps
for h in scribe medic supervisor gatekeeper theologian testkeeper generalist; do
    pidfile="${PROJECT_ROOT}/.nbs/pids/${h}.pid"
    if [[ -f "$pidfile" ]]; then
        oldpid=$(cat "$pidfile" 2>/dev/null)
        if [[ -n "$oldpid" && "$oldpid" =~ ^[0-9]+$ ]]; then
            # Wait up to 5s for each process to exit
            for i in $(seq 1 10); do
                kill -0 "$oldpid" 2>/dev/null || break
                sleep 0.5
            done
            # Force kill if still alive
            kill -9 "$oldpid" 2>/dev/null || true
        fi
    fi
done

# Belt and braces: pkill any nbs-claude and nbs-sidecar processes
# for this project that escaped pidfile tracking.
pkill -9 -f "nbs-claude.*--root=${PROJECT_ROOT}" 2>/dev/null || true
pkill -9 -f "nbs-claude.*${PROJECT_ROOT}.*dangerously" 2>/dev/null || true
pkill -9 -f "nbs-sidecar.*--root=${PROJECT_ROOT}" 2>/dev/null || true

# Wait for all kills to finish before proceeding
sleep 2

# Verify nothing survived
SURVIVORS=$(pgrep -f "nbs-claude.*${PROJECT_ROOT}" 2>/dev/null | wc -l || true)
if [[ "$SURVIVORS" -gt 0 ]]; then
    echo "[watchdog] Warning: $SURVIVORS processes survived cleanup" >&2
    pgrep -f "nbs-claude.*${PROJECT_ROOT}" 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 1
fi

rm -f "${PROJECT_ROOT}/.nbs/pids/"*.pid 2>/dev/null || true
rm -f "${PROJECT_ROOT}/.nbs/sessions/"*.honest 2>/dev/null || true
rm -f "${PROJECT_ROOT}/.nbs/sessions/"*.json 2>/dev/null || true  # transitional: clean pre-migration files
rm -f "${PROJECT_ROOT}/.nbs/control-pause" 2>/dev/null || true
# Reset trigger timestamps so librarian/pythia/shepard/fixup use their
# first_delay timing (e.g. librarian fires after 5 min, not 15).
rm -f "${PROJECT_ROOT}/.nbs/librarian-last-run" \
      "${PROJECT_ROOT}/.nbs/pythia-last-run" \
      "${PROJECT_ROOT}/.nbs/shepard-last-run" \
      "${PROJECT_ROOT}/.nbs/fixup-last-run" 2>/dev/null || true

sleep 1

# 2. Run digest (preserves institutional memory across restarts)
# Skip for empty/new chats — nothing to digest, and the digest worker
# would hang waiting for a completion signal that never comes.
CHAT_LINES=$(wc -l < "$CHAT_FILE" 2>/dev/null || echo 0)
DIGEST_OK=false
if [[ "$CHAT_LINES" -le 10 ]]; then
    echo "[watchdog] New chat ($CHAT_LINES lines) — skipping digest" >&2
elif [[ -x "$NBS_DIGEST" ]]; then
    if bash "$NBS_DIGEST" "$CHAT_FILE" >/dev/null 2>&1; then
        DIGEST_OK=true
    else
        echo "[watchdog] Warning: digest failed, continuing without it" >&2
    fi
else
    echo "[watchdog] Warning: nbs-digest-spawn not found, skipping digest" >&2
fi

# 2b. Ensure bus events directory and registry entries exist
CHAT_ABS=$(cd "$(dirname "$CHAT_FILE")" && pwd)/$(basename "$CHAT_FILE")
EVENTS_DIR="${PROJECT_ROOT}/.nbs/events"
mkdir -p "${EVENTS_DIR}/processed"
for handle in scribe medic supervisor gatekeeper theologian testkeeper generalist; do
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

# 3. Reset cursors to current end (lock-safe via nbs-chat cursor-set, msg_count-1 so agents see last message)
NBS_CHAT="${BIN_DIR}/nbs-chat"
MESSAGE_COUNT=$("$NBS_CHAT" count "$CHAT_FILE" 2>/dev/null || echo 0)
if [ "$MESSAGE_COUNT" -gt 0 ]; then
    RESET_TO=$((MESSAGE_COUNT - 1))
else
    RESET_TO=0
fi
for handle in scribe medic supervisor gatekeeper theologian testkeeper generalist; do
    "$NBS_CHAT" cursor-set "$CHAT_FILE" "$handle" "$RESET_TO" 2>/dev/null
done

# 4. Spawn agents (scribe first, 5s stagger)
# nbs-claude handles its own nbs-ts session creation internally when
# NBS_TRANSPORT=ts. Do NOT wrap it in another nbs-ts create — that
# produces double-nesting where the outer session is tail -f and
# skill injection via nbs-ts send hits tail instead of claude.
# Role prompts are read from skill files and passed as plain text via
# NBS_INITIAL_PROMPT. The sidecar injects this directly into Claude's
# input — no slash command expansion needed.
declare -A AGENT_SKILL_FILES
AGENT_SKILL_FILES[scribe]="nbs-scribe.md"
AGENT_SKILL_FILES[medic]="nbs-medic.md"
AGENT_SKILL_FILES[gatekeeper]="nbs-gatekeeper.md"
AGENT_SKILL_FILES[testkeeper]="nbs-testkeeper.md"
AGENT_SKILL_FILES[theologian]="nbs-theologian.md"
AGENT_SKILL_FILES[generalist]="nbs-teams-chat.md"
AGENT_SKILL_FILES[supervisor]="nbs-supervisor.md"

# Resolve commands directory
COMMANDS_DIR=""
if [[ -d "${PROJECT_ROOT}/.nbs/commands" ]]; then
    COMMANDS_DIR="${PROJECT_ROOT}/.nbs/commands"
elif [[ -d "${HOME}/.nbs/commands" ]]; then
    COMMANDS_DIR="${HOME}/.nbs/commands"
fi

for h in scribe medic supervisor gatekeeper theologian testkeeper generalist; do
    SKILL_CONTENT=""
    if [[ -n "$COMMANDS_DIR" && -f "${COMMANDS_DIR}/${AGENT_SKILL_FILES[$h]}" ]]; then
        SKILL_CONTENT=$(cat "${COMMANDS_DIR}/${AGENT_SKILL_FILES[$h]}")
    fi
    if [[ -z "$SKILL_CONTENT" ]]; then
        echo "[watchdog] Warning: skill file not found for $h, using fallback" >&2
        SKILL_CONTENT="You are the ${h}. Read the chat history and follow the team's direction."
    fi
    # Write skill to a file and pass a short prompt referencing it.
    # Avoids env var size limits from passing full skill content.
    SKILL_FILE="${PROJECT_ROOT}/.nbs/workers/${h}-skill.md"
    echo "$SKILL_CONTENT" > "$SKILL_FILE"
    # Launch via shared function — single source of truth for agent launch.
    source "${NBS_BIN}/nbs-launch-agent"
    launch_agent "$h" "$PROJECT_ROOT" "${NBS_BIN}/nbs-claude" \
        "Read ${SKILL_FILE} and follow the role instructions. Then read the chat history and begin work."
    echo "[watchdog] Spawned $h (pid $!)"
    sleep 5
done

# No canned restart message — the supervisor reads the chat history
# and figures out context herself. Canned messages caused confusion
# (agents searching for nonexistent digests, proposing goals when
# a clear goal already exists in the chat).

echo "[watchdog] Team restarted successfully"

# 5. Team health check after agent initialisation
# 30s wait gives agents time to start their nbs-ts sessions and sidecars.
NBS_TEAM_CHECK=""
if [[ -x "${NBS_BIN}/nbs-team-check" ]]; then
    NBS_TEAM_CHECK="${NBS_BIN}/nbs-team-check"
fi
if [[ -n "$NBS_TEAM_CHECK" ]]; then
    sleep 30
    "$NBS_TEAM_CHECK" "$CHAT_TAG" "$PROJECT_ROOT" || true
fi
