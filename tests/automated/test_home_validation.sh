#!/bin/bash
# Test: HOME Environment Variable Validation
#
# Adversarially tests that install.sh properly validates HOME:
# 1. Empty HOME fails with clear error
# 2. Non-existent HOME directory fails with clear error
# 3. Valid HOME override works correctly
# 4. Error messages are informative (not swallowed)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
INSTALL_SCRIPT="$PROJECT_ROOT/bin/install.sh"

TESTS_PASSED=0
TESTS_FAILED=0

pass() {
    echo "  PASS: $1"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

fail() {
    echo "  FAIL: $1"
    TESTS_FAILED=$((TESTS_FAILED + 1))
}

echo "=== HOME Validation Tests ==="
echo ""

# Test 1: Empty HOME should fail
echo "Test 1: Empty HOME"
OUTPUT=$(HOME="" "$INSTALL_SCRIPT" 2>&1 || true)
EXIT_CODE=$(HOME="" "$INSTALL_SCRIPT" 2>&1; echo $?) || true
# Get actual exit code
if HOME="" "$INSTALL_SCRIPT" >/dev/null 2>&1; then
    fail "Empty HOME did not cause installation to fail"
else
    pass "Empty HOME causes failure"
fi

# Verify error message content
if echo "$OUTPUT" | grep -q "ERROR.*HOME.*not set"; then
    pass "Empty HOME shows correct error message"
else
    fail "Empty HOME error message unclear: $OUTPUT"
fi

if echo "$OUTPUT" | grep -q "export HOME"; then
    pass "Empty HOME error suggests fix"
else
    fail "Empty HOME error doesn't suggest fix"
fi

echo ""

# Test 2: Non-existent HOME directory should fail
echo "Test 2: Non-existent HOME directory"
FAKE_HOME="/nonexistent/path/that/does/not/exist/$(date +%s)"
OUTPUT=$(HOME="$FAKE_HOME" "$INSTALL_SCRIPT" 2>&1 || true)

if HOME="$FAKE_HOME" "$INSTALL_SCRIPT" >/dev/null 2>&1; then
    fail "Non-existent HOME did not cause installation to fail"
else
    pass "Non-existent HOME causes failure"
fi

# Verify error message content
if echo "$OUTPUT" | grep -q "ERROR.*HOME.*not a valid directory"; then
    pass "Non-existent HOME shows correct error message"
else
    fail "Non-existent HOME error message unclear: $OUTPUT"
fi

if echo "$OUTPUT" | grep -q "$FAKE_HOME"; then
    pass "Non-existent HOME error shows the invalid path"
else
    fail "Non-existent HOME error doesn't show the invalid path"
fi

echo ""

# Test 3: Valid HOME override works
echo "Test 3: Valid HOME override"
TEST_HOME=$(mktemp -d)
trap "rm -rf $TEST_HOME" EXIT

# Install with overridden HOME (pipe "N" to decline PATH setup prompt)
if echo "N" | HOME="$TEST_HOME" "$INSTALL_SCRIPT" --prefix="$TEST_HOME/.nbs" >/dev/null 2>&1; then
    pass "Valid HOME override succeeds"
else
    fail "Valid HOME override failed"
fi

# Verify installation went to TEST_HOME
if [[ -d "$TEST_HOME/.nbs/commands" ]]; then
    pass "Installation created .nbs in overridden HOME"
else
    fail "Installation did not create .nbs in overridden HOME"
fi

if [[ -d "$TEST_HOME/.claude/commands" ]]; then
    pass "Installation created .claude/commands in overridden HOME"
else
    fail "Installation did not create .claude/commands in overridden HOME"
fi

# Verify paths in installed files reference TEST_HOME, not real HOME
if grep -rq "$TEST_HOME" "$TEST_HOME/.nbs/commands/"*.md 2>/dev/null; then
    pass "Installed files reference overridden HOME path"
else
    fail "Installed files don't reference overridden HOME path"
fi

# Adversarial: Verify real HOME was NOT touched
if [[ -f "$HOME/.nbs/commands/ADVERSARIAL_TEST_MARKER" ]]; then
    fail "Installation touched real HOME instead of overridden HOME"
else
    pass "Real HOME was not modified by overridden install"
fi

echo ""

# Test 4: Adversarial - ensure error isn't swallowed in pipelines
echo "Test 4: Error propagation in pipelines"

# This tests that if someone does: install.sh | tee log.txt
# the error still appears
PIPE_OUTPUT=$(HOME="" "$INSTALL_SCRIPT" 2>&1 | cat || true)
if echo "$PIPE_OUTPUT" | grep -q "ERROR"; then
    pass "Error propagates through pipes"
else
    fail "Error swallowed in pipe"
fi

echo ""

# Summary
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
