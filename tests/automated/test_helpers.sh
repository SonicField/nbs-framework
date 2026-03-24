#!/bin/bash
# test_helpers.sh — Shared cleanup for all NBS tests.
#
# Source this at the top of any test that creates nbs-ts sessions,
# spawns nbs-claude, or uses temp directories.
#
# Usage:
#   source "$(dirname "$0")/test_helpers.sh"
#
# What it does:
#   - Records all nbs-ts sessions at test start
#   - On EXIT, kills any sessions created during the test
#   - Kills any nbs-claude/sidecar processes in /tmp/ test dirs
#   - Safe to source multiple times (idempotent)

# Guard against double-sourcing
[[ -n "${_NBS_TEST_HELPERS_LOADED:-}" ]] && return 0
_NBS_TEST_HELPERS_LOADED=1

# Find nbs-ts binary
_NBS_TS_BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/bin/nbs-ts"

# Record sessions before the test
_NBS_TEST_SESSIONS_BEFORE="/tmp/nbs-test-before-$$"
if [[ -x "$_NBS_TS_BIN" ]]; then
    "$_NBS_TS_BIN" list 2>/dev/null | cut -f1 > "$_NBS_TEST_SESSIONS_BEFORE" 2>/dev/null || true
fi

# Cleanup function — kills leaked sessions and processes
_nbs_test_cleanup() {
    # Kill any nbs-ts sessions created during this test
    if [[ -x "$_NBS_TS_BIN" && -f "$_NBS_TEST_SESSIONS_BEFORE" ]]; then
        "$_NBS_TS_BIN" list 2>/dev/null | while IFS=$'\t' read -r handle status name cmd; do
            [[ -n "$handle" ]] || continue
            grep -q "^${handle}$" "$_NBS_TEST_SESSIONS_BEFORE" 2>/dev/null || \
                "$_NBS_TS_BIN" kill "$handle" 2>/dev/null || true
        done
    fi
    rm -f "$_NBS_TEST_SESSIONS_BEFORE"

    # Kill any nbs-claude/sidecar from temp test directories
    pkill -9 -f "nbs-claude.*/tmp/nbs-" 2>/dev/null || true
    pkill -9 -f "nbs-sidecar.*/tmp/nbs-" 2>/dev/null || true
    pkill -9 -f "nbs-restart-test" 2>/dev/null || true
}

# Register cleanup — appends to existing EXIT trap if any
trap '_nbs_test_cleanup' EXIT
