#!/bin/bash
# CinderX ARM JIT — PyTorch Build Script
# Builds PyTorch from source on build-host (aarch64)
#
# Usage:
#   ./17-02-2026-cinderx-build-pytorch.sh [cpu|cuda]
#
# Default: cpu (recommended — unblocks Phase 2a baselines)
# cuda: requires CUDA toolkit and cuDNN properly configured
#
# Prerequisites:
#   - Python 3.12.12+meta venv activated
#   - PyTorch source cloned at $PYTORCH_DIR
#   - numpy, pyyaml, typing-extensions installed in venv

set -euo pipefail

BUILD_MODE="${1:-cpu}"
CINDERX_DEV="$HOME/local/cinderx_dev"
PYTORCH_DIR="$CINDERX_DEV/pytorch"
VENV_DIR="$CINDERX_DEV/venv"
LOG_DIR="$CINDERX_DEV/build-logs"

# Validate
if [ ! -d "$PYTORCH_DIR" ]; then
    echo "ERROR: PyTorch source not found at $PYTORCH_DIR" >&2
    exit 1
fi

if [ ! -f "$VENV_DIR/bin/activate" ]; then
    echo "ERROR: Venv not found at $VENV_DIR" >&2
    echo "Create with: python3 -m venv $VENV_DIR" >&2
    exit 1
fi

# Activate venv
source "$VENV_DIR/bin/activate"

# Record environment
mkdir -p "$LOG_DIR"
TIMESTAMP=$(date -u +%Y%m%dT%H%M%SZ)
BUILD_LOG="$LOG_DIR/pytorch_build_${BUILD_MODE}_${TIMESTAMP}.log"

echo "=== PyTorch Build: mode=$BUILD_MODE ==="
echo "Python: $(python3 --version 2>&1)"
echo "Architecture: $(uname -m)"
echo "PyTorch source: $PYTORCH_DIR"
echo "Build log: $BUILD_LOG"
echo ""

# Install build dependencies
echo "=== Installing build dependencies ==="
pip install --quiet numpy pyyaml typing-extensions 2>&1 | tail -5

# Set build configuration
export MAX_JOBS="${MAX_JOBS:-8}"
export BUILD_TEST=1

case "$BUILD_MODE" in
    cpu)
        echo "=== Building CPU-only PyTorch ==="
        export USE_CUDA=0
        export USE_CUDNN=0
        export USE_NCCL=0
        export USE_DISTRIBUTED=0
        export USE_MKLDNN=1
        ;;
    cuda)
        echo "=== Building PyTorch with CUDA ==="
        export CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
        export USE_CUDA=1
        export USE_CUDNN=1
        if [ ! -d "$CUDA_HOME" ]; then
            echo "ERROR: CUDA_HOME=$CUDA_HOME not found" >&2
            exit 1
        fi
        echo "CUDA_HOME: $CUDA_HOME"
        echo "nvcc: $(nvcc --version 2>&1 | tail -1 || echo 'not found')"
        ;;
    *)
        echo "ERROR: Invalid mode '$BUILD_MODE'. Must be cpu or cuda." >&2
        exit 1
        ;;
esac

# Build
cd "$PYTORCH_DIR"
echo ""
echo "=== Starting build (MAX_JOBS=$MAX_JOBS) ==="
echo "Start time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

python3 setup.py develop 2>&1 | tee "$BUILD_LOG"
BUILD_EXIT=$?

echo ""
echo "End time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

if [ $BUILD_EXIT -ne 0 ]; then
    echo "FAIL: PyTorch build failed (exit $BUILD_EXIT)"
    echo "See: $BUILD_LOG"
    exit $BUILD_EXIT
fi

# Verify
echo ""
echo "=== Verifying build ==="
python3 -c "
import torch
print(f'PyTorch version: {torch.__version__}')
print(f'PyTorch built with CUDA: {torch.version.cuda is not None}')
print(f'CPU available: True')
print(f'CUDA available: {torch.cuda.is_available()}')
print(f'Architecture: {torch._C._get_cpu_capability()}' if hasattr(torch._C, '_get_cpu_capability') else 'Architecture: unknown')
# Quick sanity
x = torch.randn(3, 3)
y = torch.randn(3, 3)
z = x @ y
assert z.shape == (3, 3), f'Matrix multiply failed: {z.shape}'
print('Sanity check: PASS (3x3 matmul)')
"
VERIFY_EXIT=$?

if [ $VERIFY_EXIT -ne 0 ]; then
    echo "FAIL: PyTorch verification failed"
    exit $VERIFY_EXIT
fi

echo ""
echo "=== PyTorch build complete ==="
echo "Mode: $BUILD_MODE"
echo "Log: $BUILD_LOG"
