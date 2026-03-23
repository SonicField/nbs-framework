#!/usr/bin/env python3
"""Prototype: pass a PTY fd from outside the sandbox to inside it.

Run TWO terminals:

Terminal 1 (your login shell, outside sandbox):
  python3 prototype_fd_pass.py server

Terminal 2 (Claude's sandbox, inside sandbox):
  python3 prototype_fd_pass.py client 'ssh devgpu004.kcm2.facebook.com hostname'

If the hostname prints, fd passing works across cgroups.
"""

import os
import sys
import pty
import socket
import array
import struct
import subprocess

SOCK_PATH = os.path.expanduser("~/.nbs-ts-prototype.sock")


def send_fd(sock, fd):
    """Send a file descriptor over a Unix socket via SCM_RIGHTS."""
    fds = array.array("i", [fd])
    sock.sendmsg(
        [b"FD"],
        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)]
    )


def recv_fd(sock):
    """Receive a file descriptor over a Unix socket via SCM_RIGHTS."""
    fds = array.array("i", [0])
    msg, ancdata, flags, addr = sock.recvmsg(
        3,  # "FD\0" or "FD"
        socket.CMSG_LEN(fds.itemsize)
    )
    for cmsg_level, cmsg_type, cmsg_data in ancdata:
        if cmsg_level == socket.SOL_SOCKET and cmsg_type == socket.SCM_RIGHTS:
            fds.frombytes(cmsg_data[:fds.itemsize])
            return fds[0]
    raise RuntimeError("No fd received")


def server():
    """Run outside the sandbox. Creates PTYs and passes master fd back."""
    if os.path.exists(SOCK_PATH):
        os.unlink(SOCK_PATH)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(SOCK_PATH)
    s.listen(1)
    print(f"Helper listening on {SOCK_PATH}")
    print(f"My cgroup: {open('/proc/self/cgroup').read().strip()}")

    while True:
        conn, _ = s.accept()
        # Read command
        cmd = conn.recv(4096).decode().strip()
        print(f"Request: {cmd}")

        # Create PTY, fork, exec
        master_fd, slave_fd = pty.openpty()
        pid = os.fork()
        if pid == 0:
            # Child
            os.close(master_fd)
            os.setsid()
            os.dup2(slave_fd, 0)
            os.dup2(slave_fd, 1)
            os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
            os.execlp("bash", "bash", "-c", cmd)
            os._exit(127)

        # Parent — send master fd to client
        os.close(slave_fd)
        send_fd(conn, master_fd)
        os.close(master_fd)
        conn.close()
        print(f"Sent fd for pid {pid}")


def client(cmd):
    """Run inside the sandbox. Receives PTY fd and reads output."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)
    print(f"My cgroup: {open('/proc/self/cgroup').read().strip()}")

    # Send command
    s.send(cmd.encode())

    # Receive master fd
    master_fd = recv_fd(s)
    s.close()
    print(f"Received fd {master_fd}")

    # Read output
    import select
    output = b""
    while True:
        r, _, _ = select.select([master_fd], [], [], 5)
        if not r:
            break
        try:
            data = os.read(master_fd, 4096)
            if not data:
                break
            output += data
        except OSError:
            break

    os.close(master_fd)
    print(f"Output:\n{output.decode(errors='replace')}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == "server":
        server()
    elif sys.argv[1] == "client":
        cmd = sys.argv[2] if len(sys.argv) > 2 else "echo hello"
        client(cmd)
    else:
        print(__doc__)
        sys.exit(1)
