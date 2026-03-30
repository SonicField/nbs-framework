#!/bin/bash
# NBS Framework - Installation Script
#
# Usage: ./bin/install.sh [--prefix=PATH]
# Default prefix: ~/.nbs
#
# Requires: gcc, make (builds C binaries from source)
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

# V8.10: ERR trap for diagnostic on failure — reports line number
trap 'echo "ERROR: install.sh failed at line $LINENO" >&2' ERR

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
    case "$arg" in
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

# V7.1: Fail explicitly if PREFIX parent directory does not exist
# V8.6: Removed 2>/dev/null — let diagnostics reach the user
PREFIX_DIR="$(cd "$(dirname "$PREFIX")" && pwd)" || {
    echo "ERROR: Parent directory of PREFIX does not exist: $(dirname "$PREFIX")" >&2
    exit 1
}
PREFIX="${PREFIX_DIR}/$(basename "$PREFIX")"

CLAUDE_COMMANDS_DIR="$HOME/.claude/commands"

# Template processing function
process_template() {
    local template="$1"
    local output="$2"
    local nbs_root="$3"

    # V8.5: Precondition guards
    [[ -f "$template" ]] || {
        echo "ASSERTION FAILED: Template file not found: $template" >&2
        exit 1
    }
    [[ -n "$nbs_root" ]] || {
        echo "ASSERTION FAILED: nbs_root is empty — cannot expand templates" >&2
        exit 1
    }

    while IFS= read -r line || [[ -n "$line" ]]; do
        echo "${line//\{\{NBS_ROOT\}\}/$nbs_root}"
    done < "$template" > "$output"
}

echo "Installing NBS Framework..."
echo "  Prefix: $PREFIX"
echo "  Source: $PROJECT_ROOT"

# 1. Create prefix directory structure
mkdir -p "$PREFIX/commands"
mkdir -p "$PREFIX/chat"
mkdir -p "$PREFIX/events/processed"
mkdir -p "$PREFIX/scribe"
mkdir -p "$PREFIX/workers"
mkdir -p "$PREFIX/pids"
mkdir -p "$PREFIX/sessions"
mkdir -p "$PREFIX/honest"

# 2. Process command templates
# V7.2: Check claude_tools directory exists before globbing
if [[ ! -d "$PROJECT_ROOT/claude_tools" ]]; then
    echo "ERROR: claude_tools directory not found at $PROJECT_ROOT/claude_tools" >&2
    exit 1
