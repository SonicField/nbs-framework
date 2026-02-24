#!/bin/bash
# NBS Framework - Installation Script
#
# Usage: ./bin/install.sh [--prefix=PATH]
# Default prefix: ~/.nbs
#
# Creates:
#   PREFIX/commands/     - Processed command files (templates expanded)
#   PREFIX/concepts/     - Symlink to repo concepts/
#   PREFIX/docs/         - Symlink to repo docs/
#   PREFIX/templates/    - Symlink to repo templates/
#   PREFIX/bin/          - Symlink to repo bin/
#   ~/.claude/commands/* - Symlinks to PREFIX/commands/*
#
# Optionally adds PREFIX/bin to PATH via shell rc file (y/N prompt).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Validate HOME environment variable
validate_home() {
    if [[ -z "${HOME:-}" ]]; then
        echo "ERROR: HOME environment variable is not set."
        echo ""
        echo "The NBS Framework installs to \$HOME/.nbs by default and creates"
        echo "symlinks in \$HOME/.claude/commands/ for Claude Code integration."
        echo ""
        echo "To fix this, either:"
        echo "  1. Set HOME to a valid directory: export HOME=/path/to/home"
        echo "  2. Use explicit paths: $0 --prefix=/path/to/install"
        echo ""
        echo "Note: Even with --prefix, HOME must be set for Claude Code symlinks."
        return 1
    fi

    if [[ ! -d "$HOME" ]]; then
        echo "ERROR: HOME ($HOME) is not a valid directory."
        echo ""
        echo "The NBS Framework requires HOME to point to an existing directory"
        echo "because it creates symlinks in \$HOME/.claude/commands/."
        echo ""
        echo "To fix this, either:"
        echo "  1. Create the directory: mkdir -p \"$HOME\""
        echo "  2. Set HOME to a valid directory: export HOME=/path/to/home"
        return 1
    fi

    if [[ ! -w "$HOME" ]]; then
        echo "ERROR: HOME ($HOME) is not writable."
        echo ""
        echo "The NBS Framework needs to create \$HOME/.claude/commands/."
        echo "Please check permissions on $HOME."
        return 1
    fi

    return 0
}

# Parse arguments
PREFIX=""
EXPLICIT_PREFIX=false
for arg in "$@"; do
    case $arg in
        --prefix=*)
            PREFIX="${arg#--prefix=}"
            EXPLICIT_PREFIX=true
            ;;
        --help|-h)
            echo "Usage: $0 [--prefix=PATH]"
            echo "Default prefix: ~/.nbs"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [--prefix=PATH]"
            exit 1
            ;;
    esac
done

# Validate HOME (always needed for ~/.claude/commands)
if ! validate_home; then
    exit 1
fi

# Set default prefix if not explicitly provided
if [[ -z "$PREFIX" ]]; then
    PREFIX="$HOME/.nbs"
fi

# Resolve to absolute path
PREFIX="$(cd "$(dirname "$PREFIX")" 2>/dev/null && pwd)/$(basename "$PREFIX")" || PREFIX="$PREFIX"

CLAUDE_COMMANDS_DIR="$HOME/.claude/commands"

# Template processing function
process_template() {
    local template="$1"
    local output="$2"
    local nbs_root="$3"

    while IFS= read -r line || [[ -n "$line" ]]; do
        echo "${line//\{\{NBS_ROOT\}\}/$nbs_root}"
    done < "$template" > "$output"
}

echo "Installing NBS Framework..."
echo "  Prefix: $PREFIX"
echo "  Source: $PROJECT_ROOT"

# 1. Create prefix directory structure
mkdir -p "$PREFIX/commands"

# 2. Process command templates
echo "Processing command templates..."
for template in "$PROJECT_ROOT/claude_tools"/*.md; do
    if [[ -f "$template" ]]; then
        name=$(basename "$template")
        output="$PREFIX/commands/$name"
        process_template "$template" "$output" "$PREFIX"
        echo "  Processed: $name"
    fi
done

# 3. Symlink supporting directories
echo "Creating symlinks to supporting directories..."
for dir in concepts docs templates bin terminal-weathering; do
    target="$PREFIX/$dir"
    if [[ -e "$target" || -L "$target" ]]; then
        rm -rf "$target"
    fi
    ln -s "$PROJECT_ROOT/$dir" "$target"
    echo "  Linked: $dir/"
done

# 4. Create ~/.claude/commands symlinks
echo "Installing Claude Code commands..."
mkdir -p "$CLAUDE_COMMANDS_DIR"

# Remove stale skill names from prior installs
for stale in nbs-teams-supervisor.md nbs-teams-worker.md; do
    # Remove from processed templates
    stale_processed="$PREFIX/commands/$stale"
    if [[ -e "$stale_processed" ]]; then
        rm "$stale_processed"
        echo "  Removed stale template: $stale"
    fi
    # Remove from installed commands
    stale_path="$CLAUDE_COMMANDS_DIR/$stale"
    if [[ -e "$stale_path" || -L "$stale_path" ]]; then
        rm "$stale_path"
        echo "  Removed stale command: /$stale"
    fi
done

for cmd in "$PREFIX/commands"/*.md; do
    if [[ -f "$cmd" ]]; then
        name=$(basename "$cmd")
        target="$CLAUDE_COMMANDS_DIR/$name"

        if [[ -e "$target" || -L "$target" ]]; then
            rm "$target"
        fi

        ln -s "$cmd" "$target"
        echo "  Installed: /$name"
    fi
done

# 5. Offer to add bin/ to PATH
BIN_DIR="$PREFIX/bin"
PATH_LINE="export PATH=\"${BIN_DIR}:\$PATH\"  # NBS Framework"

offer_path_setup() {
    # Detect shell rc file
    local rc_file=""
    local shell_name
    shell_name=$(basename "${SHELL:-/bin/bash}")

    case "$shell_name" in
        bash)
            rc_file="$HOME/.bashrc"
            ;;
        zsh)
            rc_file="$HOME/.zshrc"
            ;;
        fish)
            echo "  Fish shell detected. Add manually:"
            echo "    fish_add_path ${BIN_DIR}"
            return 0
            ;;
        *)
            echo "  Unknown shell ($shell_name). Add manually:"
            echo "    export PATH=\"${BIN_DIR}:\$PATH\""
            return 0
            ;;
    esac

    # Check if already present
    if [[ -f "$rc_file" ]] && grep -qF "# NBS Framework" "$rc_file"; then
        echo "  PATH already configured in $rc_file"
        return 0
    fi

    # Prompt
    echo ""
    echo "Add NBS tools to your PATH?"
    echo "  This appends to $rc_file:"
    echo "    $PATH_LINE"
    echo ""
    read -rp "  Add to PATH? y/[N]: " answer

    case "$answer" in
        [Yy]|[Yy]es)
            echo "" >> "$rc_file"
            echo "$PATH_LINE" >> "$rc_file"
            echo "  Added to $rc_file"
            echo "  Run 'source $rc_file' or start a new shell to activate."
            ;;
        *)
            echo "  Skipped. You can add manually later:"
            echo "    echo '$PATH_LINE' >> $rc_file"
            ;;
    esac
}

offer_path_setup

echo ""
echo "Installation complete."
echo "  Framework root: $PREFIX"
echo "  Commands: $CLAUDE_COMMANDS_DIR"
echo "  Binaries: $BIN_DIR"
echo ""
echo "Restart Claude Code to pick up new commands."
echo "  Run /nbs-init in any project to configure its CLAUDE.md for NBS engineering standards."
