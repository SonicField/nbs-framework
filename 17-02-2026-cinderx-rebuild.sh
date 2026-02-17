#!/bin/bash
# CinderX ARM JIT — Rebuild Script
# Rebuilds CinderX with or without aarch64 JIT enabled
#
# Usage:
#   ./17-02-2026-cinderx-rebuild.sh [stub|jit]
#
# Modes:
#   stub  — default build (JIT disabled on aarch64)
#   jit   — apply all three patches, enable native aarch64 JIT
#
# Three patches required for aarch64 JIT:
#   1. detection.h: remove `#define CINDER_UNSUPPORTED` from aarch64 block
#   2. pyjit.cpp: remove `#ifndef __x86_64__ ... return 0;` in jit::initialize()
#   3. frame_asm.cpp: fix w1→w12 scratch register in closure prologue

set -euo pipefail

BUILD_MODE="${1:-stub}"
CINDERX_DEV="$HOME/local/cinderx_dev"
CINDERX_DIR="$CINDERX_DEV/cinderx"
VENV_DIR="$CINDERX_DEV/venv"
DETECTION_H="$CINDERX_DIR/cinderx/Jit/codegen/arch/detection.h"
PYJIT_CPP="$CINDERX_DIR/cinderx/Jit/pyjit.cpp"
FRAME_ASM_CPP="$CINDERX_DIR/cinderx/Jit/codegen/frame_asm.cpp"
LOG_DIR="$CINDERX_DEV/build-logs"
CC=/opt/llvm/stable/Toolchains/llvm-sand.xctoolchain/usr/bin/clang
CXX=/opt/llvm/stable/Toolchains/llvm-sand.xctoolchain/usr/bin/clang++

# Validate
if [ ! -d "$CINDERX_DIR" ]; then
    echo "ERROR: CinderX source not found at $CINDERX_DIR" >&2
    exit 1
fi

if [ ! -f "$DETECTION_H" ]; then
    echo "ERROR: detection.h not found at $DETECTION_H" >&2
    exit 1
fi

if [ ! -f "$PYJIT_CPP" ]; then
    echo "ERROR: pyjit.cpp not found at $PYJIT_CPP" >&2
    exit 1
fi

if [ ! -f "$FRAME_ASM_CPP" ]; then
    echo "ERROR: frame_asm.cpp not found at $FRAME_ASM_CPP" >&2
    exit 1
fi

if [ ! -f "$CC" ]; then
    echo "ERROR: Meta Clang not found at $CC" >&2
    exit 1
fi

if [ ! -f "$VENV_DIR/bin/activate" ]; then
    echo "ERROR: Venv not found at $VENV_DIR" >&2
    exit 1
fi

# Activate venv
source "$VENV_DIR/bin/activate"

mkdir -p "$LOG_DIR"
TIMESTAMP=$(date -u +%Y%m%dT%H%M%SZ)
BUILD_LOG="$LOG_DIR/cinderx_build_${BUILD_MODE}_${TIMESTAMP}.log"

echo "=== CinderX Rebuild: mode=$BUILD_MODE ==="
echo "Python: $(python3 --version 2>&1)"
echo "Architecture: $(uname -m)"
echo "Build log: $BUILD_LOG"

# Show current detection.h state
echo ""
echo "=== Current detection.h ==="
grep -n 'CINDER_' "$DETECTION_H" | head -20

