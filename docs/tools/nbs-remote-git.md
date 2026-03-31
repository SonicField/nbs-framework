# nbs-remote-git: Git Sync Between Machines

Synchronise git repositories between this machine and a remote host via SSH. Uses `nbs-local-run` for SSH credentials — the nbs-ts-helper must be running.

## Usage

```bash
nbs-remote-git <host> <remote-path> <command> [args]
```

## Commands

### clone

Clone a repository from a remote machine to a local path.

```bash
nbs-remote-git <host> <remote-path> clone <local-path>
```

**Example:**
```bash
nbs-remote-git devgpu004.kcm2.facebook.com /data/users/alexturner/cinderx_dev/cinderx clone /local/phoenix-cpython
```

### push

Push local commits to the remote repository. Adds a git remote on first use.

```bash
nbs-remote-git <host> <remote-path> push [branch]
```

Must be run from inside a local git repository. The branch defaults to HEAD if not specified.

**Example:**
```bash
cd /local/phoenix-cpython
nbs-remote-git devgpu004.kcm2.facebook.com /data/users/alexturner/cinderx_dev/cinderx push phoenix
```

### pull

Fetch (or pull a specific branch) from the remote repository. Adds a git remote on first use.

```bash
nbs-remote-git <host> <remote-path> pull [branch]
```

Without a branch argument, runs `git fetch`. With a branch, runs `git pull`.

**Example:**
```bash
cd /local/phoenix-cpython
nbs-remote-git devgpu004.kcm2.facebook.com /data/users/alexturner/cinderx_dev/cinderx pull main
```

### status

Show the remote repository's recent commits and working tree status.

```bash
nbs-remote-git <host> <remote-path> status
```

Uses `nbs-remote-run` (not `nbs-local-run`) because it runs git on the remote machine.

**Example:**
```bash
nbs-remote-git devgpu004.kcm2.facebook.com /data/users/alexturner/cinderx_dev/cinderx status
```

## How It Works

- `clone`, `push`, and `pull` wrap git commands in `nbs-local-run`, which provides SSH credentials through the nbs-ts-helper daemon.
- `status` uses `nbs-remote-run` to execute git commands on the remote machine directly.
- On first `push` or `pull`, the tool adds a git remote named `remote-<hostname>` (e.g. `remote-devgpu004`). Subsequent operations reuse it.

## Prerequisites

- **nbs-ts-helper must be running.** Without it, SSH fails. Start it with `nbs-ts-helper` in a user terminal or `nbs-ts-sysctl start`.
- **The remote path must be a git repository.** For `clone`, it must exist on the remote. For `push`, the remote repo must accept pushes (not a bare checkout with uncommitted changes on the target branch).

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Operation failed (git error, SSH failure, not a repo) |
| 4 | Invalid arguments |

## When to Use

Use `nbs-remote-git` when you need to synchronise code between this machine and a remote build/test machine. Do not use `nbs-remote-edit` for this — `nbs-remote-edit` is for individual files, `nbs-remote-git` is for whole repositories.

If you only need to run a command on the remote machine without syncing code, use `nbs-remote-run` instead.

## Troubleshooting

**"helper connect failed"** — the nbs-ts-helper is not running. Start it.

**"does not appear to be a git repository"** — the remote path is wrong. Check it with `nbs-remote-run <host> "ls -la <path>/.git"`.

**"Permission denied"** — SSH credentials are not available. Restart the helper.
