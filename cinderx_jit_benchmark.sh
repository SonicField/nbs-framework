#!/bin/bash
# CinderX JIT Performance Comparison - ABBA Design
# Self-contained benchmark script for build-host (aarch64)
#
# Compares CinderX+JIT vs vanilla Meta Python (no CinderX loaded).
# JIT ON:  Venv Python with CinderX → _cinderx.so loaded, JIT active
# JIT OFF: System Python -I (isolated) → pure CPython interpreter, no CinderX
#
# Design: ABBA pattern (JIT_ON, JIT_OFF, JIT_OFF, JIT_ON) x N_REPS
# to control for thermal drift, background load, and cache effects.
#
# Usage:
#   ssh build-host
#   cd ~/local/cinderx_dev/cinderx
#   bash cinderx_jit_benchmark.sh [OPTIONS] [N_REPS]
#
# Options:
#   --spec on|off|both   Specialised opcodes (default: on)
#   --exclude LIST       Comma-separated benchmark names to exclude
#   --n-iter N           Iterations per benchmark (default: 100000)
#   --n-warmup N         Specialisation warmup iterations (default: 20)
#   --results-dir DIR    Output directory (default: /tmp/cinderx_benchmark_TIMESTAMP)
#   --help               Show this usage message
#
# Default: 2 repetitions = 8 runs total (4 JIT_ON + 4 JIT_OFF)

set -euo pipefail

# --- Argument parsing ---
SPEC_MODE="on"
EXCLUDE_LIST=""
N_ITER_ARG=""
N_WARMUP_ARG=""
RESULTS_DIR_ARG=""
N_REPS=""

show_help() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
    exit 0
}

require_arg() {
    if [[ $# -lt 2 || -z "${2:-}" ]]; then
        echo "ERROR: $1 requires a value"
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --spec)
            require_arg "$1" "${2:-}"
            SPEC_MODE="$2"
            if [[ "$SPEC_MODE" != "on" && "$SPEC_MODE" != "off" && "$SPEC_MODE" != "both" ]]; then
                echo "ERROR: --spec must be on, off, or both (got: $SPEC_MODE)"
                exit 1
            fi
            shift 2
            ;;
        --exclude)
            require_arg "$1" "${2:-}"
            EXCLUDE_LIST="$2"
            shift 2
            ;;
        --n-iter)
            require_arg "$1" "${2:-}"
            N_ITER_ARG="$2"
            shift 2
            ;;
        --n-warmup)
            require_arg "$1" "${2:-}"
            N_WARMUP_ARG="$2"
            shift 2
            ;;
        --results-dir)
            require_arg "$1" "${2:-}"
            RESULTS_DIR_ARG="$2"
            shift 2
            ;;
        --help|-h)
            show_help
            ;;
        -*)
            echo "ERROR: Unknown option: $1"
            echo "Run with --help for usage."
            exit 1
            ;;
        *)
            # Positional argument: N_REPS
            N_REPS="$1"
            shift
            ;;
    esac
done

N_REPS="${N_REPS:-2}"

# --- Configuration ---
CINDERX_ROOT="${CINDERX_ROOT:-$HOME/local/cinderx_dev/cinderx}"
CINDERX_VENV="${CINDERX_VENV:-$HOME/local/cinderx_dev/venv}"
PYTHON_VANILLA="/usr/local/internal-toolchain/platform010-aarch64/bin/python3.12"
RESULTS_DIR="${RESULTS_DIR_ARG:-/tmp/cinderx_benchmark_$(date +%Y%m%d_%H%M%S)}"
CINDERX_PYTHONPATH="${PYTHONPATH:-$CINDERX_ROOT/cinderx/PythonLib}"

# Activate venv — fail fast if missing
if [ ! -f "$CINDERX_VENV/bin/activate" ]; then
    echo "FATAL: venv not found at $CINDERX_VENV"
    echo "Create it with: python3 -m venv $CINDERX_VENV"
    exit 1
fi
# shellcheck disable=SC1091
source "$CINDERX_VENV/bin/activate"

PYTHON="${CINDERX_PYTHON:-python3}"

# Verify we're on the right machine
ARCH=$(uname -m)
if [ "$ARCH" != "aarch64" ]; then
    echo "ERROR: This script must run on aarch64. Current: $ARCH"
    exit 1
fi

# Verify CinderX is available via venv Python
$PYTHON -c "import _cinderx; import cinderjit" 2>/dev/null || {
    echo "ERROR: CinderX not available via venv Python."
    echo "Venv: $CINDERX_VENV"
    echo "Python: $PYTHON"
    exit 1
}

# Verify vanilla Python is truly CinderX-free
"$PYTHON_VANILLA" -I -c "import _cinderx" 2>/dev/null && {
    echo "ERROR: System Python -I still imports _cinderx. Cannot get clean vanilla baseline."
    exit 1
}
echo "Vanilla Python verified: $PYTHON_VANILLA -I does NOT load CinderX"

mkdir -p "$RESULTS_DIR"

echo "============================================================"
echo "CinderX JIT Performance Comparison"
echo "============================================================"
echo "Platform:    $ARCH"
echo "JIT ON:      $($PYTHON --version 2>&1) (venv + CinderX)"
echo "JIT OFF:     $("$PYTHON_VANILLA" --version 2>&1) (system, isolated)"
echo "Reps:        $N_REPS (= $((N_REPS * 4)) total runs, $((N_REPS * 2)) per condition)"
echo "Spec mode:   $SPEC_MODE"
echo "Exclude:     ${EXCLUDE_LIST:-none}"
echo "N_ITER:      ${N_ITER_ARG:-100000}"
echo "N_WARMUP:    ${N_WARMUP_ARG:-20}"
echo "Results dir: $RESULTS_DIR"
echo "Pattern:     ABBA x $N_REPS"
echo "============================================================"
echo ""

