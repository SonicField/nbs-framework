#!/bin/bash
# nbs-sidecar-lib.sh — Shared function library for sidecar lifecycle management.
#
# Source this file from any script that manages sidecars.
# All sidecar operations exist here and ONLY here.
#
# Usage:
#   source "$(dirname "$0")/nbs-sidecar-lib.sh"

# Guard against double-sourcing
[[ -n "${_NBS_SIDECAR_LIB_LOADED:-}" ]] && return 0
_NBS_SIDECAR_LIB_LOADED=1

# Resolve the directory containing this library
_NBS_SC_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Process Discovery ---

# Check if a PID's cmdline contains a flag value.
# Converts null bytes to newlines for reliable matching.
# Returns: 0 if found, 1 if not.
nbs_sc_cmdline_has() {
    local pid="$1"
    local flag="$2"
    [[ -r "/proc/$pid/cmdline" ]] || return 1
    tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | grep -qF -- "$flag"
}

# Find sidecar PIDs for a given session+root.
# Writes matching PIDs to stdout, one per line.
# Uses pgrep -x (exact process name match) to avoid self-matching.
nbs_sc_find_pids() {
    local session="$1"
    local root="$2"
    local pid
    while IFS= read -r pid; do
        [[ -r "/proc/$pid/cmdline" ]] || continue
        nbs_sc_cmdline_has "$pid" "--session=$session" || continue
        nbs_sc_cmdline_has "$pid" "--root=$root" || continue
        echo "$pid"
    done < <(pgrep -x nbs-sidecar 2>/dev/null || true)
}

# Count sidecars for a session+root.
nbs_sc_count() {
    local session="$1"
    local root="$2"
    nbs_sc_find_pids "$session" "$root" | wc -l
}

# Check if a sidecar PID has a healthy sidecar-loop parent.
# Returns: 0 if healthy loop exists, 1 if orphaned.
nbs_sc_has_loop() {
    local pid="$1"
    local ppid
    ppid=$(ps -p "$pid" -o ppid= 2>/dev/null | tr -d ' ') || return 1
    [[ -n "$ppid" && "$ppid" != "1" ]] || return 1
    local pcomm
    pcomm=$(ps -p "$ppid" -o comm= 2>/dev/null) || return 1
    [[ "$pcomm" == "bash" ]] || return 1
    [[ -r "/proc/$ppid/cmdline" ]] || return 1
    tr '\0' '\n' < "/proc/$ppid/cmdline" 2>/dev/null | grep -qF 'sidecar-loop'
}

# --- Handle and Session Extraction ---

# Extract agent handle from nbs-ts session name.
# Handles are single words — stops at first hyphen after "nbs-".
# Example: nbs-supervisor-phoenix → supervisor
nbs_sc_extract_handle() {
    local name="$1"
    if [[ "$name" =~ ^nbs-([a-zA-Z0-9_]+)- ]]; then
        echo "${BASH_REMATCH[1]}"
    fi
}

# Find alive nbs-ts session by exact name.
# Writes session ID to stdout. Returns 1 if not found.
nbs_sc_find_session() {
    local session_name="$1"
    local nbs_ts
    nbs_ts=$(command -v nbs-ts 2>/dev/null || echo "${_NBS_SC_LIB_DIR}/nbs-ts")
    "$nbs_ts" list 2>/dev/null | awk -v n="$session_name" '$2=="alive" && $3==n {print $1; exit}'
}

# --- Infrastructure Handle Detection ---

# Returns 0 if handle is an ephemeral oracle (skip in sidecar management).
nbs_sc_is_infrastructure() {
    local handle="$1"
    case "$handle" in
        pythia*|shepard*|fixup*|chatdigest*|librarian*) return 0 ;;
        *) return 1 ;;
    esac
}

# --- Lock File Management ---
#
# Sidecar-loop lock files prevent duplicate loops. The lock lives at
# <root>/.nbs/locks/sidecar-<handle>.lock and contains the loop's PID.
# One writer (the loop), many readers (watchdog, dashboard, respawn).

