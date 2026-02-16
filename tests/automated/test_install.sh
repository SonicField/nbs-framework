#!/bin/bash
# Test: Verify install.sh creates correct symlinks
#
# With templating, the structure is:
#   ~/.claude/commands/*.md -> ~/.nbs/commands/*.md (processed templates)
#   ~/.nbs/commands/*.md (content with {{NBS_ROOT}} expanded)
#   ~/.nbs/{concepts,docs,templates,bin} -> repo directories
#
# Env isolation: Uses a temp HOME so the test does not depend on or
# modify the real HOME directory.  This prevents failures caused by
# stale/broken symlinks (e.g. pointing to cleaned /tmp directories).
#
# Falsification: Exits 0 if all symlinks correct, non-zero otherwise

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

# --- Env isolation: temp HOME ---
REAL_HOME="$HOME"
TEST_HOME=$(mktemp -d)
cleanup() {
    export HOME="$REAL_HOME"
    rm -rf "$TEST_HOME"
}
trap cleanup EXIT

export HOME="$TEST_HOME"

NBS_ROOT="$HOME/.nbs"
CLAUDE_COMMANDS_DIR="$HOME/.claude/commands"

# Run install.sh in the isolated HOME (answer 'n' to PATH prompt)
echo "Running install.sh in isolated HOME ($TEST_HOME)..."
echo 'n' | "$PROJECT_ROOT/bin/install.sh"
echo ""

# Colours for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No colour

FAILED=0

check_claude_symlink() {
    local name="$1"
    local expected_target="$NBS_ROOT/commands/$name"
    local actual_link="$CLAUDE_COMMANDS_DIR/$name"

    if [[ ! -L "$actual_link" ]]; then
        echo -e "${RED}FAIL${NC}: $actual_link is not a symlink"
        FAILED=1
        return
    fi

    local actual_target
    actual_target=$(readlink "$actual_link")

    # Should point to ~/.nbs/commands/
    if [[ "$actual_target" != "$expected_target" ]]; then
        echo -e "${RED}FAIL${NC}: $actual_link points to $actual_target, expected $expected_target"
        FAILED=1
        return
    fi

    # The target should exist and be a file
    if [[ ! -f "$actual_target" ]]; then
        echo -e "${RED}FAIL${NC}: $actual_link -> $actual_target but target doesn't exist"
        FAILED=1
        return
    fi

    echo -e "${GREEN}PASS${NC}: $name symlink correct"
}

check_nbs_symlink() {
    local name="$1"
    local expected_target="$PROJECT_ROOT/$name"
    local actual_link="$NBS_ROOT/$name"

    if [[ ! -L "$actual_link" ]]; then
        echo -e "${RED}FAIL${NC}: $actual_link is not a symlink"
        FAILED=1
        return
    fi

    local actual_target
    actual_target=$(readlink "$actual_link")

    if [[ "$actual_target" != "$expected_target" ]]; then
        echo -e "${RED}FAIL${NC}: $actual_link points to $actual_target, expected $expected_target"
        FAILED=1
        return
    fi

    echo -e "${GREEN}PASS${NC}: ~/.nbs/$name symlink correct"
}

echo "Testing install.sh symlinks..."
echo "Project root: $PROJECT_ROOT"
echo "NBS root: $NBS_ROOT"
echo "Claude commands dir: $CLAUDE_COMMANDS_DIR"
echo ""

# Check ~/.claude/commands symlinks point to ~/.nbs/commands
echo "=== Claude Commands Symlinks ==="
for tool in "$PROJECT_ROOT/claude_tools"/*.md; do
    if [[ -f "$tool" ]]; then
        name=$(basename "$tool")
        check_claude_symlink "$name"
    fi
done

echo ""

# Check ~/.nbs supporting directory symlinks
echo "=== NBS Supporting Directory Symlinks ==="
for dir in concepts docs templates bin; do
    check_nbs_symlink "$dir"
done

echo ""
if [[ $FAILED -eq 0 ]]; then
    echo -e "${GREEN}All symlinks correct${NC}"
    exit 0
else
    echo -e "${RED}Some symlinks incorrect - install.sh produced bad symlinks${NC}"
    exit 1
fi
