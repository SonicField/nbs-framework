#!/usr/bin/env python3
"""
Test harness: run nbs-chat-remote tests against MockSSHServer.

Starts an in-process SSH server (from nbs-ssh) with key-based auth and
real command execution, then runs the bash test suite against it. This
allows full integration testing on machines where ssh localhost is blocked
(e.g. by BpfJailer or corporate security policies).

Prerequisites:
  - nbs-ssh installed: pip install -e ~/local/nbs-ssh
    (or: ~/local/nbs-ssh/venv/bin/python this_script.py)
  - nbs-chat and nbs-chat-remote built and installed in bin/

Usage:
  ~/local/nbs-ssh/venv/bin/python tests/automated/test_nbs_chat_remote_mock.py
"""
from __future__ import annotations

import asyncio
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Ensure nbs-ssh is importable
NBS_SSH_SRC = Path.home() / "local" / "nbs-ssh" / "src"
if NBS_SSH_SRC.exists():
    sys.path.insert(0, str(NBS_SSH_SRC))


async def main() -> int:
    """Start MockSSHServer and run the bash test suite against it."""
    try:
        from nbs_ssh.testing.mock_server import MockServerConfig, MockSSHServer
        import asyncssh
    except ImportError as e:
        print(f"Error: nbs-ssh not available: {e}", file=sys.stderr)
        print("Install: pip install -e ~/local/nbs-ssh", file=sys.stderr)
        print("Or run: ~/local/nbs-ssh/venv/bin/python", sys.argv[0], file=sys.stderr)
        return 1

    # Locate project root and binaries
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent.parent
    bin_dir = project_root / "bin"
    nbs_chat = bin_dir / "nbs-chat"
    nbs_remote = bin_dir / "nbs-chat-remote"

    for binary in [nbs_chat, nbs_remote]:
        if not binary.exists():
            print(f"Error: {binary} not found. Run 'make install' first.", file=sys.stderr)
            return 1

    # Get current username for SSH auth
    username = os.environ.get("USER", "test")

    with tempfile.TemporaryDirectory(prefix="nbs_ssh_test_") as tmpdir:
        tmpdir_path = Path(tmpdir)

        # Generate temporary SSH key pair
        key_path = tmpdir_path / "test_key"
        key = asyncssh.generate_private_key("ssh-rsa", key_size=2048)
        key.write_private_key(str(key_path))
        key.write_public_key(str(key_path.with_suffix(".pub")))
        os.chmod(str(key_path), 0o600)

        # Get public key for server authorisation
        pub_key = key.export_public_key()

        # Configure MockSSHServer with key auth and real command execution
        config = MockServerConfig(
            username=username,
            password="unused",
            authorized_keys=[pub_key],
            execute_commands=True,
        )

        print(f"Starting MockSSHServer (user={username}, key auth, exec mode)...")
        async with MockSSHServer(config) as server:
            print(f"MockSSHServer listening on localhost:{server.port}")

            # Build environment for the bash test suite
            env = os.environ.copy()
            env["NBS_CHAT_HOST"] = f"{username}@localhost"
            env["NBS_CHAT_PORT"] = str(server.port)
            env["NBS_CHAT_KEY"] = str(key_path)
            env["NBS_CHAT_BIN"] = str(nbs_chat)
            env["NBS_CHAT_OPTS"] = "StrictHostKeyChecking=no,UserKnownHostsFile=/dev/null"

            # Run the bash test suite
            test_script = script_dir / "test_nbs_chat_remote.sh"
            assert test_script.exists(), f"Test script not found: {test_script}"

            print(f"Running {test_script}...")
            print()

            result = subprocess.run(
                ["bash", str(test_script)],
                env=env,
                timeout=120,
            )

            return result.returncode


if __name__ == "__main__":
    rc = asyncio.run(main())
    sys.exit(rc)
