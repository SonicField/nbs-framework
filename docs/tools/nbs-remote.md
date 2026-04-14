# nbs-remote: Remote Development Tools

Four tools for working on remote machines from within the Claude Code sandbox. The sandbox restricts SSH and proxy access, so these tools route through `nbs-ts-helper` — a process running in the user's login environment with full credentials.

## The Tools

### nbs-remote-run

Run a command on a remote machine via SSH. Creates an ephemeral nbs-ts session, connects, runs the command, captures output, cleans up. No session management required.

```
nbs-remote-run <host> '<command>'
nbs-remote-run <host> --cwd=<path> '<command>'
nbs-remote-run <host> --timeout=<secs> '<command>'
```

The command must be a single quoted string. Output goes to stdout; errors to stderr. Default timeout is 300 seconds.

### nbs-remote-edit

Edit files on remote machines via scp. Three-step workflow: pull the file, edit it locally, push it back.

```
nbs-remote-edit pull <host> <remote-path>
nbs-remote-edit push <host> <remote-path>
nbs-remote-edit diff <host> <remote-path>
```

`pull` downloads to `.nbs/remote-edit/<host><path>` and prints the local path. Edit that file with any tool. `push` uploads it back. `diff` shows what changed between local and remote. Remote paths must be absolute.

### nbs-remote-read

Read a file (or part of one) on a remote machine. Quick inspection without staging.

```
nbs-remote-read <host> <remote-path>
nbs-remote-read <host> <remote-path> --head=N
nbs-remote-read <host> <remote-path> --tail=N
nbs-remote-read <host> <remote-path> --lines=M-N
nbs-remote-read <host> <remote-path> --grep=PAT
```

Fetches via scp to a temp file, applies the filter locally, deletes the temp file. The filtering happens on the local machine, not the remote — the entire file is transferred regardless.

### nbs-local-run

Run a command locally but with the user's full login environment — proxy credentials, SSH agent, authenticated remotes. Same mechanism as `nbs-remote-run` but without the SSH hop.

```
nbs-local-run '<command>'
nbs-local-run --timeout=<secs> '<command>'
```

The canonical use case is `git push`, which needs proxy access the sandbox does not have.

## nbs-ts-helper Requirement

All four tools create nbs-ts sessions via `nbs-ts create`. This requires `nbs-ts-helper` to be running in the user's terminal — it is the process that allocates PTYs with the user's credentials. If the helper is not running, session creation fails and the tools exit with an error.

Start the helper: `bin/nbs-ts-helper` in a user terminal. It must remain running for the duration of the work session.

## Security

`nbs-remote-edit` and `nbs-remote-read` validate hostnames (alphanumeric, dots, hyphens, `@`) and require absolute remote paths containing only POSIX-safe characters. This prevents shell injection through crafted hostnames or paths.

The `<host>` argument is an SSH hostname or `user@hostname` — never an nbs-ts handle.

## Exit Codes

All four tools share a common scheme:

| Code | Meaning |
|------|---------|
| 0 | Command ran / transfer succeeded |
| 1 | Connection or general error |
| 2 | Invalid arguments (or file not found for remote-edit/read) |
| 3 | Timeout or transfer failure |

## See Also

- [nbs-ssh-bridge](nbs-ssh-bridge.md) — SSH connection multiplexing (eliminates repeated authentication)
- [nbs-ts](nbs-ts.md) — terminal session manager underlying all remote tools
- [nbs-claude](nbs-claude.md) — agent launcher that sets up the environment
