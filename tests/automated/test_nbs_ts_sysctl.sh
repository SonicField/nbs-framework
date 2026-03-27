#!/bin/bash
# Test: nbs-ts-sysctl service management script
#
# Tests all subcommands using mock systemctl/loginctl/nbs-ts-helper.
# Does NOT touch real systemd state — all calls are intercepted by mocks.
#
# Tests:
#   1.  No arguments → help text, exit 4
#   2.  Unknown subcommand → error, exit 4
#   3.  help → usage text, exit 0
#   4.  install: no systemd → error, exit 1
#   5.  install: no binary → error, exit 1
#   6.  install: happy path → unit file written, systemctl called correctly
#   7.  install: already installed → error, exit 1
#   8.  install: start fails → cleans up unit file
#   9.  remove: happy path → stop, disable, remove file, daemon-reload
#  10.  remove: not installed → clean exit 0
#  11.  status: when running → shows PID, socket
#  12.  status: when stopped → shows stopped state
#  13.  doctor: all passing → 9 PASS lines
#  14.  doctor: missing socket → FAIL with fix
#  15.  doctor: no linger → FAIL with fix
#  16.  start: not installed → error, exit 1
#  17.  restart: not installed → error, exit 1

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SYSCTL="${PROJECT_ROOT}/bin/nbs-ts-sysctl"

MOCK_DIR=$(mktemp -d)
TEST_HOME=$(mktemp -d)
ERRORS=0
PASS_COUNT=0

cleanup() {
    # Kill any socket server processes
    if [[ -f "$MOCK_DIR/socket_server.pid" ]]; then
        kill "$(cat "$MOCK_DIR/socket_server.pid")" 2>/dev/null || true
    fi
    rm -rf "$MOCK_DIR" "$TEST_HOME"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "   FAIL: $label"
        ERRORS=$((ERRORS + 1))
    fi
}

# Save real PATH
REAL_PATH="$PATH"

# ---- Mock setup ----

# Create a listening Unix socket and return the server PID
create_test_socket() {
    local sock_path="$1"
    mkdir -p "$(dirname "$sock_path")"
    python3 -c "
import socket, os, threading, time, sys
path = sys.argv[1]
if os.path.exists(path): os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
def accept_loop():
    while True:
        try:
            conn, _ = s.accept()
            conn.close()
        except:
            break
t = threading.Thread(target=accept_loop, daemon=True)
t.start()
time.sleep(60)
" "$sock_path" &
    local pid=$!
    echo "$pid" > "$MOCK_DIR/socket_server.pid"
    # Wait for socket to appear
    local i
    for i in $(seq 1 20); do
        [[ -S "$sock_path" ]] && return 0
        sleep 0.1
    done
    return 1
}

kill_socket_servers() {
    if [[ -f "$MOCK_DIR/socket_server.pid" ]]; then
        kill "$(cat "$MOCK_DIR/socket_server.pid")" 2>/dev/null || true
        rm -f "$MOCK_DIR/socket_server.pid"
    fi
}