case "$BUILD_MODE" in
    stub)
        echo ""
        echo "Mode: stub (JIT disabled on aarch64 — falls through to interpreter)"
        # Restore both files from git
        cd "$CINDERX_DIR"
        if ! grep -A1 'aarch64 support everywhere' "$DETECTION_H" | grep -q 'CINDER_UNSUPPORTED'; then
            echo "Restoring detection.h..."
            git checkout -- "$DETECTION_H" 2>/dev/null || \
                echo "WARNING: Could not restore detection.h from git."
        fi
        if ! grep -q '#ifndef __x86_64__' "$PYJIT_CPP"; then
            echo "Restoring pyjit.cpp..."
            git checkout -- "$PYJIT_CPP" 2>/dev/null || \
                echo "WARNING: Could not restore pyjit.cpp from git."
        fi
        if grep -q 'a64::w12, FRAME_OWNED_BY_THREAD' "$FRAME_ASM_CPP"; then
            echo "Restoring frame_asm.cpp..."
            git checkout -- "$FRAME_ASM_CPP" 2>/dev/null || \
                echo "WARNING: Could not restore frame_asm.cpp from git."
        fi
        ;;
    jit)
        echo ""
        echo "Mode: jit (enabling native aarch64 JIT — patching both guards)"

        # --- Guard 1: detection.h CINDER_UNSUPPORTED ---
        cp "$DETECTION_H" "${DETECTION_H}.bak"

        # Remove ONLY the CINDER_UNSUPPORTED in the aarch64 block (line after
        # the comment "until we have aarch64 support everywhere"), NOT the one
        # in the #else (unknown arch) block.
        if grep -A1 'aarch64 support everywhere' "$DETECTION_H" | grep -q 'CINDER_UNSUPPORTED'; then
            sed -i '/aarch64 support everywhere/{n;/CINDER_UNSUPPORTED/d;}' "$DETECTION_H"
            echo "Guard 1: Removed CINDER_UNSUPPORTED from aarch64 block in detection.h"
        else
            echo "Guard 1: CINDER_UNSUPPORTED already removed from detection.h"
        fi

        echo ""
        echo "=== Updated detection.h ==="
        grep -n 'CINDER_' "$DETECTION_H" | head -20

        # --- Guard 2: pyjit.cpp #ifndef __x86_64__ early return ---
        cp "$PYJIT_CPP" "${PYJIT_CPP}.bak"

        # Remove the #ifndef __x86_64__ block that prevents jit::initialize()
        # from creating the cinderjit module on non-x86 platforms.
        # The block spans from '#ifndef __x86_64__' to '#endif' and contains
        # a JIT_DLOG and 'return 0'.
        if grep -q '#ifndef __x86_64__' "$PYJIT_CPP"; then
            # Remove the 7-line block from '#ifndef __x86_64__' through '#endif'
            # that immediately follows the 'force_init' check in jit::initialize().
            # We identify the line number and delete that range.
            GUARD_LINE=$(grep -n '#ifndef __x86_64__' "$PYJIT_CPP" | head -1 | cut -d: -f1)
            if [ -n "$GUARD_LINE" ]; then
                # Verify context: the line before should be empty or a comment about config
                ENDIF_LINE=$((GUARD_LINE + 6))
                # Verify the #endif is where we expect
                ENDIF_CONTENT=$(sed -n "${ENDIF_LINE}p" "$PYJIT_CPP")
                if echo "$ENDIF_CONTENT" | grep -q '#endif'; then
                    sed -i "${GUARD_LINE},${ENDIF_LINE}d" "$PYJIT_CPP"
                    echo "Guard 2: Removed lines ${GUARD_LINE}-${ENDIF_LINE} (#ifndef __x86_64__ block) from pyjit.cpp"
                else
                    echo "ERROR: #endif not found at expected line $ENDIF_LINE (found: '$ENDIF_CONTENT')" >&2
                    echo "Manual removal needed. The block starts at line $GUARD_LINE." >&2
                    cp "${PYJIT_CPP}.bak" "$PYJIT_CPP"
                    exit 1
                fi
            fi
            # Verify removal
            if grep -q '#ifndef __x86_64__' "$PYJIT_CPP"; then
                echo "ERROR: Failed to remove #ifndef __x86_64__ guard from pyjit.cpp" >&2
                cp "${PYJIT_CPP}.bak" "$PYJIT_CPP"
                exit 1
            fi
            echo "Guard 2: Verified — no #ifndef __x86_64__ remaining in pyjit.cpp"
        else
            echo "Guard 2: #ifndef __x86_64__ already removed from pyjit.cpp"
        fi

        echo ""
        echo "=== pyjit.cpp around jit::initialize() ==="
        grep -n 'force_init\|__x86_64__\|cinderjit\|JIT only supported' "$PYJIT_CPP" | head -10

        # --- Guard 3: frame_asm.cpp w1 → w12 closure fix ---
        cp "$FRAME_ASM_CPP" "${FRAME_ASM_CPP}.bak"

        # Fix the register clobber bug: a64::w1 is used as scratch for
        # FRAME_OWNED_BY_THREAD, but x1 holds the live args pointer.
        # Change to a64::w12 (arch::reg_scratch_0, safe scratch register).
        if grep -q 'a64::w1, FRAME_OWNED_BY_THREAD' "$FRAME_ASM_CPP"; then
            # Replace w1 with w12 in both the mov and the strb
            sed -i 's/a64::w1, FRAME_OWNED_BY_THREAD/a64::w12, FRAME_OWNED_BY_THREAD/' "$FRAME_ASM_CPP"
            sed -i 's/strb(\s*$/strb(/' "$FRAME_ASM_CPP"  # no-op normalise
            sed -i '/strb(/{ n; s/a64::w1,/a64::w12,/; }' "$FRAME_ASM_CPP"
            echo "Guard 3: Fixed w1→w12 scratch register in frame_asm.cpp (closure fix)"
        else
            echo "Guard 3: w12 fix already applied in frame_asm.cpp"
        fi
        ;;
    *)
        echo "ERROR: Invalid mode '$BUILD_MODE'. Must be stub or jit." >&2
        exit 1
        ;;
