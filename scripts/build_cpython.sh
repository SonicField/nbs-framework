#!/bin/bash
# CPython Build Script for CinderX Development
# Builds CPython with --enable-shared and --export-dynamic so that
# CinderX's _cinderx.so can resolve internal CPython symbols at runtime.
#
# Usage: bash build_cpython.sh
#
# Prerequisites:
#   - CPython 3.12.x source tree at CPYTHON_SRC
#   - Build tools (gcc/clang, make)
#
# Created: 2026-03-02 from two sessions of build archaeology.
# The key flags are:
#   --enable-shared    : creates libpython3.12.so (required by _cinderx.so)
#   --export-dynamic   : exports symbols from the executable's dynamic table
#   -fvisibility=default : overrides CPython's -fvisibility=hidden on internal
#                          headers, ensuring _Py* symbols are exported from .so
#   --without-ensurepip: skips pip install, saves ~2 minutes

set -euo pipefail

CPYTHON_SRC="${CPYTHON_SRC:-$HOME/local/cinderx_dev/python-3.12}"
INSTALL_PREFIX="${INSTALL_PREFIX:-$HOME/local/cinderx_dev/python-install}"

echo "=== CPython Build Script for CinderX ==="
echo "Time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "Source: $CPYTHON_SRC"
echo "Install: $INSTALL_PREFIX"
echo "Arch: $(uname -m)"

# Verify source tree exists
if [ ! -f "$CPYTHON_SRC/configure" ]; then
    echo "ERROR: CPython source not found at $CPYTHON_SRC"
    echo "Expected to find 'configure' script"
    exit 1
fi

cd "$CPYTHON_SRC"

echo ""
echo "=== Step 1: Clean ==="
make clean 2>/dev/null || true

echo ""
echo "=== Step 2: Configure ==="
# Note: CFLAGS at configure time is overridden by CPython's CONFIGURE_CFLAGS_NODIST
# which appends -fvisibility=hidden AFTER user CFLAGS. We pass -fvisibility=default
# via CFLAGS_NODIST at make time instead (Step 3), which comes last in PY_CFLAGS_NODIST.
LDFLAGS='-Wl,--export-dynamic' ./configure \
    --enable-shared \
    --prefix="$INSTALL_PREFIX" \
    --without-ensurepip

echo ""
echo "=== Step 3: Build ==="
# CFLAGS_NODIST overrides CPython's -fvisibility=hidden (last flag wins)
make CFLAGS_NODIST='-fvisibility=default' -j8

echo ""
echo "=== Step 4: Install ==="
make install

echo ""
echo "=== Step 5: Verify ==="
# Check shared library exists
if [ -f "$INSTALL_PREFIX/lib/libpython3.12.so.1.0" ]; then
    echo "PASS: libpython3.12.so.1.0 exists"
else
    echo "FAIL: libpython3.12.so.1.0 not found"
    exit 1
fi

# Check _PyUnion_Type is exported
if nm -D "$INSTALL_PREFIX/lib/libpython3.12.so.1.0" 2>/dev/null | grep -q '_PyUnion_Type'; then
    echo "PASS: _PyUnion_Type is exported"
else
    echo "FAIL: _PyUnion_Type is NOT exported — --export-dynamic may not have worked"
    exit 1
fi

# Check python binary works
"$INSTALL_PREFIX/bin/python3.12" --version

echo ""
echo "=== CPython build complete ==="
echo "Python: $INSTALL_PREFIX/bin/python3.12"
echo "Shared lib: $INSTALL_PREFIX/lib/libpython3.12.so.1.0"
echo ""
echo "Next: run build_cinderx.sh to build CinderX against this Python."
echo "Remember: set LD_LIBRARY_PATH=$INSTALL_PREFIX/lib when running."