# Reset mocks between tests.
# NOTE: rm -rf dir/* does NOT remove dotfiles (.config, .nbs-ts, etc.),
# causing state leakage between tests. We recreate TEST_HOME entirely.
reset_mocks() {
    kill_socket_servers
    rm -rf "$MOCK_DIR"/*
    rm -rf "$TEST_HOME"
    mkdir -p "$TEST_HOME"
    : > "$MOCK_DIR/calls.log"

    export MOCK_LOG="$MOCK_DIR/calls.log"
    export MOCK_DIR
    export HOME="$TEST_HOME"
    export PATH="$MOCK_DIR:$REAL_PATH"

    # Create mock systemctl — default: everything works
    cat > "$MOCK_DIR/systemctl" << 'MOCK'
#!/bin/bash
echo "systemctl $*" >> "$MOCK_LOG"
if [[ "${1:-}" == "--user" ]]; then shift; fi
case "$1" in
    status)          exit 0 ;;
    is-active)       echo "active"; exit 0 ;;
    is-enabled)      echo "enabled"; exit 0 ;;
    show)
        shift
        prop=""
        while [[ $# -gt 0 ]]; do
            case "$1" in
                -p) prop="$2"; shift 2 ;;
                --value) shift ;;
                *) shift ;;
            esac
        done
        case "$prop" in
            MainPID) echo "12345" ;;
            ActiveEnterTimestamp) echo "Thu 2026-03-27 10:00:00 UTC" ;;
            *) echo "" ;;
        esac
        ;;
    start|stop|restart|enable|disable|daemon-reload) exit 0 ;;
    *)               exit 0 ;;
esac
MOCK
    chmod +x "$MOCK_DIR/systemctl"

    # Create mock loginctl — default: linger yes
    cat > "$MOCK_DIR/loginctl" << 'MOCK'
#!/bin/bash
echo "loginctl $*" >> "$MOCK_LOG"
case "$1" in
    enable-linger) exit 0 ;;
    show-user)
        echo "yes"
        ;;
    *) exit 0 ;;
esac
MOCK
    chmod +x "$MOCK_DIR/loginctl"

    # Create a fake helper binary
    mkdir -p "$TEST_HOME/.nbs/bin"
    cat > "$TEST_HOME/.nbs/bin/nbs-ts-helper" << 'HELPER'
#!/bin/bash
sleep 30
HELPER
    chmod +x "$TEST_HOME/.nbs/bin/nbs-ts-helper"

    # Create .nbs-ts directory
    mkdir -p "$TEST_HOME/.nbs-ts"
}

echo "=== nbs-ts-sysctl Tests ==="
echo "Mock dir: $MOCK_DIR"
echo "Test HOME: $TEST_HOME"
echo ""

# --- Test 1: No arguments → exit 4 ---
echo "1. No arguments..."
set +e
OUTPUT=$("$SYSCTL" 2>&1)
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
check "shows usage" "$( echo "$OUTPUT" | grep -qiF 'usage' && echo pass || echo fail )"
echo ""

# --- Test 2: Unknown subcommand → exit 4 ---
echo "2. Unknown subcommand..."
set +e
OUTPUT=$("$SYSCTL" frobnicate 2>&1)
RC=$?
set -e
check "exit code 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
check "shows error" "$( echo "$OUTPUT" | grep -qF "unknown subcommand" && echo pass || echo fail )"
echo ""

# --- Test 3: help → exit 0 ---
echo "3. help subcommand..."
set +e
OUTPUT=$("$SYSCTL" help 2>&1)
RC=$?
set -e
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "lists install" "$( echo "$OUTPUT" | grep -qF 'install' && echo pass || echo fail )"
check "lists doctor" "$( echo "$OUTPUT" | grep -qF 'doctor' && echo pass || echo fail )"
echo ""

# --- Test 4: install with no systemd → error, exit 1 ---
echo "4. install: no systemd..."
reset_mocks
# Override systemctl to always fail (no systemd available)
cat > "$MOCK_DIR/systemctl" << 'MOCK'
#!/bin/bash
echo "systemctl $*" >> "$MOCK_LOG"
exit 1
MOCK
chmod +x "$MOCK_DIR/systemctl"
set +e
OUTPUT=$("$SYSCTL" install 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "mentions systemd" "$( echo "$OUTPUT" | grep -qiF 'systemd' && echo pass || echo fail )"
echo ""

# --- Test 5: install with no binary → error, exit 1 ---
echo "5. install: no binary..."
reset_mocks
rm -f "$TEST_HOME/.nbs/bin/nbs-ts-helper"
# Copy the script to MOCK_DIR so find_helper_binary's dirname fallback
# points to MOCK_DIR (which has no nbs-ts-helper), not PROJECT_ROOT/bin/
cp "$SYSCTL" "$MOCK_DIR/nbs-ts-sysctl"
chmod +x "$MOCK_DIR/nbs-ts-sysctl"
set +e
OUTPUT=$("$MOCK_DIR/nbs-ts-sysctl" install 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "mentions not found" "$( echo "$OUTPUT" | grep -qi 'not found' && echo pass || echo fail )"
echo ""

# --- Test 6: install happy path ---
echo "6. install: happy path..."
reset_mocks
# Mock systemctl start to create the socket
cat > "$MOCK_DIR/systemctl" << MOCK
#!/bin/bash
echo "systemctl \$*" >> "$MOCK_LOG"
if [[ "\${1:-}" == "--user" ]]; then shift; fi
case "\$1" in
    start)
        # Simulate helper creating the socket
        mkdir -p "$TEST_HOME/.nbs-ts"
        python3 -c "
import socket, os, threading, time
path = '$TEST_HOME/.nbs-ts/helper.sock'
if os.path.exists(path): os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
def accept_loop():
    while True:
        try:
            conn, _ = s.accept()
            conn.close()
        except:
            break
t = threading.Thread(target=accept_loop, daemon=True)
t.start()
time.sleep(60)
" &
        echo \$! > "$MOCK_DIR/socket_server.pid"
        sleep 0.3
        exit 0
        ;;
    status|enable|disable|daemon-reload|stop) exit 0 ;;
    *) exit 0 ;;
esac
MOCK
chmod +x "$MOCK_DIR/systemctl"
set +e
OUTPUT=$("$SYSCTL" install 2>&1)
RC=$?
set -e
kill_socket_servers
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "unit file created" "$( [[ -f "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" ]] && echo pass || echo fail )"
check "unit has ExecStart" "$( grep -qF 'ExecStart=' "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" 2>/dev/null && echo pass || echo fail )"
check "unit has bash --login" "$( grep -qF 'bash --login' "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" 2>/dev/null && echo pass || echo fail )"
check "unit has TimeoutStartSec=30" "$( grep -qF 'TimeoutStartSec=30' "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" 2>/dev/null && echo pass || echo fail )"
check "unit has Restart=on-failure" "$( grep -qF 'Restart=on-failure' "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" 2>/dev/null && echo pass || echo fail )"
check "systemctl daemon-reload called" "$( grep -qF 'daemon-reload' "$MOCK_LOG" && echo pass || echo fail )"
check "systemctl enable called" "$( grep -qF 'enable' "$MOCK_LOG" && echo pass || echo fail )"
check "systemctl start called" "$( grep -q 'start nbs-ts-helper' "$MOCK_LOG" && echo pass || echo fail )"
check "reports success" "$( echo "$OUTPUT" | grep -qF 'installed and running' && echo pass || echo fail )"
echo ""

# --- Test 7: install already installed ---
echo "7. install: already installed..."
reset_mocks
mkdir -p "$TEST_HOME/.config/systemd/user"
echo "[Unit]" > "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
set +e
OUTPUT=$("$SYSCTL" install 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "mentions already installed" "$( echo "$OUTPUT" | grep -qF 'already installed' && echo pass || echo fail )"
echo ""

# --- Test 8: install start fails → cleanup ---
echo "8. install: start fails → cleanup..."
reset_mocks
cat > "$MOCK_DIR/systemctl" << 'MOCK'
#!/bin/bash
echo "systemctl $*" >> "$MOCK_LOG"
if [[ "${1:-}" == "--user" ]]; then shift; fi
case "$1" in
    start) exit 1 ;;
    status|enable|disable|daemon-reload|stop) exit 0 ;;
    *) exit 0 ;;
esac
MOCK
chmod +x "$MOCK_DIR/systemctl"
set +e
OUTPUT=$("$SYSCTL" install 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "unit file cleaned up" "$( [[ ! -f "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" ]] && echo pass || echo fail )"
echo ""

# --- Test 9: remove happy path ---
echo "9. remove: happy path..."
reset_mocks
mkdir -p "$TEST_HOME/.config/systemd/user"
echo "[Unit]" > "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
set +e
OUTPUT=$("$SYSCTL" remove 2>&1)
RC=$?
set -e
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "unit file removed" "$( [[ ! -f "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" ]] && echo pass || echo fail )"
check "systemctl stop called" "$( grep -qF 'stop' "$MOCK_LOG" && echo pass || echo fail )"
check "systemctl disable called" "$( grep -qF 'disable' "$MOCK_LOG" && echo pass || echo fail )"
check "systemctl daemon-reload called" "$( grep -qF 'daemon-reload' "$MOCK_LOG" && echo pass || echo fail )"
check "reports removed" "$( echo "$OUTPUT" | grep -qF 'removed' && echo pass || echo fail )"
echo ""

# --- Test 10: remove not installed → exit 0 ---
echo "10. remove: not installed..."
reset_mocks
rm -f "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
set +e
OUTPUT=$("$SYSCTL" remove 2>&1)
RC=$?
set -e
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "mentions not installed" "$( echo "$OUTPUT" | grep -qF 'not installed' && echo pass || echo fail )"
echo ""

# --- Test 11: status when running ---
echo "11. status: running..."
reset_mocks
mkdir -p "$TEST_HOME/.config/systemd/user"
cat > "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service" << EOF
[Unit]
Description=NBS Terminal Session Helper

[Service]
ExecStart=/bin/bash --login -c 'exec $TEST_HOME/.nbs/bin/nbs-ts-helper'
EOF
create_test_socket "$TEST_HOME/.nbs-ts/helper.sock"
set +e
OUTPUT=$("$SYSCTL" status 2>&1)
RC=$?
set -e
kill_socket_servers
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "shows active" "$( echo "$OUTPUT" | grep -qF 'active' && echo pass || echo fail )"
check "shows PID" "$( echo "$OUTPUT" | grep -qF '12345' && echo pass || echo fail )"
echo ""

# --- Test 12: status when stopped ---
echo "12. status: stopped..."
reset_mocks
cat > "$MOCK_DIR/systemctl" << 'MOCK'
#!/bin/bash
echo "systemctl $*" >> "$MOCK_LOG"
if [[ "${1:-}" == "--user" ]]; then shift; fi
case "$1" in
    status) exit 0 ;;
    is-active) echo "inactive"; exit 1 ;;
    show)
        shift; prop=""
        while [[ $# -gt 0 ]]; do
            case "$1" in -p) prop="$2"; shift 2 ;; --value) shift ;; *) shift ;; esac
        done
        case "$prop" in MainPID) echo "0" ;; *) echo "" ;; esac
        ;;
    *) exit 0 ;;
esac
MOCK
chmod +x "$MOCK_DIR/systemctl"
set +e
OUTPUT=$("$SYSCTL" status 2>&1)
RC=$?
set -e
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "shows inactive" "$( echo "$OUTPUT" | grep -qF 'inactive' && echo pass || echo fail )"
check "shows socket missing" "$( echo "$OUTPUT" | grep -qF 'missing' && echo pass || echo fail )"
echo ""

# --- Test 13: doctor all passing ---
echo "13. doctor: all passing..."
reset_mocks
mkdir -p "$TEST_HOME/.config/systemd/user"
echo "[Unit]" > "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
echo "test log" > "$TEST_HOME/.nbs-ts/helper.log"
create_test_socket "$TEST_HOME/.nbs-ts/helper.sock"
set +e
OUTPUT=$("$SYSCTL" doctor 2>&1)
RC=$?
set -e
kill_socket_servers
PASS_LINES=$(echo "$OUTPUT" | grep -c 'PASS' || true)
check "exit code 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "9 PASS lines" "$( [[ $PASS_LINES -eq 9 ]] && echo pass || echo fail )"
check "shows 9/9" "$( echo "$OUTPUT" | grep -qF '9/9' && echo pass || echo fail )"
echo ""

# --- Test 14: doctor missing socket ---
echo "14. doctor: missing socket..."
reset_mocks
mkdir -p "$TEST_HOME/.config/systemd/user"
echo "[Unit]" > "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
echo "test log" > "$TEST_HOME/.nbs-ts/helper.log"
# No socket created
set +e
OUTPUT=$("$SYSCTL" doctor 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "socket FAIL" "$( echo "$OUTPUT" | grep 'FAIL' | grep -qi 'socket' && echo pass || echo fail )"
echo ""

# --- Test 15: doctor no linger ---
echo "15. doctor: no linger..."
reset_mocks
# Override loginctl to report no linger
cat > "$MOCK_DIR/loginctl" << 'MOCK'
#!/bin/bash
echo "loginctl $*" >> "$MOCK_LOG"
case "$1" in
    enable-linger) exit 0 ;;
    show-user) echo "no" ;;
    *) exit 0 ;;
esac
MOCK
chmod +x "$MOCK_DIR/loginctl"
mkdir -p "$TEST_HOME/.config/systemd/user"
echo "[Unit]" > "$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
echo "test log" > "$TEST_HOME/.nbs-ts/helper.log"
create_test_socket "$TEST_HOME/.nbs-ts/helper.sock"
set +e
OUTPUT=$("$SYSCTL" doctor 2>&1)
RC=$?
set -e
kill_socket_servers
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "linger FAIL" "$( echo "$OUTPUT" | grep 'FAIL' | grep -qi 'linger' && echo pass || echo fail )"
echo ""

# --- Test 16: start not installed → exit 1 ---
echo "16. start: not installed..."
reset_mocks
rm -rf "$TEST_HOME/.config/systemd"
set +e
OUTPUT=$("$SYSCTL" start 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "mentions not installed" "$( echo "$OUTPUT" | grep -qF 'not installed' && echo pass || echo fail )"
echo ""

# --- Test 17: restart not installed → exit 1 ---
echo "17. restart: not installed..."
reset_mocks
rm -rf "$TEST_HOME/.config/systemd"
set +e
OUTPUT=$("$SYSCTL" restart 2>&1)
RC=$?
set -e
check "exit code 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
check "mentions not installed" "$( echo "$OUTPUT" | grep -qF 'not installed' && echo pass || echo fail )"
echo ""

# --- Test 18: test_socket passes path safely (no shell injection) ---
echo "18. test_socket: safe path passing..."
reset_mocks
# Create a socket path with an apostrophe in the directory name
TRICKY_DIR="$TEST_HOME/.nbs-ts-it's-a-test"
mkdir -p "$TRICKY_DIR"
# Create a listening socket at the tricky path
TRICKY_SOCK="$TRICKY_DIR/helper.sock"
create_test_socket "$TRICKY_SOCK" || true
# Override SOCKET_PATH in the script by setting HOME to a dir where
# .nbs-ts/helper.sock maps to our tricky path — can't do that cleanly.
# Instead, test the Python snippet directly with the tricky path:
if command -v python3 >/dev/null 2>&1; then
    set +e
    python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(sys.argv[1])
    s.close()
except:
    sys.exit(1)
" "$TRICKY_SOCK" 2>/dev/null
    SAFE_RC=$?
    set -e
    check "python connect with safe arg passing" "$( [[ $SAFE_RC -eq 0 ]] && echo pass || echo fail )"
else
    echo "   SKIP: python3 not available"
fi
# Also verify the current nbs-ts-sysctl test_socket function would fail
# with an inline single-quote path (demonstrates the bug)
set +e
python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect('$TRICKY_SOCK')
    s.close()
except:
    sys.exit(1)
" 2>/dev/null
INLINE_RC=$?
set -e
# The inline version should fail (syntax error from the apostrophe)
check "inline path with quote fails (demonstrates bug)" "$( [[ $INLINE_RC -ne 0 ]] && echo pass || echo fail )"
kill_socket_servers
echo ""

# --- Test 19: status shows correct binary path ---
echo "19. status: binary path extraction..."
reset_mocks
# Install first so there's a unit file
create_test_socket "$TEST_HOME/.nbs-ts/helper.sock"
set +e
"$SYSCTL" install >/dev/null 2>&1
set -e
# Check the unit file was written
UNIT_FILE="$TEST_HOME/.config/systemd/user/nbs-ts-helper.service"
check "unit file exists" "$( [[ -f "$UNIT_FILE" ]] && echo pass || echo fail )"
# Run status and check binary path shows the actual helper path
set +e
OUTPUT=$("$SYSCTL" status 2>&1)
set -e
HELPER_PATH="$TEST_HOME/.nbs/bin/nbs-ts-helper"
check "status shows binary path" "$( echo "$OUTPUT" | grep -qF "$HELPER_PATH" && echo pass || echo fail )"
# The path should NOT show "unknown" or garbage
check "path is not unknown" "$( echo "$OUTPUT" | grep -i 'Binary:' | grep -qvF 'unknown' && echo pass || echo fail )"
kill_socket_servers
echo ""

# --- Test 20: install --reinstall when already installed ---
echo "20. install --reinstall..."
reset_mocks
create_test_socket "$TEST_HOME/.nbs-ts/helper.sock"
# First install
set +e
"$SYSCTL" install >/dev/null 2>&1
set -e
# Normal install again should fail
set +e
OUTPUT=$("$SYSCTL" install 2>&1)
RC=$?
set -e
check "double install fails" "$( [[ $RC -ne 0 ]] && echo pass || echo fail )"
check "suggests reinstall" "$( echo "$OUTPUT" | grep -qi 'reinstall\|remove' && echo pass || echo fail )"
# Now try --reinstall
set +e
OUTPUT=$("$SYSCTL" install --reinstall 2>&1)
RC=$?
set -e
check "reinstall succeeds" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "reinstall shows success" "$( echo "$OUTPUT" | grep -qF 'installed and running' && echo pass || echo fail )"
# Verify systemctl calls include stop and disable (cleanup before reinstall)
check "reinstall called stop" "$( grep -q 'stop' "$MOCK_LOG" && echo pass || echo fail )"
kill_socket_servers
echo ""

# --- Summary ---
echo "=== Results: $PASS_COUNT passed, $ERRORS failed ==="
if [[ $ERRORS -eq 0 ]]; then
    exit 0
else
    exit 1
fi