fi
echo "Processing command templates..."
TEMPLATES_PROCESSED=0
for template in "$PROJECT_ROOT/claude_tools"/*.md; do
    if [[ -f "$template" ]]; then
        name=$(basename "$template")
        output="$PREFIX/commands/$name"
        process_template "$template" "$output" "$PREFIX"
        echo "  Processed: $name"
        TEMPLATES_PROCESSED=$((TEMPLATES_PROCESSED + 1))
    fi
done
# V8.3: Postcondition — at least one template must have been processed
[[ $TEMPLATES_PROCESSED -gt 0 ]] || {
    echo "ASSERTION FAILED: No command templates found in $PROJECT_ROOT/claude_tools/" >&2
    echo "  Expected *.md files but found none. Installation cannot continue." >&2
    exit 1
}

# 3. Build honest library
echo "Building honest library..."
HONEST_BUILD_OK=false
if make -C "$PROJECT_ROOT/lib/honest" 2>&1; then
    echo "  honest library built"
    HONEST_BUILD_OK=true
    # Install session rulebook (not affected by bin/ symlink step)
    install -m 644 "$PROJECT_ROOT/honest/session.honest-rulebook" \
        "$PREFIX/honest/session.honest-rulebook"
    echo "  Installed: session.honest-rulebook"
else
    echo "WARNING: honest library build failed." >&2
    echo "  Session metadata will not use Honest format." >&2
fi

# 4. Build binaries from source (clean first to avoid stale object files)
# Install to PREFIX/bin via BIN_DIR override — avoids "Text file busy"
# when live binaries are in use by running agents.
echo "Building from source..."
make -C "$PROJECT_ROOT" clean 2>/dev/null || true
BUILD_FAILED=false
if ! make -C "$PROJECT_ROOT" install BIN_DIR="$PREFIX/bin" 2>&1; then
    BUILD_FAILED=true
    echo "WARNING: Build failed." >&2
    echo "  Skills and symlinks will still be updated." >&2
    echo "  Run 'make install BIN_DIR=$PREFIX/bin' manually to retry." >&2
fi

# 5. Symlink supporting directories
echo "Creating symlinks to supporting directories..."
for dir in concepts docs templates bin terminal-weathering; do
    target="$PREFIX/$dir"
    # V8.1: Collapsed dead code — single guard covers symlinks, directories, and regular files.
    # -L catches dangling symlinks (which -e misses); -e catches everything else.
    if [[ -L "$target" || -e "$target" ]]; then
        rm -rf "$target"
    fi
    # V7.4: Verify source directory exists before creating symlink
    if [[ ! -d "$PROJECT_ROOT/$dir" ]]; then
        echo "WARNING: Source directory not found, skipping symlink: $PROJECT_ROOT/$dir" >&2
        continue
    fi
    ln -s "$PROJECT_ROOT/$dir" "$target"
    # V8.9: Postcondition — verify symlink was created correctly
    [[ -L "$target" ]] || {
        echo "ASSERTION FAILED: Symlink creation failed for $target" >&2
        exit 1
    }
    echo "  Linked: $dir/"
done

# 5b. Install honest CLI tools (after symlink step — PREFIX/bin is now
# a symlink to PROJECT_ROOT/bin, so tools end up in the source tree's bin/)
if $HONEST_BUILD_OK; then
    for tool in honest-build honest-get; do
        if [[ -f "$PROJECT_ROOT/lib/honest/build/$tool" ]]; then
            rm -f "$PREFIX/bin/$tool"
            install "$PROJECT_ROOT/lib/honest/build/$tool" "$PREFIX/bin/$tool"
            echo "  Installed: $tool"
        fi
    done
fi

# 6. Create ~/.claude/commands symlinks
#
# CRITICAL: Symlinks ALWAYS point to ~/.nbs/commands/ (the global default
# prefix), never to a project-specific prefix. This ensures that deleting
# a project's .nbs/ directory doesn't break Claude Code skills globally.
#
# When --prefix is not ~/.nbs, we still process templates into the custom
# prefix (step 2), but ~/.claude/commands/ symlinks point to ~/.nbs/commands/.
# If ~/.nbs/commands/ doesn't have the templates yet, we copy them there.
echo "Installing Claude Code commands..."
mkdir -p "$CLAUDE_COMMANDS_DIR"

# Ensure ~/.nbs/commands/ has current templates (the stable global location)
GLOBAL_COMMANDS="$HOME/.nbs/commands"
if [[ "$PREFIX" != "$HOME/.nbs" ]]; then
    mkdir -p "$GLOBAL_COMMANDS"
    # Process templates into ~/.nbs/commands/ with ~/.nbs as NBS_ROOT
    for template in "$PROJECT_ROOT/claude_tools"/*.md; do
        if [[ -f "$template" ]]; then
            name=$(basename "$template")
            output="$GLOBAL_COMMANDS/$name"
            process_template "$template" "$output" "$HOME/.nbs"
        fi
    done
    echo "  Global commands updated: $GLOBAL_COMMANDS"
fi
# When PREFIX is ~/.nbs, step 2 already processed templates there.

# Remove stale skill names from prior installs
for stale in nbs-teams-supervisor.md nbs-teams-worker.md; do
    # Remove from processed templates (both prefix and global)
    for dir in "$PREFIX/commands" "$GLOBAL_COMMANDS"; do
        stale_processed="$dir/$stale"
        if [[ -e "$stale_processed" ]]; then
            rm "$stale_processed"
            echo "  Removed stale template: $stale (from $dir)"
        fi
    done
    # Remove from installed commands
    stale_path="$CLAUDE_COMMANDS_DIR/$stale"
    if [[ -e "$stale_path" || -L "$stale_path" ]]; then
        rm "$stale_path"
        echo "  Removed stale command: /$stale"
    fi
done

COMMANDS_INSTALLED=0
for cmd in "$GLOBAL_COMMANDS"/*.md; do
    if [[ -f "$cmd" ]]; then
        name=$(basename "$cmd")
        target="$CLAUDE_COMMANDS_DIR/$name"

        if [[ -e "$target" || -L "$target" ]]; then
            rm "$target"
        fi

        ln -s "$cmd" "$target"
        echo "  Installed: /$name"
        COMMANDS_INSTALLED=$((COMMANDS_INSTALLED + 1))
    fi
done
# V8.4: Postcondition — at least one command must have been installed
[[ $COMMANDS_INSTALLED -gt 0 ]] || {
    echo "ASSERTION FAILED: No commands installed to $CLAUDE_COMMANDS_DIR" >&2
    echo "  $GLOBAL_COMMANDS/ appears empty. Check template processing." >&2
    exit 1
}
# V9.1: Postcondition — all symlinks point to ~/.nbs/commands/, not a project prefix
for link in "$CLAUDE_COMMANDS_DIR"/nbs-*.md; do
    [[ -L "$link" ]] || continue
    link_target=$(readlink "$link")
    [[ "$link_target" == "$GLOBAL_COMMANDS/"* ]] || {
        echo "ASSERTION FAILED: $(basename "$link") points to $link_target, not $GLOBAL_COMMANDS/" >&2
        exit 1
    }
done

# 7. Offer to add bin/ to PATH
BIN_DIR="$PREFIX/bin"
PATH_LINE="export PATH=\"${BIN_DIR}:\$PATH\"  # NBS Framework PATH"

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

    # V8.7: Check for the specific PATH marker, not the generic "# NBS Framework"
    if [[ -f "$rc_file" ]] && grep -qF "# NBS Framework PATH" "$rc_file"; then
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
            # V8.8: Postcondition — verify the write succeeded
            grep -qF "# NBS Framework PATH" "$rc_file" || {
                echo "ASSERTION FAILED: Failed to write PATH to $rc_file" >&2
                exit 1
            }
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
# V8.2: Distinguish full vs partial installation in final message
if $BUILD_FAILED; then
    echo "Installation partially complete (build failed — binaries not updated)."
else
    echo "Installation complete."
fi
echo "  Framework root: $PREFIX"
echo "  Commands: $CLAUDE_COMMANDS_DIR"
echo "  Binaries: $BIN_DIR"
echo ""
echo "Restart Claude Code to pick up new commands."
echo "  Run /nbs-init in any project to configure its CLAUDE.md for NBS engineering standards."
