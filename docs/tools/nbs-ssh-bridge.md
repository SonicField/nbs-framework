# nbs-ssh-bridge: SSH Connection Multiplexing

Drop-in replacement for `ssh` and `scp` that configures and reuses a single master connection per host. The first connection authenticates; subsequent connections to the same `user@host:port` piggyback on it without re-authentication.

## Usage

```bash
nbs-ssh-bridge [ssh-options] destination
nbs-scp-bridge [scp-options] source... target
```

`nbs-scp-bridge` is a symlink to `nbs-ssh-bridge`. The script detects its invocation name and routes to `ssh` or `scp` accordingly.

## What It Does

On first invocation, `setup_config` runs once:

1. Creates `~/.ssh/sockets/` (mode 700) for Unix domain sockets
2. Creates `~/.ssh/conf.d/multiplexing.conf` with `ControlMaster auto` and `ControlPath ~/.ssh/sockets/%r@%h-%p`
3. Injects `Include conf.d/*` at the top of `~/.ssh/config` (before any `Host`/`Match` blocks — SSH requires this ordering)

After setup, it calls `exec ssh "$@"` (or `exec scp "$@"`). SSH's built-in ControlMaster handling does the rest: the first connection becomes the master, and subsequent connections reuse its socket.

## Master Lifecycle

The master connection persists as long as the originating shell session. When the session ends, the socket is cleaned up automatically by SSH.

To manage masters manually:

```bash
ssh -O check user@host    # is a master running?
ssh -O exit  user@host    # tear it down
```

## Relationship to nbs-remote Tools

`nbs-remote-run`, `nbs-remote-edit`, and `nbs-local-run` route SSH through `nbs-ts-helper` — a daemon that provides credentials to the Claude Code sandbox. `nbs-ssh-bridge` operates at a different layer: it optimises SSH itself by eliminating repeated authentication handshakes.

The two are complementary. When `nbs-ts-helper` establishes SSH connections on behalf of sandbox tools, those connections benefit from multiplexing if `nbs-ssh-bridge` has configured ControlMaster. When working outside the sandbox (direct terminal use), `nbs-ssh-bridge` provides the same benefit without requiring the helper.

## Installation

```bash
# The script is installed to bin/ by make install
# Create the scp symlink:
ln -sf nbs-ssh-bridge bin/nbs-scp-bridge
```

## Prerequisites

- OpenSSH (`ssh`, `scp`)
- `~/.ssh/` must exist or be creatable

No dependency on `nbs-ts-helper`. This tool works standalone.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Connection succeeded / transfer completed |
| 1 | No arguments provided (usage printed) |
| * | Passes through `ssh`/`scp` exit codes |

## See Also

- [Remote Tools](nbs-remote.md) — sandbox-aware remote development tools
- [Remote Git](nbs-remote-git.md) — repository sync between machines
- [nbs-ts](nbs-ts.md) — terminal session manager
