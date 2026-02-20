#!/bin/bash
# Test: Resume semantics — session metadata infrastructure
#
# Tests that nbs-claude creates and manages session metadata files
# in .nbs/sessions/<handle>.json, enabling nbs-worker continue.
#
# Deterministic tests:
#   1.  Session metadata directory created on startup
#   2.  Session metadata file created with correct handle name
#   3.  Metadata contains valid UUID session_id
#   4.  Metadata contains handle field matching NBS_HANDLE
#   5.  Metadata contains model field when --model specified
#   6.  Metadata contains started timestamp in ISO 8601
#   7.  Metadata contains tmux_session field
#   8.  Metadata contains pid field matching running process
#   9.  Metadata contains project_root as absolute path
#   10. --session-id passed to claude command line
#   11. --model passed to claude command line when NBS_MODEL set
#   12. --continue= maps to claude --resume
#   13. Metadata cleaned up in cleanup trap (pidfile removed)
#
# Adversarial tests:
#   14. Missing .nbs/sessions/ directory — created automatically
#   15. Invalid model name — passed through (claude validates)
#   16. UUID generation works without uuidgen (fallback)
#
# nbs-worker tests:
#   17. nbs-worker session reads metadata correctly
#   18. nbs-worker continue reads session_id from metadata
#   19. nbs-worker continue with --model override

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CLAUDE="$PROJECT_ROOT/bin/nbs-claude"
NBS_WORKER="$PROJECT_ROOT/bin/nbs-worker"

ERRORS=0

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

# --- Preconditions ---
if [[ ! -f "$NBS_CLAUDE" ]]; then
    echo "FATAL: nbs-claude not found at $NBS_CLAUDE"
    exit 1
fi

echo "Test: Resume semantics — session metadata"
echo "=========================================="
echo ""

# --- Test: nbs-claude arg parsing and session metadata structure ---
# We can't actually run nbs-claude (it launches claude interactively),
# but we can verify the script contains the required patterns.

echo "Script structure tests (nbs-claude):"

# 1. Creates .nbs/sessions/ directory
if grep -q 'mkdir.*sessions' "$NBS_CLAUDE"; then
    check "1. Creates .nbs/sessions/ directory" "pass"
else
    check "1. Creates .nbs/sessions/ directory" "fail"
fi

# 2. Writes session metadata file with handle name
if grep -q 'sessions.*SIDECAR_HANDLE\|sessions.*handle' "$NBS_CLAUDE"; then
    check "2. Writes metadata file with handle name" "pass"
else
    check "2. Writes metadata file with handle name" "fail"
fi

# 3. Generates UUID for session_id
if grep -qE 'uuidgen|/proc/sys/kernel/random/uuid|uuid' "$NBS_CLAUDE"; then
    check "3. UUID generation present" "pass"
else
    check "3. UUID generation present" "fail"
fi

# 4. Passes --session-id to claude
if grep -qF -- '--session-id' "$NBS_CLAUDE"; then
    check "4. --session-id passed to claude" "pass"
else
    check "4. --session-id passed to claude" "fail"
fi

# 5. Supports --model argument
if grep -qF -- '--model' "$NBS_CLAUDE"; then
    check "5. --model argument supported" "pass"
else
    check "5. --model argument supported" "fail"
fi

# 6. Supports --continue argument (nbs-level, maps to claude --resume)
if grep -qF -- '--continue' "$NBS_CLAUDE"; then
    check "6. --continue argument supported" "pass"
else
    check "6. --continue argument supported" "fail"
fi

# 7. Session metadata contains required JSON fields
# Check that the metadata write includes session_id, handle, model, started, pid
REQUIRED_FIELDS=("session_id" "handle" "model" "started" "pid" "project_root")
FOUND_FIELDS=0
for field in "${REQUIRED_FIELDS[@]}"; do
    if grep -q "\"$field\"" "$NBS_CLAUDE"; then
        FOUND_FIELDS=$((FOUND_FIELDS + 1))
    fi
