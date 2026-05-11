# nbs-sidecar: Session Monitor

A background process that watches a Claude Code session and injects notifications when something needs attention. One sidecar per agent. No UI, no human interaction — it reads terminal output, detects idle prompts, and types into the session on the agent's behalf.

## Lifecycle

`nbs-claude` creates an nbs-ts session, then forks a sidecar via `setsid` so it survives shell exits. The sidecar receives the agent's handle, the project root, and the nbs-ts session handle. It ignores SIGHUP and SIGPIPE, redirects stderr to a log file, validates its config, initialises the transport, and enters the main loop.

On startup, the sidecar seeds the agent's resource registry from existing `.nbs/` resources (chat files, event directories). If an initial prompt is configured, it waits up to 60 seconds for the Claude prompt to appear, then injects it. This wait is non-blocking — queries and interrupts are processed in parallel.

## The Main Loop

One-second tick. Each iteration:

1. **Pause check.** If `.nbs/control-pause` exists, sleep 5 seconds, skip everything. Created by `/pause`, deleted by `/resume`.
2. **State invariant assertions.** Verify idle_seconds, bus_check_counter, and notify_fail_count are non-negative.
3. **Control inbox.** Process new lines from `.nbs/control-inbox-<handle>` (register/unregister resources).
4. **Query check.** Look for `chat-query` bus events targeting this handle. If found, capture terminal output, strip ANSI, take the last 16 non-blank lines (truncated to 80 chars each), sanitise `@` signs, and post to the first registered chat.
5. **Initial prompt injection.** If pending, check for prompt and inject. 60-second deadline.
6. **Interrupt check.** Look for `chat-interrupt` events (handle-specific or `@team`). Send Escape every 10 seconds for up to 60 seconds. On prompt detection, inject the interrupt message. On failure, post URGENT to chat.
7. **Mention check.** Store `chat-mention` events (handle or `@team`) for later injection. Ack immediately.
8. **Transport alive check.** If the session process is dead, exit.
9. **Content capture and hash.** Read last 30 lines of output. Hash with FNV-1a. If changed, reset idle counters, check for blocking dialogues.
10. **Idle processing.** If content stable: increment idle counter, check blocking dialogues, check bus events on interval, decide whether to inject notification.

## Notification Injection

Notifications are plain text prompts, not slash commands. The sidecar types directly into the terminal:

```
Hey, you have messages to read in your chat.
```

This replaced `/nbs-notify` slash command injection, which failed because Enter does not reliably register in terminal contexts.

## Timing

| Parameter | Default | Description |
|-----------|---------|-------------|
| `NBS_BUS_CHECK_INTERVAL` | 3s | Seconds of idle before checking bus/chat |
| `NBS_NOTIFY_COOLDOWN` | 15s | Minimum gap between notifications |
| `NBS_STARTUP_GRACE` | 30s | Silence period after start |
| `NBS_FLUSH_INTERVAL` | 60s | Wall-clock Enter flush (prevents stuck UI) |
| `NBS_POLL_INTERVAL` | 0 (off) | Legacy `/nbs-poll` safety net |

Critical-priority bus events and non-sidecar mentions bypass the cooldown. Sidecar-originated mentions (payload contains "from sidecar:") do not bypass — this prevents O(N^2) notification storms from `@team` messages.

## Periodic Triggers

Four triggers share a common pattern: shared timestamp file for cross-sidecar deduplication, lock-guarded fork+exec via `nbs-workers`.

| Trigger | Env (minutes) | Default | First delay |
|---------|--------------|---------|-------------|
| librarian | `NBS_LIBRARIAN_INTERVAL` | 15 | 5 min |
| shepard | `NBS_SHEPARD_INTERVAL` | 20 | 10 min |
| pythia | `NBS_PYTHIA_INTERVAL` | 30 | 10 min |
| fixup | `NBS_FIXUP_INTERVAL` | 3600s | 10 min |

Checked once per minute (not every tick) to limit file I/O. The shared timestamp file means only one sidecar fires each trigger, regardless of team size.

## Oracle Reaper

Every 10 seconds, the sidecar runs `nbs-oracle-reaper check` as fire-and-forget. Stateless — no accumulated state, no coordination with other sidecars. Kills oracle workers that have posted their result to chat and should be cleaned up.

## Blocking Dialogue Detection

When Claude Code shows a numbered options prompt (plan mode, permission requests), the sidecar detects it and automatically responds. Checked on every content change and during idle periods. After responding, it sleeps for a settle period and resets the content hash.

## Transport Abstraction

The sidecar uses a vtable (`transport_t`) with four operations: `capture`, `send_text`, `send_key`, `is_alive`. The only implementation is `nbs-ts`:

- **capture**: reads the tail of `~/.nbs-ts/sessions/<handle>/output.log` via `pread`
- **send_text**: writes to `input.fifo` with bracketed paste markers
- **send_key**: writes raw bytes (`\r` for Enter, `\x1b` for Escape) to `input.fifo`
- **is_alive**: reads PID from `pid` file, checks with `kill(pid, 0)`

No fork+exec for any transport operation. All syscalls against session files.

## Context Stress

If the terminal output contains signs of context window pressure, the sidecar backs off: resets idle counters, sleeps 30 seconds. No notifications are injected during context stress.

## See Also

- [nbs-claude](nbs-claude.md) — launches the sidecar
- [nbs-bus](nbs-bus.md) — event queue the sidecar monitors
- [nbs-ts](nbs-ts.md) — terminal session manager providing the transport
