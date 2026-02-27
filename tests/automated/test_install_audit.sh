#!/bin/bash
# Test: Install.sh Audit Violation Fixes
#
# Adversarial tests for each violation found in the audit report.
# These tests run install.sh in an isolated temp HOME to avoid
# polluting the real HOME directory.
#
# Violations tested:
#   V1  (HARDENING) Dead code in rm -rf guard
#   V2  (BUG)       make install failure silently continues
#   V3  (HARDENING) Zero-template glob produces silent empty install
#   V4  (HARDENING) Zero-command symlink glob produces silent empty install
#   V5  (BUG)       process_template lacks precondition guards
#   V6  (HARDENING) 2>/dev/null suppresses diagnostics on PREFIX resolution
#   V7  (SECURITY)  Overly broad grep marker for PATH dedup
#   V8  (HARDENING) No postcondition after writing PATH_LINE to rc file
#   V9  (HARDENING) No postcondition after ln -s
#   V10 (HARDENING) No ERR trap for diagnostic on failure
#
# Falsification: Each test defines what would prove the fix is absent
# or broken. Exit 0 only if all tests pass.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
INSTALL_SCRIPT="$PROJECT_ROOT/bin/install.sh"

# Save real HOME
REAL_HOME="$HOME"

# Use a results file so subshell pass/fail counts propagate
RESULTS_FILE=$(mktemp)
echo "0 0" > "$RESULTS_FILE"

pass() {
    echo "  PASS: $1"
    local counts
    counts=$(cat "$RESULTS_FILE")
    local p f
    p=$(echo "$counts" | awk '{print $1}')
    f=$(echo "$counts" | awk '{print $2}')
    echo "$((p + 1)) $f" > "$RESULTS_FILE"
}

fail() {
    echo "  FAIL: $1"
    local counts
    counts=$(cat "$RESULTS_FILE")
    local p f
    p=$(echo "$counts" | awk '{print $1}')
    f=$(echo "$counts" | awk '{print $2}')
    echo "$p $((f + 1))" > "$RESULTS_FILE"
}

# Create isolated test environment
setup_test_home() {
    local test_home
    test_home=$(mktemp -d)
    echo "$test_home"
}