# Acquire a sidecar-loop lock. Returns 0 if acquired, 1 if held by another.
nbs_sc_lock_acquire() {
    local handle="$1"
    local root="$2"
    local lockdir="${root}/.nbs/locks"
    local lockfile="${lockdir}/sidecar-${handle}.lock"

    mkdir -p "$lockdir" 2>/dev/null

    if [[ -f "$lockfile" ]]; then
        local held_pid
        held_pid=$(cat "$lockfile" 2>/dev/null)
        if [[ -n "$held_pid" && "$held_pid" == "$$" ]]; then
            # Re-entrant — we already hold it
            return 0
        fi
        if [[ -n "$held_pid" ]] && kill -0 "$held_pid" 2>/dev/null; then
            # Held by a live process
            return 1
        fi
        # Stale — take over
    fi

    echo "$$" > "$lockfile"
    return 0
}

# Release a sidecar-loop lock.
nbs_sc_lock_release() {
    local handle="$1"
    local root="$2"
    rm -f "${root}/.nbs/locks/sidecar-${handle}.lock" 2>/dev/null
}

# Check if a sidecar-loop lock is held (without acquiring).
# Returns 0 if held by a live process, 1 if not held.
nbs_sc_lock_check() {
    local handle="$1"
    local root="$2"
    local lockfile="${root}/.nbs/locks/sidecar-${handle}.lock"

    [[ -f "$lockfile" ]] || return 1
    local held_pid
    held_pid=$(cat "$lockfile" 2>/dev/null)
    [[ -n "$held_pid" ]] && kill -0 "$held_pid" 2>/dev/null
}

# --- PID File Management ---

# Remove stale sidecar PID file.
nbs_sc_clean_pid() {
    local handle="$1"
    local root="$2"
    rm -f "${root}/.nbs/pids/sidecar-${handle}.pid" 2>/dev/null || true
}

# --- Loop Script Generation ---

