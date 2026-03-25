# nbs-oracle-reaper: Oracle Cleanup

Oracles — pythia, librarian, shepard, fixup — are ephemeral. They spawn, do their work, post to chat, and should die. The reaper ensures they do.

## Design

Stateless. No state files, no counters, no memory between invocations. Every run discovers the world from scratch:

1. List alive `nbs-ts` sessions whose names match oracle role prefixes (`nbs-pythia-`, `nbs-librarian-`, etc.).
2. Read each session's meta file for its start timestamp.
3. Search the chat file for posts by that oracle role after that timestamp.
4. If a post is found, the oracle has done its job. Kill the session.

The sidecar calls `nbs-oracle-reaper check` every 10 seconds as fire-and-forget. Multiple sidecars may call it concurrently — the stateless design makes this safe. Killing an already-dead session is a no-op.

## Usage

```
nbs-oracle-reaper check <project-root>
```

There is one command. The project root is used to locate the chat file via the supervisor's control registry.

## Timing

| Parameter | Value | Purpose |
|-----------|-------|---------|
| Grace | 30s | Do not check sessions younger than this — the oracle may not have posted yet |
| Timeout | 600s | Kill any oracle session older than 10 minutes, regardless of chat post |
| Settle | 10s | Wait after detecting a post before killing — lets the oracle finish cleanup |

Grace prevents false negatives (checking before the oracle has had time to work). Timeout prevents leaked sessions from accumulating indefinitely. Settle prevents killing an oracle mid-write.

## Chat Post Detection

The reaper uses `nbs-chat search` with the oracle's role as handle and the session start time as the `--after` filter. If any message is found, the oracle has posted. The reaper does not parse the message content — existence is sufficient.

## Roles

The reaper knows four oracle roles: `pythia`, `shepard`, `librarian`, `fixup`. These are hardcoded. Adding a new oracle role requires editing the script.

## See Also

- [nbs-sidecar](nbs-sidecar.md) — invokes the reaper on its 10-second tick
- [nbs-ts](nbs-ts.md) — session manager providing session listing and kill
- [nbs-chat](nbs-chat.md) — chat search used for post detection