esac

# Rebuild CinderX
echo ""
echo "=== Building CinderX ==="
cd "$CINDERX_DIR"
export CC CXX

python3 setup.py build_ext --inplace 2>&1 | tee "$BUILD_LOG"
BUILD_EXIT=$?

if [ $BUILD_EXIT -ne 0 ]; then
    echo "FAIL: CinderX build failed (exit $BUILD_EXIT)"
    echo "See: $BUILD_LOG"
    if [ "$BUILD_MODE" = "jit" ]; then
        echo ""
        echo "Restoring patched files from backup..."
        [ -f "${DETECTION_H}.bak" ] && cp "${DETECTION_H}.bak" "$DETECTION_H"
        [ -f "${PYJIT_CPP}.bak" ] && cp "${PYJIT_CPP}.bak" "$PYJIT_CPP"
        [ -f "${FRAME_ASM_CPP}.bak" ] && cp "${FRAME_ASM_CPP}.bak" "$FRAME_ASM_CPP"
        echo "Restored. The build failure may mean additional aarch64 code is needed."
    fi
    exit $BUILD_EXIT
fi

# Verify
echo ""
echo "=== Verifying build ==="
PYTHONPATH="$CINDERX_DIR/scratch/lib.linux-aarch64-cpython-312:${PYTHONPATH:-}" \
    python3 -c "
import cinderx
cinderx.init()
print(f'CinderX loaded: {cinderx.__file__}')
print(f'Supported runtime: {cinderx.is_supported_runtime()}')
cinderx.install_frame_evaluator()
print(f'Frame evaluator installed: {cinderx.is_frame_evaluator_installed()}')

# Verify cinderjit module exists (proves jit::initialize() ran fully)
try:
    import cinderjit
    print(f'cinderjit module: LOADED')
    print(f'JIT enabled: {cinderjit.is_enabled()}')
except ImportError:
    if '$BUILD_MODE' == 'jit':
        print('FAIL: cinderjit module not found — jit::initialize() did not complete')
        print('The #ifndef __x86_64__ guard in pyjit.cpp may not have been removed.')
        import sys
        sys.exit(1)
    else:
        print('cinderjit module: not available (expected in stub mode)')

# Quick JIT test
def add(a, b):
    return a + b

for _ in range(200):
    result = add(3, 4)
assert result == 7, f'Expected 7, got {result}'
print(f'Quick JIT test: add(3,4) = {result} — PASS')

# In JIT mode, verify function was actually compiled
if '$BUILD_MODE' == 'jit':
    import cinderjit
    compiled = cinderjit.is_jit_compiled(add)
    print(f'add() JIT compiled: {compiled}')
    if compiled:
        size = cinderjit.get_compiled_size(add)
        print(f'add() native code size: {size} bytes')
        all_compiled = cinderjit.get_compiled_functions()
        print(f'Total JIT-compiled functions: {len(all_compiled)}')
    else:
        print('WARNING: add() was not JIT compiled after 200 calls')
        print('JIT may have a high compilation threshold or be in a fallback state')
"
VERIFY_EXIT=$?

if [ $VERIFY_EXIT -ne 0 ]; then
    echo "FAIL: CinderX verification failed"
    if [ "$BUILD_MODE" = "jit" ]; then
        echo "The JIT may have crashed. Check for segfaults in the log."
    fi
    exit $VERIFY_EXIT
fi

echo ""
echo "=== CinderX rebuild complete ==="
echo "Mode: $BUILD_MODE"
echo "Log: $BUILD_LOG"