# --- Benchmark Python code ---
# Embedded as heredoc to make the script self-contained.
# Tests both generator-specific and general workloads.
cat > "$RESULTS_DIR/benchmark.py" << 'BENCH_EOF'
"""CinderX JIT micro-benchmarks for aarch64.

Measures wall-clock time for several workloads with and without JIT.
All hot inner functions are defined at module level so they can be
force-compiled individually by the JIT.
Outputs JSON for easy parsing.
"""
import json
import math
import os
import platform
import random
import sys
import time

# =====================================================================
# Module-level hot functions (compilable by JIT)
# =====================================================================

# --- Generator helpers ---
def _gen_simple(n):
    for i in range(n):
        yield i

def _gen_param(base, step, n):
    for i in range(n):
        yield base + i * step

def _compute_nested(a, b):
    return a * b + a - b

def _gen_nested(n):
    for i in range(n):
        yield _compute_nested(i, i + 1)

def _gen_interleaved(base, n):
    for i in range(n):
        yield base + i

# --- Coroutine helpers ---
def _coro_stage(target):
    while True:
        value = (yield)
        target.send(value + 1)

def _coro_sink():
    total = 0
    while True:
        value = (yield)
        total += value

# --- Yield-from helpers ---
def _yf_bottom(n):
    for i in range(n):
        yield i

def _yf_mid(n):
    yield from _yf_bottom(n)

def _yf_top(n):
    yield from _yf_mid(n)

# --- Plain function helpers ---
def _f_add3(a, b, c):
    return a + b + c

def _fib(n):
    if n < 2:
        return n
    return _fib(n - 1) + _fib(n - 2)

# --- Spectral norm helpers ---
_SPECTRAL_N = 100

def _spectral_A(i, j):
    return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)

def _spectral_mul_Av(v):
    n = _SPECTRAL_N
    return [sum(_spectral_A(i, j) * v[j] for j in range(n)) for i in range(n)]

def _spectral_mul_Atv(v):
    n = _SPECTRAL_N
    return [sum(_spectral_A(j, i) * v[j] for j in range(n)) for i in range(n)]

def _spectral_mul_AtAv(v):
    return _spectral_mul_Atv(_spectral_mul_Av(v))

# --- Fannkuch helper ---
def _fannkuch(n):
    perm = list(range(n))
    count = [0] * n
    max_flips = 0
    r = n
    check = 0
    while True:
        if check < 30:
            check += 1
        while r != 1:
            count[r - 1] = r
            r -= 1
        if perm[0] != 0 and perm[n - 1] != n - 1:
            perm2 = list(perm)
            flips = 0
            k = perm2[0]
            while k:
                perm2[:k + 1] = perm2[k::-1]
                flips += 1
                k = perm2[0]
            if flips > max_flips:
                max_flips = flips
        while r != n:
            perm.insert(r, perm.pop(0))
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1
        else:
            return max_flips
    return max_flips

# --- N-Queens helper ---
def _nqueens_solve(n, row=0, cols=0, diag1=0, diag2=0):
    if row == n:
        return 1
    count = 0
    available = ((1 << n) - 1) & ~(cols | diag1 | diag2)
    while available:
        bit = available & (-available)
        available ^= bit
        count += _nqueens_solve(n, row + 1, cols | bit,
                                (diag1 | bit) << 1, (diag2 | bit) >> 1)
    return count

# --- Richards (slots) helpers ---
class _RichardsSlotTask:
    __slots__ = ('id', 'pri', 'next', 'state')
    def __init__(self, tid, pri):
        self.id = tid
        self.pri = pri
        self.next = None
        self.state = 0

# --- Richards (full pyperformance) helpers ---
# Based on Dr Martin Richards' benchmark, via pyperformance bm_richards.
# Multiple task types with polymorphic dispatch, no __slots__.

_RICHARDS_BUFSIZE = 4
_RICHARDS_I_IDLE = 1
_RICHARDS_I_WORK = 2
_RICHARDS_I_HANDLERA = 3
_RICHARDS_I_HANDLERB = 4
_RICHARDS_I_DEVA = 5
_RICHARDS_I_DEVB = 6
_RICHARDS_K_DEV = 1000
_RICHARDS_K_WORK = 1001

class _RPacket:
    def __init__(self, l, i, k):
        self.link = l
        self.ident = i
        self.kind = k
        self.datum = 0
        self.data = [0] * _RICHARDS_BUFSIZE

    def append_to(self, lst):
        self.link = None
        if lst is None:
            return self
        p = lst
        nxt = p.link
        while nxt is not None:
            p = nxt
            nxt = p.link
        p.link = self
        return lst