# Cleanup a test home
cleanup_test_home() {
    local test_home="$1"
    if [[ -d "$test_home" ]] && [[ "$test_home" == /tmp/* ]]; then
        rm -rf "$test_home"
    fi
}

echo "=== Install.sh Audit Violation Tests ==="
echo "Project root: $PROJECT_ROOT"
echo ""

# ============================================================
# V1: HARDENING - Dead code in rm -rf guard (lines 151-155)
#
# The original code has:
#   if [[ -L "$target" || -d "$target" ]]; then rm -rf "$target"
#   elif [[ -e "$target" ]]; then rm -rf "$target"
# Both branches do the same thing. The fix collapses them.
# Test: Place a regular file (not symlink, not directory) at a
# target path and verify install.sh handles it correctly.
# ============================================================
echo "--- V1: Dead code in rm -rf guard ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"
PREFIX="$HOME/.nbs"
mkdir -p "$PREFIX"

# Plant a regular file where a symlink should go
echo "I am a regular file, not a symlink" > "$PREFIX/concepts"

# Run install
if echo 'n' | "$INSTALL_SCRIPT" >/dev/null 2>&1; then
    # Verify the regular file was replaced with a symlink
    if [[ -L "$PREFIX/concepts" ]]; then
        pass "V1: Regular file replaced with symlink correctly"
    else
        fail "V1: Regular file was NOT replaced with symlink"
    fi
else
    fail "V1: Install failed when regular file present at target"
fi
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V2: BUG - make install failure silently continues
#
# The original code downgrades build failure to WARNING and
# reports "Installation complete" even with no binaries.
# Test: Verify that when make fails, the final message does NOT
# say "Installation complete" unqualified — it must say "partial".
# ============================================================
echo "--- V2: make install failure reporting ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"

# Run install normally
OUTPUT=$(echo 'n' | "$INSTALL_SCRIPT" 2>&1 || true)

# Check if build failed (look for WARNING in output)
if echo "$OUTPUT" | grep -q "WARNING.*Build failed"; then
    # Build failed — the final message MUST say "partial", not "Installation complete"
    if echo "$OUTPUT" | grep -q "partially complete"; then
        pass "V2: Failed build reports partial installation"
    elif echo "$OUTPUT" | grep -q "Installation complete\\."; then
        fail "V2: Failed build falsely says 'Installation complete'"
    else
        pass "V2: Failed build does not claim full completion"
    fi
else
    # Build succeeded — "Installation complete" is correct
    if echo "$OUTPUT" | grep -q "Installation complete"; then
        pass "V2: Successful install says 'Installation complete'"
    else
        fail "V2: Successful install missing completion message"
    fi
fi
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V3: HARDENING - Zero templates processed, no postcondition
#
# If claude_tools/ has no .md files, the template loop silently
# processes nothing. The fix adds a postcondition check.
# Test: Create a fake claude_tools/ with no .md files and verify
# install.sh fails with a diagnostic.
# ============================================================
echo "--- V3: Zero templates postcondition ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"

# Create a fake project layout with empty claude_tools/
FAKE_PROJECT=$(mktemp -d)
mkdir -p "$FAKE_PROJECT/claude_tools"
mkdir -p "$FAKE_PROJECT/bin"

# Copy install.sh to the fake project
cp "$INSTALL_SCRIPT" "$FAKE_PROJECT/bin/install.sh"
chmod +x "$FAKE_PROJECT/bin/install.sh"

# Create a minimal Makefile that does nothing
printf 'install:\n\t@true\n' > "$FAKE_PROJECT/Makefile"

# Create source directories for symlinks
for dir in concepts docs templates bin terminal-weathering; do
    mkdir -p "$FAKE_PROJECT/$dir"
done

# Put a non-.md file in claude_tools to ensure the directory exists
echo "not a template" > "$FAKE_PROJECT/claude_tools/README.txt"

# Run install -- should fail because no templates processed
OUTPUT=$("$FAKE_PROJECT/bin/install.sh" --prefix="$TEST_HOME/.nbs" 2>&1 || true)
EXIT_CODE=0
"$FAKE_PROJECT/bin/install.sh" --prefix="$TEST_HOME/.nbs" >/dev/null 2>&1 || EXIT_CODE=$?

if [[ $EXIT_CODE -ne 0 ]]; then
    if echo "$OUTPUT" | grep -qi "no.*template\|zero.*template\|ASSERTION FAILED"; then
        pass "V3: Zero templates causes failure with diagnostic"
    else
        pass "V3: Zero templates causes failure (exit code $EXIT_CODE)"
    fi
else
    fail "V3: Zero templates did NOT cause failure -- silent empty install"
fi

rm -rf "$FAKE_PROJECT"
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V4: HARDENING - Zero commands symlinked, no postcondition
#
# Similar to V3 but for the command symlinking loop.
# Test: Verify that a successful install produces at least one
# command symlink.
# ============================================================
echo "--- V4: Zero commands postcondition ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"
echo 'n' | "$INSTALL_SCRIPT" >/dev/null 2>&1 || true

# Count command symlinks
CMD_COUNT=0
if [[ -d "$HOME/.claude/commands" ]]; then
    CMD_COUNT=$(find "$HOME/.claude/commands" -name "*.md" -type l 2>/dev/null | wc -l)
fi

if [[ $CMD_COUNT -gt 0 ]]; then
    pass "V4: $CMD_COUNT command symlinks created"
else
    fail "V4: Zero command symlinks created"
fi
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V5: BUG - process_template lacks precondition guards
#
# If called with a non-existent template or empty nbs_root,
# it should fail with a clear assertion message.
# Test: Check that the source has assertion guards in the function.
# ============================================================
echo "--- V5: process_template precondition guards ---"

# Test 5a: Non-existent template file guard
if grep -q 'ASSERTION FAILED.*[Tt]emplate' "$INSTALL_SCRIPT"; then
    pass "V5a: process_template has template file precondition"
else
    fail "V5a: process_template lacks template file precondition guard"
fi

# Test 5b: Empty nbs_root guard
if grep -q 'ASSERTION FAILED.*nbs_root' "$INSTALL_SCRIPT"; then
    pass "V5b: process_template has nbs_root precondition"
else
    fail "V5b: process_template lacks nbs_root precondition guard"
fi

echo ""

# ============================================================
# V6: HARDENING - 2>/dev/null suppresses diagnostics
#
# The PREFIX resolution line redirects stderr to /dev/null.
# This suppresses useful error messages.
# Test: Verify that a bad prefix parent shows a diagnostic
# error to the user (not swallowed).
# ============================================================
echo "--- V6: PREFIX resolution diagnostics ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"

# Use a prefix whose parent does not exist
OUTPUT=$("$INSTALL_SCRIPT" --prefix="/nonexistent_parent_dir_$$/.nbs" 2>&1 || true)

if echo "$OUTPUT" | grep -qi "ERROR.*parent.*does not exist\|ERROR.*directory"; then
    pass "V6: Bad prefix parent shows clear error"
else
    fail "V6: Bad prefix parent error not clear. Output: $OUTPUT"
fi
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V7: SECURITY - Overly broad grep marker for PATH dedup
#
# The marker "# NBS Framework" is too broad. A commented-out
# or unrelated line containing "# NBS Framework" would prevent
# the PATH line from being added.
# Test: Create an rc file with a stale "# NBS Framework" comment
# (without PATH) and verify install.sh still adds the PATH line.
# ============================================================
echo "--- V7: PATH dedup marker specificity ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"
export SHELL=/bin/bash

# Create .bashrc with a stale/commented NBS Framework reference
cat > "$HOME/.bashrc" << 'RCEOF'
# Old config
# NBS Framework was here before but removed
export SOME_OTHER_VAR=1
RCEOF

# Run install, answering 'y' to PATH prompt
echo 'y' | "$INSTALL_SCRIPT" >/dev/null 2>&1 || true

# The PATH line should have been added despite the old comment
if grep -q 'export PATH=.*\.nbs/bin' "$HOME/.bashrc"; then
    pass "V7: PATH line added despite stale NBS Framework comment"
else
    fail "V7: PATH line NOT added -- stale comment caused false positive"
fi

# Also verify the source uses the specific marker
if grep -q '# NBS Framework PATH' "$INSTALL_SCRIPT"; then
    pass "V7: Source uses specific PATH marker"
else
    fail "V7: Source still uses overly broad marker"
fi
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V8: HARDENING - No postcondition after writing PATH to rc file
#
# After appending PATH_LINE, verify it is actually present.
# Test: Make rc file read-only, answer 'y' to PATH prompt,
# verify the script handles the write failure gracefully.
# ============================================================
echo "--- V8: PATH write postcondition ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"
export SHELL=/bin/bash

# Run install first to create everything
echo 'n' | "$INSTALL_SCRIPT" >/dev/null 2>&1 || true

# Create a .bashrc and make it read-only
echo "# existing config" > "$HOME/.bashrc"
chmod 444 "$HOME/.bashrc"

# Run install again, answering 'y' to PATH prompt
OUTPUT=$(echo 'y' | "$INSTALL_SCRIPT" 2>&1 || true)

# The script should either fail or report the write failure.
# Under set -e, the echo >> will fail and trigger ERR trap.
if echo "$OUTPUT" | grep -qi "ASSERTION FAILED\|failed to write\|Permission denied\|cannot write\|failed at line"; then
    pass "V8: Read-only rc file detected and reported"
else
    # Check if the line was actually written (it shouldn't be)
    if grep -q 'NBS' "$HOME/.bashrc" 2>/dev/null; then
        fail "V8: Somehow wrote to read-only file (unexpected)"
    else
        pass "V8: Write to read-only rc file failed (set -e caught it)"
    fi
fi

# Restore permissions for cleanup
chmod 644 "$HOME/.bashrc" 2>/dev/null || true
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

echo ""

# ============================================================
# V9: HARDENING - No postcondition after ln -s
#
# After creating symlinks, verify they exist and are valid.
# Test: Run install normally and verify all expected symlinks
# exist and are not dangling.
# ============================================================
echo "--- V9: Symlink postcondition ---"
TEST_HOME=$(setup_test_home)
export HOME="$TEST_HOME"
echo 'n' | "$INSTALL_SCRIPT" >/dev/null 2>&1 || true

PREFIX="$HOME/.nbs"
ALL_OK=true

for dir in concepts docs templates bin; do
    target="$PREFIX/$dir"
    if [[ ! -L "$target" ]]; then
        fail "V9: $dir is not a symlink"
        ALL_OK=false
    elif [[ ! -e "$target" ]]; then
        fail "V9: $dir symlink is dangling"
        ALL_OK=false
    fi
done

if $ALL_OK; then
    pass "V9: All symlinks exist and are valid"
fi
export HOME="$REAL_HOME"
cleanup_test_home "$TEST_HOME"

# Also verify the source code has postcondition assertions
if grep -q 'ASSERTION FAILED.*[Ss]ymlink' "$INSTALL_SCRIPT"; then
    pass "V9: Source has symlink postcondition assertion"
else
    fail "V9: Source lacks symlink postcondition assertion"
fi

echo ""

# ============================================================
# V10: HARDENING - No ERR trap for diagnostic on failure
#
# The script should have a trap that reports the line number
# on failure.
# Test: Check that the script contains an ERR trap.
# ============================================================
echo "--- V10: ERR trap for diagnostics ---"
if grep -q "trap.*ERR" "$INSTALL_SCRIPT"; then
    pass "V10: ERR trap present in install.sh"
else
    fail "V10: No ERR trap found in install.sh"
fi

# Verify the trap includes LINENO for diagnostic value
if grep -q 'LINENO.*ERR\|ERR.*LINENO' "$INSTALL_SCRIPT"; then
    pass "V10: ERR trap includes line number"
else
    # Check if LINENO appears on same line or nearby
    if grep -q 'LINENO' "$INSTALL_SCRIPT" && grep -q "trap.*ERR" "$INSTALL_SCRIPT"; then
        pass "V10: ERR trap and LINENO both present"
    else
        fail "V10: ERR trap lacks line number information"
    fi
fi

echo ""

# ============================================================
# Summary
# ============================================================
COUNTS=$(cat "$RESULTS_FILE")
TESTS_PASSED=$(echo "$COUNTS" | awk '{print $1}')
TESTS_FAILED=$(echo "$COUNTS" | awk '{print $2}')
rm -f "$RESULTS_FILE"

echo "=== Summary ==="
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"
echo ""

if [[ $TESTS_FAILED -gt 0 ]]; then
    echo "=== TESTS FAILED ==="
    exit 1
else
    echo "=== ALL TESTS PASSED ==="
    exit 0
fi
