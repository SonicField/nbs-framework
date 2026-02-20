#!/bin/bash
# Test: pty-session --last=N alias for --scrollback=N
# Verifies that --last=N works as an alias and produces the same output

set -euo pipefail

PTY="${HOME}/.nbs/bin/pty-session"
TEST_SESSION="test-last-alias-$$"

cleanup() {
    "$PTY" kill "$TEST_SESSION" 2>/dev/null || true
}
trap cleanup EXIT

echo "Test: pty-session --last=N alias"

# Create a test session
"$PTY" create "$TEST_SESSION" 'bash'
sleep 1

# Send some numbered lines
for i in $(seq 1 20); do
    "$PTY" send "$TEST_SESSION" "echo line_$i"
    sleep 0.1
done
sleep 1

# Test 1: --last=5 should return output (not error)
output_last=$("$PTY" read "$TEST_SESSION" --last=5 2>&1) || {
    echo "FAIL: --last=5 returned error"
    exit 1
}
if [[ -z "$output_last" ]]; then
    echo "FAIL: --last=5 returned empty output"
    exit 1
fi
echo "  PASS: --last=5 returns output"

# Test 2: --scrollback=5 should return equivalent output
output_scroll=$("$PTY" read "$TEST_SESSION" --scrollback=5 2>&1) || {
    echo "FAIL: --scrollback=5 returned error"
    exit 1
}
if [[ -z "$output_scroll" ]]; then
    echo "FAIL: --scrollback=5 returned empty output"
    exit 1
fi
echo "  PASS: --scrollback=5 returns output"

# Test 3: Both should have the same number of lines
lines_last=$(echo "$output_last" | wc -l)
lines_scroll=$(echo "$output_scroll" | wc -l)
if [[ "$lines_last" -ne "$lines_scroll" ]]; then
    echo "FAIL: --last=5 ($lines_last lines) != --scrollback=5 ($lines_scroll lines)"
    exit 1
fi
echo "  PASS: --last=N and --scrollback=N produce same line count ($lines_last)"

# Test 4: --last=100 (default equivalent) should return more lines
output_full=$("$PTY" read "$TEST_SESSION" --last=100 2>&1) || {
    echo "FAIL: --last=100 returned error"
    exit 1
}
lines_full=$(echo "$output_full" | wc -l)
if [[ "$lines_full" -le "$lines_last" ]]; then
    echo "FAIL: --last=100 ($lines_full lines) should be > --last=5 ($lines_last lines)"
    exit 1
fi
echo "  PASS: --last=100 ($lines_full lines) > --last=5 ($lines_last lines)"

echo "All tests passed."