class _RTaskState:
    def __init__(self):
        self.packet_pending = True
        self.task_waiting = False
        self.task_holding = False

    def packetPending(self):
        self.packet_pending = True
        self.task_waiting = False
        self.task_holding = False
        return self

    def waiting(self):
        self.packet_pending = False
        self.task_waiting = True
        self.task_holding = False
        return self

    def running(self):
        self.packet_pending = False
        self.task_waiting = False
        self.task_holding = False
        return self

    def waitingWithPacket(self):
        self.packet_pending = True
        self.task_waiting = True
        self.task_holding = False
        return self

    def isPacketPending(self):
        return self.packet_pending

    def isTaskWaiting(self):
        return self.task_waiting

    def isTaskHolding(self):
        return self.task_holding

    def isTaskHoldingOrWaiting(self):
        return self.task_holding or (not self.packet_pending and self.task_waiting)

    def isWaitingWithPacket(self):
        return self.packet_pending and self.task_waiting and not self.task_holding

class _RTaskWorkArea:
    def __init__(self):
        self.taskTab = [None] * 10
        self.taskList = None
        self.holdCount = 0
        self.qpktCount = 0

class _RTask(_RTaskState):
    def __init__(self, wa, i, p, w, initialState, r):
        self.link = wa.taskList
        self.ident = i
        self.priority = p
        self.input = w
        self.packet_pending = initialState.isPacketPending()
        self.task_waiting = initialState.isTaskWaiting()
        self.task_holding = initialState.isTaskHolding()
        self.handle = r
        wa.taskList = self
        wa.taskTab[i] = self
        self._wa = wa

    def fn(self, pkt, r):
        raise NotImplementedError

    def addPacket(self, p, old):
        if self.input is None:
            self.input = p
            self.packet_pending = True
            if self.priority > old.priority:
                return self
        else:
            p.append_to(self.input)
        return old

    def runTask(self):
        if self.isWaitingWithPacket():
            msg = self.input
            self.input = msg.link
            if self.input is None:
                self.running()
            else:
                self.packetPending()
        else:
            msg = None
        return self.fn(msg, self.handle)

    def waitTask(self):
        self.task_waiting = True
        return self

    def hold(self):
        self._wa.holdCount += 1
        self.task_holding = True
        return self.link

    def release(self, i):
        t = self._wa.taskTab[i]
        t.task_holding = False
        if t.priority > self.priority:
            return t
        return self

    def qpkt(self, pkt):
        t = self._wa.taskTab[pkt.ident]
        self._wa.qpktCount += 1
        pkt.link = None
        pkt.ident = self.ident
        return t.addPacket(pkt, self)

class _RDeviceTask(_RTask):
    def fn(self, pkt, r):
        if pkt is None:
            pkt = r.pending
            if pkt is None:
                return self.waitTask()
            r.pending = None
            return self.qpkt(pkt)
        r.pending = pkt
        return self.hold()

class _RHandlerTask(_RTask):
    def fn(self, pkt, r):
        if pkt is not None:
            if pkt.kind == _RICHARDS_K_WORK:
                r.work_in = pkt.append_to(r.work_in)
            else:
                r.device_in = pkt.append_to(r.device_in)
        work = r.work_in
        if work is None:
            return self.waitTask()
        count = work.datum
        if count >= _RICHARDS_BUFSIZE:
            r.work_in = work.link
            return self.qpkt(work)
        dev = r.device_in
        if dev is None:
            return self.waitTask()
        r.device_in = dev.link
        dev.datum = work.data[count]
        work.datum = count + 1
        return self.qpkt(dev)

class _RIdleTask(_RTask):
    def __init__(self, wa, i, p, w, s, r):
        _RTask.__init__(self, wa, i, 0, None, s, r)

    def fn(self, pkt, r):
        r.count -= 1
        if r.count == 0:
            return self.hold()
        if r.control & 1 == 0:
            r.control //= 2
            return self.release(_RICHARDS_I_DEVA)
        r.control = r.control // 2 ^ 0xd008
        return self.release(_RICHARDS_I_DEVB)

class _RWorkTask(_RTask):
    def fn(self, pkt, r):
        if pkt is None:
            return self.waitTask()
        dest = _RICHARDS_I_HANDLERB if r.destination == _RICHARDS_I_HANDLERA else _RICHARDS_I_HANDLERA
        r.destination = dest
        pkt.ident = dest
        pkt.datum = 0
        for i in range(_RICHARDS_BUFSIZE):
            r.count += 1
            if r.count > 26:
                r.count = 1
            pkt.data[i] = ord('A') + r.count - 1
        return self.qpkt(pkt)

class _RDeviceTaskRec:
    def __init__(self):
        self.pending = None

class _RIdleTaskRec:
    def __init__(self):
        self.control = 1
        self.count = 10000

class _RHandlerTaskRec:
    def __init__(self):
        self.work_in = None
        self.device_in = None

class _RWorkerTaskRec:
    def __init__(self):
        self.destination = _RICHARDS_I_HANDLERA
        self.count = 0

def _richards_schedule(wa):
    t = wa.taskList
    while t is not None:
        if t.isTaskHoldingOrWaiting():
            t = t.link
        else:
            t = t.runTask()