# Generate a sidecar loop script.
# Builds the script line-by-line using arrays — no heredoc.
# The generated script:
#   - Runs the sidecar with the given args
#   - On crash: checks session liveness, restarts if alive
#   - On session death: searches for replacement by exact session name
#   - Logs all events to <root>/.nbs/sidecar-loop-<handle>.log
#   - Self-deletes via trap on any exit
#
# Writes script path to stdout.
nbs_sc_generate_loop() {
    local sidecar_bin="" handle="" root="" session="" session_name="" log_path="" prompt=""

    local arg
    for arg in "$@"; do
        case "$arg" in
            --sidecar-bin=*)    sidecar_bin="${arg#*=}" ;;
            --handle=*)         handle="${arg#*=}" ;;
            --root=*)           root="${arg#*=}" ;;
            --session=*)        session="${arg#*=}" ;;
            --session-name=*)   session_name="${arg#*=}" ;;
            --log=*)            log_path="${arg#*=}" ;;
            --initial-prompt=*) prompt="${arg#*=}" ;;
        esac
    done

    # Validate required args
    if [[ -z "$sidecar_bin" || -z "$handle" || -z "$root" || -z "$session" ]]; then
        echo "nbs_sc_generate_loop: missing required args" >&2
        return 1
    fi

    local script_file
    script_file=$(mktemp /tmp/nbs-sidecar-loop.XXXXXX.sh)

    # Build the sidecar command as properly quoted elements
    local -a sidecar_cmd=(
        "$sidecar_bin"
        "--handle=$handle"
        "--root=$root"
        "--transport=ts"
    )
    [[ -n "$log_path" ]] && sidecar_cmd+=("--log=$log_path")
    [[ -n "$prompt" ]] && sidecar_cmd+=("--initial-prompt=$prompt")

    # Quote each element for the generated script
    # --session= is NOT included here — added separately to avoid printf %q escaping $
    local sidecar_cmd_str=""
    local elem
    for elem in "${sidecar_cmd[@]}"; do
        sidecar_cmd_str+="$(printf '%q ' "$elem")"
    done

    local nbs_ts_bin
    nbs_ts_bin=$(command -v nbs-ts 2>/dev/null || echo "${_NBS_SC_LIB_DIR}/nbs-ts")
    local find_session_bin
    find_session_bin=$(command -v nbs-sidecar-find-session 2>/dev/null || echo "${_NBS_SC_LIB_DIR}/nbs-sidecar-find-session")

    local logfile="${root}/.nbs/sidecar-loop-${handle}.log"

    # Generate the script line by line
    local -a lines=()
    lines+=('#!/bin/bash')
    lines+=("CURRENT_SESSION=$(printf '%q' "$session")")
    lines+=("LOGFILE=$(printf '%q' "$logfile")")
    lines+=('log() { echo "$(date -u "+%Y-%m-%dT%H:%M:%SZ") $*" >> "$LOGFILE"; }')
    local lockdir="${root}/.nbs/locks"
    local lockfile="${lockdir}/sidecar-${handle}.lock"

    lines+=("LOCKFILE=$(printf '%q' "$lockfile")")
    lines+=("mkdir -p $(printf '%q' "$lockdir") 2>/dev/null")
    lines+=('# Acquire lock — exit if another loop holds it')
    lines+=('if [[ -f "$LOCKFILE" ]]; then')
    lines+=('    held_pid=$(cat "$LOCKFILE" 2>/dev/null)')
    lines+=('    if [[ -n "$held_pid" && "$held_pid" != "$$" ]] && kill -0 "$held_pid" 2>/dev/null; then')
    lines+=("        echo \"sidecar-loop for ${handle}: lock held by PID \$held_pid — exiting\" >&2")
    lines+=('        exit 0')
    lines+=('    fi')
    lines+=('fi')
    lines+=('echo $$ > "$LOCKFILE"')
    lines+=("log \"sidecar-loop started for ${handle} session=\$CURRENT_SESSION pid=\$\$\"")
    lines+=("trap 'log \"sidecar-loop exiting for ${handle}\"; rm -f $(printf '%q' "$script_file") \"\$LOCKFILE\"' EXIT")
    lines+=('while true; do')
    lines+=('    log "starting sidecar session=$CURRENT_SESSION"')
    lines+=("    ${sidecar_cmd_str}--session=\"\$CURRENT_SESSION\"")
    lines+=('    rc=$?')
    lines+=('    log "sidecar exited rc=$rc session=$CURRENT_SESSION"')
    lines+=("    if $(printf '%q' "$nbs_ts_bin") status \"\$CURRENT_SESSION\" 2>/dev/null | grep -q \"alive\"; then")
    lines+=('        log "session $CURRENT_SESSION alive, restarting in 5s"')
    lines+=('        sleep 5')
    lines+=('        continue')
    lines+=('    fi')
    lines+=('    log "session $CURRENT_SESSION dead, searching for replacement"')
    lines+=("    NEW_SESSION=\$($(printf '%q' "$find_session_bin") $(printf '%q' "$session_name"))")
    lines+=('    if [[ -n "$NEW_SESSION" ]]; then')
    lines+=('        log "found replacement session $NEW_SESSION"')
    lines+=('        CURRENT_SESSION="$NEW_SESSION"')
    lines+=('        sleep 5')
    lines+=('        continue')
    lines+=('    fi')
    lines+=('    log "no replacement session — exiting"')
    lines+=('    break')
    lines+=('done')

    # Write the script
    printf '%s\n' "${lines[@]}" > "$script_file"
    chmod +x "$script_file"

    echo "$script_file"
}

# Launch a sidecar with a loop.
# Generates script, cleans PID file, launches via setsid.
# Writes PID to stdout.
nbs_sc_spawn() {
    local handle="" root=""
    local arg
    for arg in "$@"; do
        case "$arg" in
            --handle=*) handle="${arg#*=}" ;;
            --root=*)   root="${arg#*=}" ;;
        esac
    done

    # Check lock — don't spawn if a loop already exists
    if [[ -n "$handle" && -n "$root" ]] && nbs_sc_lock_check "$handle" "$root"; then
        echo "0"  # signal: not spawned, already running
        return 1
    fi

    # Clean stale PID marker before spawning
    [[ -n "$handle" && -n "$root" ]] && nbs_sc_clean_pid "$handle" "$root"

    local script
    script=$(nbs_sc_generate_loop "$@") || return 1

    setsid bash "$script" </dev/null >/dev/null 2>&1 &
    local pid=$!
    disown "$pid" 2>/dev/null || true

    echo "$pid"
}