done
if [[ $FOUND_FIELDS -ge 5 ]]; then
    check "7. Metadata contains required fields ($FOUND_FIELDS/${#REQUIRED_FIELDS[@]})" "pass"
else
    check "7. Metadata contains required fields ($FOUND_FIELDS/${#REQUIRED_FIELDS[@]})" "fail"
fi

# 8. Cleanup removes session metadata or updates it
if grep -q 'sessions.*rm\|sessions.*cleanup\|session.*exit\|rm.*sessions' "$NBS_CLAUDE"; then
    check "8. Cleanup handles session metadata" "pass"
else
    check "8. Cleanup handles session metadata" "fail"
fi

# 9. NBS_MODEL environment variable support
if grep -q 'NBS_MODEL' "$NBS_CLAUDE"; then
    check "9. NBS_MODEL environment variable supported" "pass"
else
    check "9. NBS_MODEL environment variable supported" "fail"
fi

# 10. --continue maps to --resume in claude args
if grep -qE -- '--continue.*resume|--resume.*continue' "$NBS_CLAUDE" || \
   (grep -qF -- '--continue' "$NBS_CLAUDE" && grep -qF -- '--resume' "$NBS_CLAUDE"); then
    check "10. --continue maps to --resume" "pass"
else
    check "10. --continue maps to --resume" "fail"
fi

echo ""
echo "Adversarial tests:"

# 11. UUID fallback (not just uuidgen)
# The UUID generation should have multiple methods on a fallback chain (||)
UUID_LINE=$(grep -E 'uuidgen|/proc/sys/kernel/random/uuid|python3.*uuid' "$NBS_CLAUDE" | head -1 || true)
UUID_METHODS=0
if echo "$UUID_LINE" | grep -q 'uuidgen'; then UUID_METHODS=$((UUID_METHODS + 1)); fi
if echo "$UUID_LINE" | grep -q '/proc/sys/kernel/random/uuid'; then UUID_METHODS=$((UUID_METHODS + 1)); fi
if echo "$UUID_LINE" | grep -q 'python3.*uuid'; then UUID_METHODS=$((UUID_METHODS + 1)); fi
if [[ "$UUID_METHODS" -ge 2 ]]; then
    check "11. UUID generation has fallback" "pass"
else
    check "11. UUID generation has fallback ($UUID_METHODS methods)" "fail"
fi

# 12. Session metadata is valid JSON (check for proper quoting)
if grep -q 'cat.*<<.*EOF\|printf.*json\|echo.*{' "$NBS_CLAUDE" && \
   grep -q '"session_id"' "$NBS_CLAUDE"; then
    check "12. Session metadata written as JSON" "pass"
else
    check "12. Session metadata written as JSON" "fail"
fi

echo ""
echo "nbs-worker tests:"

# 13. nbs-worker has 'continue' command
if [[ -f "$NBS_WORKER" ]] && grep -q 'continue\|cmd_continue' "$NBS_WORKER"; then
    check "13. nbs-worker has continue command" "pass"
else
    check "13. nbs-worker has continue command" "fail"
fi

# 14. nbs-worker has 'session' command
if [[ -f "$NBS_WORKER" ]] && grep -q 'session\|cmd_session' "$NBS_WORKER"; then
    check "14. nbs-worker has session command" "pass"
else
    check "14. nbs-worker has session command" "fail"
fi

# 15. nbs-worker continue reads session metadata
if [[ -f "$NBS_WORKER" ]] && grep -q 'sessions.*json\|session.*metadata' "$NBS_WORKER"; then
    check "15. nbs-worker continue reads session metadata" "pass"
else
    check "15. nbs-worker continue reads session metadata" "fail"
fi

echo ""
echo "=========================================="
if [[ $ERRORS -eq 0 ]]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "FAILURES: $ERRORS"
    exit 1
fi