def _richards_run_once(wa):
    wa.holdCount = 0
    wa.qpktCount = 0
    _RIdleTask(wa, _RICHARDS_I_IDLE, 1, 10000,
               _RTaskState().running(), _RIdleTaskRec())
    wkq = _RPacket(None, 0, _RICHARDS_K_WORK)
    wkq = _RPacket(wkq, 0, _RICHARDS_K_WORK)
    _RWorkTask(wa, _RICHARDS_I_WORK, 1000, wkq,
               _RTaskState().waitingWithPacket(), _RWorkerTaskRec())
    wkq = _RPacket(None, _RICHARDS_I_DEVA, _RICHARDS_K_DEV)
    wkq = _RPacket(wkq, _RICHARDS_I_DEVA, _RICHARDS_K_DEV)
    wkq = _RPacket(wkq, _RICHARDS_I_DEVA, _RICHARDS_K_DEV)
    _RHandlerTask(wa, _RICHARDS_I_HANDLERA, 2000, wkq,
                  _RTaskState().waitingWithPacket(), _RHandlerTaskRec())
    wkq = _RPacket(None, _RICHARDS_I_DEVB, _RICHARDS_K_DEV)
    wkq = _RPacket(wkq, _RICHARDS_I_DEVB, _RICHARDS_K_DEV)
    wkq = _RPacket(wkq, _RICHARDS_I_DEVB, _RICHARDS_K_DEV)
    _RHandlerTask(wa, _RICHARDS_I_HANDLERB, 3000, wkq,
                  _RTaskState().waitingWithPacket(), _RHandlerTaskRec())
    _RDeviceTask(wa, _RICHARDS_I_DEVA, 4000, None,
                 _RTaskState().waiting(), _RDeviceTaskRec())
    _RDeviceTask(wa, _RICHARDS_I_DEVB, 5000, None,
                 _RTaskState().waiting(), _RDeviceTaskRec())
    _richards_schedule(wa)
    return wa.holdCount == 9297 and wa.qpktCount == 23246

# --- Method calls helpers ---
class _Point:
    __slots__ = ('x', 'y')
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def distance_to(self, other):
        dx = self.x - other.x
        dy = self.y - other.y
        return (dx * dx + dy * dy) ** 0.5
    def translate(self, dx, dy):
        return _Point(self.x + dx, self.y + dy)

# =====================================================================
# All module-level functions to force-compile
# =====================================================================
_ALL_COMPILABLE = [
    _gen_simple, _gen_param, _compute_nested, _gen_nested, _gen_interleaved,
    _coro_stage, _coro_sink,
    _yf_bottom, _yf_mid, _yf_top,
    _f_add3, _fib,
    _spectral_A, _spectral_mul_Av, _spectral_mul_Atv, _spectral_mul_AtAv,
    _fannkuch, _nqueens_solve,
    _RichardsSlotTask.__init__,
    _RPacket.__init__, _RPacket.append_to,
    _RTaskState.__init__, _RTaskState.packetPending, _RTaskState.waiting,
    _RTaskState.running, _RTaskState.waitingWithPacket,
    _RTaskState.isPacketPending, _RTaskState.isTaskWaiting,
    _RTaskState.isTaskHolding, _RTaskState.isTaskHoldingOrWaiting,
    _RTaskState.isWaitingWithPacket,
    _RTask.__init__, _RTask.addPacket, _RTask.runTask,
    _RTask.waitTask, _RTask.hold, _RTask.release, _RTask.qpkt,
    _RDeviceTask.fn, _RHandlerTask.fn, _RIdleTask.__init__, _RIdleTask.fn, _RWorkTask.fn,
    _richards_schedule, _richards_run_once,
    _Point.__init__, _Point.distance_to, _Point.translate,
]

# =====================================================================
# Benchmark wrapper functions
# =====================================================================

def bench_generator_simple(n_iter):
    """Simple generator: yield integers."""
    total = 0
    for val in _gen_simple(n_iter):
        total += val
    return total

def bench_generator_parameterised(n_iter):
    """Parameterised generator: yield with arithmetic."""
    total = 0
    for val in _gen_param(100, 3, n_iter):
        total += val
    return total

def bench_generator_nested(n_iter):
    """Generator calling another function."""
    total = 0
    for val in _gen_nested(n_iter):
        total += val
    return total

def bench_generator_interleaved(n_iter):
    """Multiple interleaved generators."""
    g1 = _gen_interleaved(0, n_iter)
    g2 = _gen_interleaved(1000, n_iter)
    g3 = _gen_interleaved(2000, n_iter)
    total = 0
    for _ in range(n_iter):
        total += next(g1) + next(g2) + next(g3)
    return total

def bench_coroutine_chain(n_iter):
    """Coroutine/generator chain (send/yield pattern)."""
    s = _coro_sink()
    next(s)
    g1 = _coro_stage(s); next(g1)
    g2 = _coro_stage(g1); next(g2)
    g3 = _coro_stage(g2); next(g3)
    for i in range(n_iter):
        g3.send(i)
    return 0

def bench_yield_from_chain(n_iter):
    """yield-from delegation chain."""
    total = 0
    for val in _yf_top(n_iter):
        total += val
    return total

def bench_function_calls(n_iter):
    """Regular function call overhead."""
    total = 0
    for i in range(n_iter):
        total += _f_add3(i, i + 1, i + 2)
    return total

def bench_dict_operations(n_iter):
    """Dictionary operations (non-generator baseline)."""
    d = {}
    for i in range(n_iter):
        d[i] = i * i
    total = sum(d.values())
    return total

def bench_list_comprehension(n_iter):
    """List comprehension (non-generator baseline)."""
    result = [i * i + i for i in range(n_iter)]
    return sum(result)

def bench_float_arithmetic(n_iter):
    """Float-heavy computation (pyperformance 'float' style)."""
    x = 1.0
    for i in range(1, n_iter + 1):
        x = x * 1.000001 + 0.5 / i - x * 0.000001
    return x

def bench_fibonacci_recursive(n_iter):
    """Recursive function calls (call overhead stress test)."""
    return _fib(25)

