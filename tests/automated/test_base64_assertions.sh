#!/bin/bash
# test_base64_assertions.sh — Automated test for base64 encode/decode
#
# Tests:
#   1. Builds the C unit test program (both optimised and ASan)
#   2. Runs the unit tests and checks all pass
#   3. Verifies that invalid input is rejected (non-base64 chars)
#   4. Verifies that large inputs round-trip correctly
#
# Exit code: 0 on success, 1 on any failure

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC_DIR="$PROJECT_ROOT/src/nbs-chat"
TEST_DIR="$PROJECT_ROOT/tests"
TEST_SRC="$TEST_DIR/test_base64_unit.c"
TEST_BIN="$TEST_DIR/test_base64_unit"
TEST_BIN_ASAN="$TEST_DIR/test_base64_unit_asan"

PASS=0
FAIL=0

pass() {
    PASS=$((PASS + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    echo "  FAIL: $1" >&2
}

echo "=== base64 assertion tests ==="
echo ""

# --- Step 1: Build the unit test (optimised) ---
echo "Building unit test (optimised)..."
if gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
    -I "$SRC_DIR" -o "$TEST_BIN" \
    "$TEST_SRC" "$SRC_DIR/base64.c" "$SRC_DIR/chat_file.c" "$SRC_DIR/lock.c" 2>&1; then
    pass "optimised build"
else
    fail "optimised build"
    echo "Cannot continue without a successful build" >&2
    exit 1
fi

# --- Step 2: Build the unit test (ASan + UBSan) ---
echo "Building unit test (ASan + UBSan)..."
if clang -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$SRC_DIR" -o "$TEST_BIN_ASAN" \
    "$TEST_SRC" "$SRC_DIR/base64.c" "$SRC_DIR/chat_file.c" "$SRC_DIR/lock.c" \
    -fsanitize=address,undefined 2>&1; then
    pass "ASan + UBSan build"
else
    fail "ASan + UBSan build"
    echo "Continuing with optimised build only" >&2
fi

# --- Step 3: Run the unit tests (optimised) ---
echo ""
echo "Running unit tests (optimised)..."
if "$TEST_BIN" 2>/dev/null; then
    pass "unit tests (optimised)"
else
    fail "unit tests (optimised)"
fi

# --- Step 4: Run the unit tests (ASan + UBSan) ---
if [ -f "$TEST_BIN_ASAN" ]; then
    echo ""
    echo "Running unit tests (ASan + UBSan)..."
    if "$TEST_BIN_ASAN" 2>/dev/null; then
        pass "unit tests (ASan + UBSan)"
    else
        fail "unit tests (ASan + UBSan)"
    fi
fi

# --- Step 5: Verify the full project still builds ---
echo ""
echo "Verifying full project build..."
if (cd "$SRC_DIR" && make clean && make) >/dev/null 2>&1; then
    pass "full project build (optimised)"
else
    fail "full project build (optimised)"
fi

echo ""
echo "Verifying full project build (ASan)..."
if (cd "$SRC_DIR" && make asan) >/dev/null 2>&1; then
    pass "full project build (ASan)"
else
    fail "full project build (ASan)"
fi

# --- Cleanup ---
rm -f "$TEST_BIN" "$TEST_BIN_ASAN"

# --- Summary ---
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
