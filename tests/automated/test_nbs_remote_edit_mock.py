#!/usr/bin/env python3
"""
Test harness: run nbs-remote-edit tests against MockSSHServer.

Starts an in-process SSH server (from nbs-ssh) with key-based auth and
real command execution, then runs the bash test suite against it.

Prerequisites:
  - nbs-ssh installed: pip install -e ~/local/nbs-ssh
    (or: ~/local/nbs-ssh/venv/bin/python this_script.py)

Usage:
  ~/local/nbs-ssh/venv/bin/python tests/automated/test_nbs_remote_edit_mock.py
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
        return 1

    script_dir = Path(__file__).resolve().parent
    username = os.environ.get("USER", "test")

    with tempfile.TemporaryDirectory(prefix="nbs_remote_edit_test_") as tmpdir:
        tmpdir_path = Path(tmpdir)

        # Generate temporary SSH key pair
        key_path = tmpdir_path / "test_key"
        key = asyncssh.generate_private_key("ssh-rsa", key_size=2048)
        key.write_private_key(str(key_path))
        key.write_public_key(str(key_path.with_suffix(".pub")))
        os.chmod(str(key_path), 0o600)

        pub_key = key.export_public_key()

        config = MockServerConfig(
            username=username,
            password="unused",
            authorized_keys=[pub_key],
            execute_commands=True,
        )

        print(f"Starting MockSSHServer (user={username}, key auth, exec mode)...")
        async with MockSSHServer(config) as server:
            print(f"MockSSHServer listening on localhost:{server.port}")

            staging_dir = tmpdir_path / "staging"
            staging_dir.mkdir()

            env = os.environ.copy()
            env["NBS_REMOTE_EDIT_HOST"] = f"{username}@localhost"
            env["NBS_REMOTE_EDIT_PORT"] = str(server.port)
            env["NBS_REMOTE_EDIT_KEY"] = str(key_path)
            env["NBS_REMOTE_EDIT_DIR"] = str(staging_dir)

            test_script = script_dir / "test_nbs_remote_edit.sh"
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