def bench_nbody_step(n_iter):
    """N-body simulation step (pyperformance 'nbody' style)."""
    bodies = [
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0],
        [4.84, -1.16, -1.04e-1, 1.66e-3, 7.70e-3, -6.90e-5, 9.55e-4],
        [8.34, 4.12, -4.03e-1, -2.77e-3, 4.99e-3, 2.30e-5, 2.86e-4],
    ]
    dt = 0.01
    for _ in range(n_iter // 10):
        for i in range(len(bodies)):
            for j in range(i + 1, len(bodies)):
                dx = bodies[i][0] - bodies[j][0]
                dy = bodies[i][1] - bodies[j][1]
                dz = bodies[i][2] - bodies[j][2]
                dist = math.sqrt(dx*dx + dy*dy + dz*dz)
                mag = dt / (dist * dist * dist)
                bodies[i][3] -= dx * mag * bodies[j][6]
                bodies[i][4] -= dy * mag * bodies[j][6]
                bodies[i][5] -= dz * mag * bodies[j][6]
                bodies[j][3] += dx * mag * bodies[i][6]
                bodies[j][4] += dy * mag * bodies[i][6]
                bodies[j][5] += dz * mag * bodies[i][6]
        for b in bodies:
            b[0] += dt * b[3]
            b[1] += dt * b[4]
            b[2] += dt * b[5]
    return bodies[0][0]

def bench_spectral_norm(n_iter):
    """Spectral norm (pyperformance style)."""
    u = [1.0] * _SPECTRAL_N
    for _ in range(10):
        v = _spectral_mul_AtAv(u)
        u = _spectral_mul_AtAv(v)
    vBv = sum(u[i] * v[i] for i in range(_SPECTRAL_N))
    vv = sum(v[i] * v[i] for i in range(_SPECTRAL_N))
    return math.sqrt(vBv / vv)

def bench_chaos_game(n_iter):
    """Chaos game / fractal (pyperformance 'chaos' style)."""
    random.seed(42)
    vertices = [(0.0, 0.0), (1.0, 0.0), (0.5, 0.866)]
    x, y = 0.5, 0.5
    total = 0.0
    for _ in range(n_iter):
        v = vertices[random.randint(0, 2)]
        x = (x + v[0]) / 2
        y = (y + v[1]) / 2
        total += x + y
    return total

def bench_richards_slots(n_iter):
    """Richards (simplified, __slots__) — NOT the pyperformance variant."""
    tasks = [_RichardsSlotTask(i, i % 5) for i in range(n_iter // 100)]
    total = 0
    for _ in range(100):
        for t in tasks:
            t.state = (t.state + t.pri + 1) % 7
            total += t.state
    return total

def bench_richards_full(n_iter):
    """Richards benchmark (proper pyperformance variant).
    Polymorphic task dispatch, no __slots__, linked-list packets.
    Each iteration creates the full task set and runs the scheduler."""
    n = max(1, n_iter // 10000)
    wa = _RTaskWorkArea()
    for _ in range(n):
        wa.taskTab = [None] * 10
        wa.taskList = None
        ok = _richards_run_once(wa)
        assert ok, f"Richards validation failed: hold={wa.holdCount} qpkt={wa.qpktCount}"
    return n

def bench_fannkuch(n_iter):
    """Fannkuch benchmark — permutation + reversal."""
    return _fannkuch(9)

def bench_nqueens(n_iter):
    """N-Queens solver — recursive backtracking."""
    return _nqueens_solve(11)

def bench_json_roundtrip(n_iter):
    """JSON serialisation/deserialisation (real-world workload)."""
    data = {
        "users": [
            {"id": i, "name": f"user_{i}", "scores": [j * 1.1 for j in range(10)],
             "active": i % 2 == 0, "tags": [f"tag_{k}" for k in range(5)]}
            for i in range(50)
        ],
        "metadata": {"version": 1, "count": 50}
    }
    total = 0
    for _ in range(n_iter // 1000):
        s = json.dumps(data)
        d = json.loads(s)
        total += len(d["users"])
    return total

def bench_method_calls(n_iter):
    """Class method dispatch overhead."""
    points = [_Point(i * 0.1, i * 0.2) for i in range(100)]
    total = 0.0
    for _ in range(n_iter // 100):
        for i in range(len(points) - 1):
            total += points[i].distance_to(points[i + 1])
            points[i] = points[i].translate(0.01, 0.02)
    return total

def bench_string_ops(n_iter):
    """String manipulation — join, split, replace, case conversion."""
    words = [f"word_{i}" for i in range(100)]
    total = 0
    for _ in range(n_iter // 100):
        s = " ".join(words)
        parts = s.split(" ")
        s2 = "-".join(reversed(parts))
        total += len(s2)
        s3 = s.upper().lower().replace("word", "item")
        total += s3.count("item")
    return total

def bench_unpack_sequence(n_iter):
    """Tuple/list unpacking in tight loops."""
    pairs = [(i, i + 1) for i in range(100)]
    triples = [(i, i + 1, i + 2) for i in range(100)]
    total = 0
    for _ in range(n_iter // 100):
        for a, b in pairs:
            total += a + b
        for a, b, c in triples:
            total += a + b + c
    return total

def bench_exceptions(n_iter):
    """Exception handling overhead — try/except in hot loop."""
    d = {i: i * 2 for i in range(0, 1000, 2)}
    total = 0
    for i in range(n_iter):
        try:
            total += d[i % 1000]
        except KeyError:
            total += 1
    return total

# --- Configuration (overridable via environment) ---
N_ITER = int(os.environ.get("BENCH_N_ITER", "100000"))
N_WARMUP = 3       # warmup AFTER JIT compilation (thermal)
N_SPECIALISE = int(os.environ.get("BENCH_N_WARMUP", "20"))
N_MEASURE = 5
_EXCLUDE = set(os.environ.get("BENCH_EXCLUDE", "").split(",")) - {""}
_SPEC_ENABLED = os.environ.get("BENCH_SPEC", "on") == "on"

BENCHMARKS = [
    # Generator-focused (our Phase 5f work)
    ("gen_simple",         bench_generator_simple),
    ("gen_parameterised",  bench_generator_parameterised),
    ("gen_nested",         bench_generator_nested),
    ("gen_interleaved",    bench_generator_interleaved),
    ("coroutine_chain",    bench_coroutine_chain),
    ("yield_from_chain",   bench_yield_from_chain),
    # Standard pyperformance-style
    ("func_calls",         bench_function_calls),
    ("float_arith",        bench_float_arithmetic),
    ("fibonacci",          bench_fibonacci_recursive),
    ("nbody",              bench_nbody_step),
    ("spectral_norm",      bench_spectral_norm),
    ("chaos_game",         bench_chaos_game),
    ("richards_slots",     bench_richards_slots),
    ("richards_full",      bench_richards_full),
    ("fannkuch",           bench_fannkuch),
    ("nqueens",            bench_nqueens),
    ("json_roundtrip",     bench_json_roundtrip),
    ("method_calls",       bench_method_calls),
    # Data structure / general workloads
    ("dict_ops",           bench_dict_operations),
    ("list_comp",          bench_list_comprehension),
    ("string_ops",         bench_string_ops),
    ("unpack_seq",         bench_unpack_sequence),
    ("exceptions",         bench_exceptions),
]

# Apply exclusion filter
if _EXCLUDE:
    BENCHMARKS = [(n, f) for n, f in BENCHMARKS if n not in _EXCLUDE]

# --- JIT status ---
try:
    import _cinderx
    import cinderjit
    jit_available = True
    jit_disabled = os.environ.get("PYTHONJITDISABLE", "0") == "1"
except ImportError:
    jit_available = False
    jit_disabled = True

condition = "JIT_OFF" if (not jit_available or jit_disabled) else "JIT_ON"

# Enable specialised opcode support in JIT (controlled by --spec flag)
if jit_available and not jit_disabled and _SPEC_ENABLED:
    cinderjit.enable_specialized_opcodes()

# Phase 1: Specialisation warmup — let CPython adaptive interpreter specialise
# bytecodes BEFORE JIT compilation. This ensures the JIT compiles SPECIALISED
# opcodes (BINARY_OP_ADD_INT, CALL_PY_EXACT_ARGS, etc.) rather than generic ones.
# CPython 3.12 needs ~8 calls to specialise; we use 20 for safety.
for name, func in BENCHMARKS:
    for _ in range(N_SPECIALISE):
        func(N_ITER)

# Also warm up compilable helper functions
for func in _ALL_COMPILABLE:
    try:
        func()  # Call once if possible — some may need args
    except (TypeError, Exception):
        pass  # Not all helpers can be called bare; that's fine

# Phase 2: JIT compile AFTER specialisation
if jit_available and not jit_disabled:
    for func in _ALL_COMPILABLE:
        cinderjit.force_compile(func)
    for name, func in BENCHMARKS:
        cinderjit.force_compile(func)

results = {
    "platform": platform.machine(),
    "python_version": sys.version.split()[0],
    "condition": condition,
    "jit_available": jit_available,
    "jit_disabled": jit_disabled,
    "spec_enabled": _SPEC_ENABLED,
    "n_iter": N_ITER,
    "n_specialise": N_SPECIALISE,
    "n_warmup": N_WARMUP,
    "n_measure": N_MEASURE,
    "excluded": sorted(_EXCLUDE) if _EXCLUDE else [],
    "benchmarks": {},
}

if jit_available and not jit_disabled:
    jit_status = {}
    for func in _ALL_COMPILABLE:
        jit_status[func.__qualname__] = cinderjit.is_jit_compiled(func)
    for name, func in BENCHMARKS:
        jit_status[name] = cinderjit.is_jit_compiled(func)
    results["jit_compiled"] = jit_status

for name, func in BENCHMARKS:
    # Warmup
    for _ in range(N_WARMUP):
        func(N_ITER)

    # Measure
    times = []
    return_val = None
    for _ in range(N_MEASURE):
        t0 = time.perf_counter_ns()
        rv = func(N_ITER)
        t1 = time.perf_counter_ns()
        times.append((t1 - t0) / 1e6)  # Convert to ms
        if return_val is None:
            return_val = rv

    results["benchmarks"][name] = {
        "times_ms": times,
        "mean_ms": sum(times) / len(times),
        "min_ms": min(times),
        "max_ms": max(times),
        "return_value": repr(return_val),
    }

# Output JSON to stdout
print(json.dumps(results, indent=2))
BENCH_EOF

# --- Common env vars for benchmark Python ---
BENCH_ENV="BENCH_EXCLUDE=$EXCLUDE_LIST"
if [ -n "$N_ITER_ARG" ]; then
    BENCH_ENV="$BENCH_ENV BENCH_N_ITER=$N_ITER_ARG"
fi
if [ -n "$N_WARMUP_ARG" ]; then
    BENCH_ENV="$BENCH_ENV BENCH_N_WARMUP=$N_WARMUP_ARG"
fi

# --- Function to run one ABBA sweep ---
run_abba_sweep() {
    local spec_setting="$1"
    local sweep_dir="$2"
    local sweep_label="$3"

    mkdir -p "$sweep_dir"
    cp "$RESULTS_DIR/benchmark.py" "$sweep_dir/benchmark.py"

    echo "============================================================"
    echo "ABBA sweep: $sweep_label (spec=$spec_setting)"
    echo "============================================================"

    local run_num=0
    for rep in $(seq 1 "$N_REPS"); do
        for condition in ON OFF OFF ON; do
            run_num=$((run_num + 1))
            local label="jit_${condition,,}_rep${rep}_run${run_num}"
            local output_file="$sweep_dir/${label}.json"

            echo -n "Run $run_num/$((N_REPS * 4)): JIT_${condition} (rep $rep) ... "

            if [ "$condition" = "OFF" ]; then
                # Run WITHOUT CinderX - system Python in isolated mode
                env $BENCH_ENV BENCH_SPEC="$spec_setting" \
                    "$PYTHON_VANILLA" -I "$sweep_dir/benchmark.py" > "$output_file" 2>/dev/null
            else
                # Run WITH CinderX on PYTHONPATH - JIT active
                env $BENCH_ENV BENCH_SPEC="$spec_setting" \
                    PYTHONPATH="$CINDERX_PYTHONPATH" PYTHONJIT=1 \
                    $PYTHON "$sweep_dir/benchmark.py" > "$output_file" 2>/dev/null
            fi

            # Extract mean times for quick display
            MEAN=$($PYTHON -c "
import json, sys
with open('$output_file') as f:
    d = json.load(f)
total = sum(b['mean_ms'] for b in d['benchmarks'].values())
print(f'{total:.1f}ms total')
")
            echo "$MEAN"

            # Brief pause between runs to let CPU settle
            sleep 2
        done
    done
    echo ""
}

# --- Run ABBA pattern ---
echo "Starting ABBA benchmark runs..."
echo ""

if [ "$SPEC_MODE" = "both" ]; then
    # Run two sweeps: spec OFF then spec ON
    run_abba_sweep "off" "$RESULTS_DIR/spec_off" "Specialisations OFF"
    run_abba_sweep "on"  "$RESULTS_DIR/spec_on"  "Specialisations ON"
else
    # Single sweep
    run_abba_sweep "$SPEC_MODE" "$RESULTS_DIR" "Specialisations ${SPEC_MODE^^}"
fi

echo "All runs complete. Generating comparison..."
echo ""

# --- Comparison script ---
cat > "$RESULTS_DIR/compare.py" << 'COMPARE_EOF'
"""Compare ABBA benchmark results.

Usage:
  compare.py DIR                  — compare JIT ON vs OFF in DIR
  compare.py DIR1 DIR2            — compare two sweep directories (e.g. spec_off vs spec_on)
"""
import glob
import json
import os
import sys

def load_sweep(results_dir):
    """Load JIT ON and OFF results from a single sweep directory."""
    jit_on_files = sorted(glob.glob(os.path.join(results_dir, "jit_on_*.json")))
    jit_off_files = sorted(glob.glob(os.path.join(results_dir, "jit_off_*.json")))
    return jit_on_files, jit_off_files

def aggregate(files, all_benchmarks):
    """Compute per-benchmark mean across files."""
    means = {b: [] for b in all_benchmarks}
    for f in files:
        with open(f) as fh:
            d = json.load(fh)
            for b in all_benchmarks:
                if b in d["benchmarks"]:
                    means[b].append(d["benchmarks"][b]["mean_ms"])
    return means

def print_comparison(jit_on_files, jit_off_files, label=""):
    """Print a JIT ON vs OFF comparison table."""
    all_benchmarks = set()
    for f in jit_on_files + jit_off_files:
        with open(f) as fh:
            d = json.load(fh)
            all_benchmarks.update(d["benchmarks"].keys())
    all_benchmarks = sorted(all_benchmarks)

    jit_on_means = aggregate(jit_on_files, all_benchmarks)
    jit_off_means = aggregate(jit_off_files, all_benchmarks)

    title = f"CinderX JIT Performance Comparison (aarch64)"
    if label:
        title += f" — {label}"
    print("=" * 80)
    print(title)
    print("=" * 80)
    print(f"JIT ON runs:  {len(jit_on_files)}")
    print(f"JIT OFF runs: {len(jit_off_files)}")

    # Show spec status from first JIT_ON run
    if jit_on_files:
        with open(jit_on_files[0]) as fh:
            d = json.load(fh)
            spec = d.get("spec_enabled", "unknown")
            print(f"Spec enabled: {spec}")
            excluded = d.get("excluded", [])
            if excluded:
                print(f"Excluded:     {', '.join(excluded)}")

    print("")
    print(f"{'Benchmark':<25} {'No CinderX':>12} {'CinderX+JIT':>12} {'Speedup':>10} {'Δ%':>8}")
    print("-" * 80)

    total_on = 0
    total_off = 0
    bench_results = {}

    for b in all_benchmarks:
        on_mean = sum(jit_on_means[b]) / len(jit_on_means[b]) if jit_on_means[b] else 0
        off_mean = sum(jit_off_means[b]) / len(jit_off_means[b]) if jit_off_means[b] else 0

        total_on += on_mean
        total_off += off_mean

        if on_mean > 0:
            speedup = off_mean / on_mean
            delta_pct = ((off_mean - on_mean) / off_mean) * 100
        else:
            speedup = 0
            delta_pct = 0

        bench_results[b] = {"on_mean": on_mean, "off_mean": off_mean, "speedup": speedup, "delta_pct": delta_pct}
        marker = "**" if speedup > 1.05 else ("!!" if speedup < 0.95 else "  ")
        print(f"{b:<25} {off_mean:>10.2f}ms {on_mean:>10.2f}ms {speedup:>9.2f}x {delta_pct:>7.1f}% {marker}")

    print("-" * 80)
    if total_on > 0:
        overall_speedup = total_off / total_on
        overall_delta = ((total_off - total_on) / total_off) * 100
        print(f"{'TOTAL':<25} {total_off:>10.2f}ms {total_on:>10.2f}ms {overall_speedup:>9.2f}x {overall_delta:>7.1f}%")
    print("=" * 80)
    print("")
    print("** = JIT >5% faster   !! = JIT >5% slower")
    print("")

    # JIT compilation status from first JIT_ON run
    if jit_on_files:
        with open(jit_on_files[0]) as fh:
            d = json.load(fh)
            if "jit_compiled" in d:
                print("JIT compilation status:")
                for b, compiled in d["jit_compiled"].items():
                    status = "COMPILED" if compiled else "INTERPRETED"
                    print(f"  {b}: {status}")
        print("")

    # Output validation: check return values match between JIT ON and OFF
    on_returns = {}
    off_returns = {}
    for f in jit_on_files:
        with open(f) as fh:
            d = json.load(fh)
            for b, data in d["benchmarks"].items():
                if "return_value" in data:
                    on_returns.setdefault(b, set()).add(data["return_value"])
    for f in jit_off_files:
        with open(f) as fh:
            d = json.load(fh)
            for b, data in d["benchmarks"].items():
                if "return_value" in data:
                    off_returns.setdefault(b, set()).add(data["return_value"])

    mismatches = []
    for b in sorted(set(on_returns) & set(off_returns)):
        if on_returns[b] != off_returns[b]:
            mismatches.append((b, on_returns[b], off_returns[b]))

    if mismatches:
        print("!! OUTPUT VALIDATION FAILURES !!")
        for b, on_vals, off_vals in mismatches:
            print(f"  {b}: JIT_ON={on_vals} JIT_OFF={off_vals}")
        print("WARNING: Different return values indicate a correctness bug.")
        print("")
    elif on_returns:
        print("Output validation: all return values match between JIT ON and OFF")
        print("")

    return bench_results

def print_spec_comparison(results_off, results_on):
    """Print a specialisation impact comparison (spec OFF vs ON)."""
    all_benchmarks = sorted(set(results_off.keys()) | set(results_on.keys()))

    print("=" * 90)
    print("Specialisation Impact (Spec OFF → ON, JIT ON only)")
    print("=" * 90)
    print(f"{'Benchmark':<25} {'Spec OFF Δ%':>12} {'Spec ON Δ%':>12} {'Improvement':>14} {'Direction':>10}")
    print("-" * 90)

    for b in all_benchmarks:
        off_delta = results_off.get(b, {}).get("delta_pct", 0)
        on_delta = results_on.get(b, {}).get("delta_pct", 0)
        improvement = on_delta - off_delta

        if improvement > 2:
            direction = "↑"
        elif improvement < -2:
            direction = "↓"
        else:
            direction = "—"

        print(f"{b:<25} {off_delta:>10.1f}% {on_delta:>10.1f}% {improvement:>12.1f}pp {direction:>8}")

    print("=" * 90)
    print("")

# Main
if len(sys.argv) < 2:
    print("Usage: compare.py DIR [DIR2]")
    sys.exit(1)

if len(sys.argv) == 2:
    # Single sweep comparison
    results_dir = sys.argv[1]
    jit_on, jit_off = load_sweep(results_dir)
    if not jit_on or not jit_off:
        print("ERROR: No result files found")
        sys.exit(1)
    print_comparison(jit_on, jit_off)
elif len(sys.argv) == 3:
    # Two-sweep comparison (spec OFF vs ON)
    dir_off = sys.argv[1]
    dir_on = sys.argv[2]
    on1, off1 = load_sweep(dir_off)
    on2, off2 = load_sweep(dir_on)
    if not on1 or not off1:
        print(f"ERROR: No result files in {dir_off}")
        sys.exit(1)
    if not on2 or not off2:
        print(f"ERROR: No result files in {dir_on}")
        sys.exit(1)
    results_off = print_comparison(on1, off1, "Spec OFF")
    results_on = print_comparison(on2, off2, "Spec ON")
    print_spec_comparison(results_off, results_on)
COMPARE_EOF

# Run comparison
if [ "$SPEC_MODE" = "both" ]; then
    $PYTHON "$RESULTS_DIR/compare.py" "$RESULTS_DIR/spec_off" "$RESULTS_DIR/spec_on"
else
    $PYTHON "$RESULTS_DIR/compare.py" "$RESULTS_DIR"
fi

echo ""
echo "Raw results: $RESULTS_DIR/"
if [ "$SPEC_MODE" = "both" ]; then
    echo "Re-run comparison: $PYTHON $RESULTS_DIR/compare.py $RESULTS_DIR/spec_off $RESULTS_DIR/spec_on"
else
    echo "Re-run comparison: $PYTHON $RESULTS_DIR/compare.py $RESULTS_DIR"
fi
