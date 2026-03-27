# nbs-ts-sysctl — Service Management for nbs-ts-helper

Manages the `nbs-ts-helper` daemon as a systemd user service: install, lifecycle, diagnostics, and removal.

## Why

`nbs-ts-helper` is a long-running daemon that allocates PTYs for nbs-ts sessions via Unix socket fd passing. Without it, agents cannot use SSH, proxy, or git push. Previously it had to be started manually; `nbs-ts-sysctl` automates this via systemd.

## Requirements

- systemd user session (`systemctl --user` must work)
- `nbs-ts-helper` binary installed (typically at `~/.nbs/bin/nbs-ts-helper`)
- `loginctl enable-linger` for persistence beyond login sessions (optional but recommended)

## Subcommands

### install

Installs and starts the systemd user service.

```bash
nbs-ts-sysctl install
```

What it does:
1. Verifies systemd user session is available
2. Locates the `nbs-ts-helper` binary
3. Writes the unit file to `~/.config/systemd/user/nbs-ts-helper.service`
4. Enables linger (warns if unavailable)
5. Reloads systemd, enables, and starts the service
6. Waits up to 15 seconds for the socket to appear and verifies connectivity

On failure: cleans up the unit file — no half-installed state.

### remove

Stops and removes the service.

```bash
nbs-ts-sysctl remove
```

Safe to run when not installed (exits 0).

### start / stop / restart

Standard lifecycle commands.

```bash
nbs-ts-sysctl start
nbs-ts-sysctl stop
nbs-ts-sysctl restart   # Use after rebuilding the helper binary
```

`start` and `restart` wait for the socket to appear before reporting success.

### status

Shows current state, PID, uptime, socket, and recent log lines.

```bash
nbs-ts-sysctl status
```

### doctor

Runs 9 health checks with PASS/FAIL and actionable fix instructions.

```bash
nbs-ts-sysctl doctor
```

Checks:
1. systemd user session available
2. Unit file installed
3. Service enabled
4. Service running
5. Socket exists
6. Socket connectable
7. Helper binary exists
8. Linger enabled
9. Log file writable

### help

Shows usage text.

```bash
nbs-ts-sysctl help
```

## The systemd Unit File

```ini
[Unit]
Description=NBS Terminal Session Helper

[Service]
ExecStart=/bin/bash --login -c 'exec ~/.nbs/bin/nbs-ts-helper'
Restart=on-failure
RestartSec=2
TimeoutStartSec=30
StandardOutput=append:~/.nbs-ts/helper.log
StandardError=append:~/.nbs-ts/helper.log

[Install]
WantedBy=default.target
```

Key design choices:

- **`bash --login -c 'exec ...'`** — The helper inherits the user's full login environment (PATH, proxy, SSH agent). Child processes must behave identically to processes launched from a login shell. The `exec` replaces bash so systemd tracks the correct PID.
- **`Restart=on-failure`** — Automatic restart on crash, but not on clean exit (stop command).
- **`RestartSec=2`** — Short delay to avoid tight crash loops.
- **`TimeoutStartSec=30`** — Allows time for login profile to load before systemd considers startup failed.
- **`WantedBy=default.target`** — Starts on user login.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Operation failed (with diagnostic message) |
| 4 | Invalid arguments / unknown subcommand |

## Environment Variables

| Variable | Description |
|----------|-------------|
| `NBS_TS_HELPER_BIN` | Override the helper binary path (skips auto-detection) |

## File Locations

| File | Purpose |
|------|---------|
| `~/.config/systemd/user/nbs-ts-helper.service` | Unit file |
| `~/.nbs-ts/helper.sock` | Unix socket |
| `~/.nbs-ts/helper.log` | Log file |
| `~/.nbs/bin/nbs-ts-helper` | Default binary location |

## Linger

Without linger, the helper stops when you log out. Enable it:

```bash
loginctl enable-linger $USER
```

`nbs-ts-sysctl install` attempts this automatically and warns if it fails.

## Troubleshooting

**Service won't start:** Run `nbs-ts-sysctl doctor` for diagnostics. Check `~/.nbs-ts/helper.log` for errors.

**Socket missing after start:** The helper may have crashed. Check `journalctl --user -u nbs-ts-helper` and the log file.

**Binary updated (make install):** Run `nbs-ts-sysctl restart` to pick up the new binary.

**No systemd (containers/chroots):** Use manual `nbs-ts-helper` instead. Run it in a terminal or tmux session.

**Permission denied on socket:** Ensure only one user runs the helper. The socket enforces UID matching.

## Manual Usage

`nbs-ts-helper` still works without systemd. Start it in a terminal or tmux:

```bash
nbs-ts-helper
```

This is useful on systems without systemd or for debugging.
